#pragma once

// Persistent prefix / KV-state cache for llama-server (Stage 2 + Stage 3 of
// llama-disk-cache/ARCHITECTURE.md). Lives entirely inside llama.cpp - no external proxy.
//
// On-disk layout of the cache root (the root is shared by all namespaces):
//
//   <root>/<model-slug>-<fp12>/
//       manifest.json          namespace descriptor + all fingerprint components
//       index.bin              compact binary index (little-endian, crc32-protected header)
//       states/<id>.meta       self-describing header + the exact token array of the entry
//       states/<id>.bin        payload: [target seq-state][dft][ckpt][spec]
//       states/*.tmp           incomplete writes; ignored and deleted when the namespace opens
//
// The size limit applies to the WHOLE cache root (all namespaces), but a namespace only ever
// reads/sums and evicts its own files - other namespaces are left untouched.
//
// Stage 2: namespace + fingerprint + manifest + index + atomic writes.
// Stage 3: streamed save/load of the sequence state (no multi-GB host buffer for the target state).
// Stage 4-6 (slot selection, cost model, eviction/concurrency policy) plug in on top of this API.

#include "llama.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define SERVER_DISK_CACHE_FORMAT_VERSION 1

// index/entry flags
#define SERVER_DISK_CACHE_FLAG_HAS_DFT  (1u << 0)
#define SERVER_DISK_CACHE_FLAG_HAS_CKPT (1u << 1)
#define SERVER_DISK_CACHE_FLAG_HAS_SPEC (1u << 2)

// rolling prefix hash: h = h * PRIME + (uint32_t) token (odd prime - deterministic, one pass)
#define SERVER_DISK_CACHE_HASH_PRIME 0x100000001B3ull

//
// fingerprint (namespace identity)
//

// everything that is part of the state layout; changing any of it invalidates the namespace
struct server_disk_cache_fp_params {
    // model identity (GGUFKV)
    std::string arch;
    std::string name;
    std::string model_path; // not part of the hash, recorded in the manifest only
    std::string file_type;
    std::string quantization_version;
    std::string size_label;
    std::string model_desc;
    // hparams
    int32_t n_layer   = 0;
    int32_t n_embd    = 0;
    int32_t n_head    = 0;
    int32_t n_head_kv = 0;

    // runtime configuration that ends up in the state layout
    std::string type_k;
    std::string type_v;
    std::string flash_attn;
    bool        kv_unified = false;
    bool        swa_full   = false;

    int32_t n_ctx_checkpoints   = 0;
    int32_t checkpoint_min_step = 0;

    std::string spec_type;

    // applied adapters, "path@sha256" (sha256 empty when the file could not be hashed)
    std::vector<std::string> loras;
};

struct server_disk_cache_fingerprint {
    std::string hex;        // full sha256 hex (64 chars) of the canonical component string
    std::string slug;       // sanitized model slug
    std::string dir_name;   // "<slug>-<fp12>"
    std::string model_path; // recorded in the manifest, not hashed
    std::string model_desc; // recorded in the manifest, not hashed
    std::vector<std::pair<std::string, std::string>> components; // ordered, hash input
};

// fills the model-derived part (GGUFKV + hparams) from a loaded model
void server_disk_cache_fp_model(const llama_model * model, const std::string & model_path, server_disk_cache_fp_params & out);

// canonicalizes the components and hashes them
server_disk_cache_fingerprint server_disk_cache_fp_make(const server_disk_cache_fp_params & params);

// sha256 of a file as lowercase hex, streamed in chunks; empty string on error
std::string server_disk_cache_sha256_file(const std::string & path);

//
// config
//

struct server_disk_cache_config {
    std::string root;                                            // --cache-disk
    uint64_t    size_limit_bytes = 500ull * 1024 * 1024 * 1024;  // --cache-disk-size (whole root)
    int32_t     read_mbps        = 200;                          // --cache-disk-read-mbps (decimal MB/s)
    int32_t     min_gain_ms      = 1000;                         // --cache-disk-min-gain-ms
    int32_t     min_tokens       = 1024;                         // --cache-disk-min-tokens: shorter prompts are never written
    int32_t     prefill_tps      = 0;                            // --cache-prefill-tps (0 = auto)
};

//
// index / payload records
//

// one payload section that is not the target seq-state (draft / checkpoint / spec)
struct server_disk_cache_blob {
    std::vector<uint8_t> data;
    uint64_t n_tokens = 0;
    uint64_t pos_min  = 0;
    uint64_t pos_max  = 0;
};

struct server_disk_cache_extra {
    std::vector<server_disk_cache_blob> dft;   // 0 or 1
    std::vector<server_disk_cache_blob> ckpt;  // context checkpoints
    std::vector<server_disk_cache_blob> spec;  // 0 or 1
};

struct server_disk_cache_entry {
    uint64_t id               = 0;
    uint64_t n_tokens         = 0;
    uint64_t hash_prefix_full = 0;
    uint64_t payload_bytes    = 0;
    uint64_t main_bytes       = 0;
    uint64_t dft_bytes        = 0;
    uint64_t ckpt_bytes       = 0;
    uint64_t spec_bytes       = 0;
    uint64_t last_used_unix   = 0;
    uint64_t created_unix     = 0;
    uint32_t hit_count        = 0;
    uint32_t flags            = 0;
};

struct server_disk_cache_candidate {
    uint64_t id            = 0;
    uint64_t n_tokens      = 0;
    uint64_t lcp           = 0; // how much of the request this entry can serve
    uint64_t payload_bytes = 0;
};

uint64_t server_disk_cache_hash_step(uint64_t h, llama_token token);
uint64_t server_disk_cache_hash_prefix(const std::vector<llama_token> & tokens);

//
// Stage 4+5: the prefix candidate and the cost model
//

// where a candidate state comes from; the server compares exactly two of them:
//   RESIDENT - what the slot holds right now (this includes a state just taken from the RAM cache)
//   DISK     - an entry of this namespace that is an exact prefix of the request
enum server_prefix_source {
    SERVER_PREFIX_SOURCE_RESIDENT = 0,
    SERVER_PREFIX_SOURCE_DISK     = 1,
};

struct server_prefix_candidate {
    server_prefix_source source         = SERVER_PREFIX_SOURCE_RESIDENT;
    uint64_t             state_id       = 0; // DISK only: entry id inside the namespace
    size_t               n_tokens_state = 0; // tokens covered by the state
    size_t               n_tokens_match = 0; // exact prefix match with the request (== n_tokens_state)
    uint64_t             restore_bytes  = 0; // bytes that have to be read before the tail can be decoded

    bool   t_estimate_valid = false; // false while the prefill throughput is not known
    double t_restore_est    = 0.0;   // seconds
    double t_tail_est       = 0.0;   // seconds
    double t_total_est      = 0.0;   // seconds
};

// POLICY CONSTANTS - not measurements of any stand:
// the disk path must win by this margin (hysteresis against estimation noise)
#define SERVER_DISK_CACHE_COST_MARGIN 0.05
// fixed per-restore overhead of the disk path. NOT MEASURED yet (Stage 7) - it stays 0 so that it
// cannot hide a real cost; measure it before setting it to anything else
#define SERVER_DISK_CACHE_T_RESTORE_FIXED_SEC 0.0
// conservative prefill throughput used ONLY while nothing has been observed yet (policy floor)
#define SERVER_DISK_CACHE_TPS_FLOOR 20.0
// a prompt evaluation must cover at least this many tokens before its rate feeds the EMA: a prompt
// that was almost fully restored decodes a token or two, and such a rate is pure noise
#define SERVER_DISK_CACHE_TPS_MIN_TOKENS 32

struct server_disk_cache_cost {
    // EMA (alpha = 0.2) of the prefill throughput this server has actually observed; 0 = unknown
    double   ema_tps = 0.0;
    double   alpha   = 0.2;
    uint64_t n_obs   = 0;

    // one observation of a real prompt evaluation
    void observe(size_t n_tokens, double t_seconds);

    // effective prefill throughput: the CLI override wins over the EMA; 0 = unknown
    double tps(int32_t cfg_prefill_tps) const;
};

// fills t_restore_est / t_tail_est / t_total_est; `tps` = 0 means "unknown"
void server_prefix_candidate_estimate(server_prefix_candidate & cand, size_t n_prompt,
                                      double tps, double read_bps);

// the Stage 5 decision rule (PLAN-STAGE4-6.md 4.3); `reason` is always filled, for the log
bool server_prefix_candidate_prefer_disk(const server_prefix_candidate & disk,
                                         const server_prefix_candidate & resident,
                                         size_t n_prompt, bool tps_known, int32_t min_gain_ms,
                                         std::string * reason);

//
// the cache
//

class server_disk_cache {
public:
    server_disk_cache(const server_disk_cache_config & cfg, const server_disk_cache_fingerprint & fp);
    ~server_disk_cache();

    server_disk_cache(const server_disk_cache &)             = delete;
    server_disk_cache & operator=(const server_disk_cache &) = delete;

    // false when the namespace could not be created/opened; the caller then runs without disk cache
    bool ok() const;

    const server_disk_cache_fingerprint & fp()      const;
    const server_disk_cache_config &      config()  const;
    const std::string &                   dir()     const;

    size_t   n_entries() const;
    uint64_t n_bytes()   const;  // sum of payload_bytes over the entries of this namespace
    uint64_t root_bytes() const; // size of the whole cache root as seen at open/top-up time

    // cumulative counters of this namespace (stage 7: the server scrapes them instead of guessing
    // from n_entries(), which stays flat when a save immediately triggers an eviction)
    uint64_t n_new_writes_total() const;
    uint64_t n_evictions_total()  const;

    // candidate search: one pass over the request tokens (rolling hash), then the short list is
    // verified against the exact token array from states/<id>.meta; only an entry whose tokens are
    // an exact prefix of `tokens` is usable, and then LCP = entry.n_tokens
    bool find(const std::vector<llama_token> & tokens, server_disk_cache_candidate & out) const;

    // Stage 7: the entry that shares the longest common prefix with the request. Unlike find(),
    // the entry does not have to be a prefix of the request - it may be longer.
    bool find_best(const std::vector<llama_token> & tokens, server_disk_cache_candidate & out) const;

    // exact token array of an entry, read from states/<id>.meta
    bool tokens_of(uint64_t id, std::vector<llama_token> & out) const;

    // index record of an entry (no payload read); false when the entry is unknown
    bool entry_info(uint64_t id, server_disk_cache_entry & out) const;

    // Stage 3: write the target sequence state straight into the payload file (streaming, no host
    // copy of the state); the extra sections are handed over as blobs and written one at a time
    // (peak RAM = one section). Returns the new entry id, or 0 on failure.
    uint64_t save(llama_context * ctx_tgt, llama_seq_id seq_id,
                  const std::vector<llama_token> & tokens,
                  server_disk_cache_extra && extra);

    // same, but with a caller-provided payload for the target section (used by the self-test,
    // which runs without a model)
    uint64_t save_raw(const std::vector<llama_token> & tokens,
                      const std::vector<uint8_t> & main_payload,
                      server_disk_cache_extra && extra);

    // restore the target sequence state from the payload file (llama_state_seq_load_file) and
    // return the extra sections, if the caller asks for them
    bool load(llama_context * ctx_tgt, llama_seq_id seq_id, uint64_t id, server_disk_cache_extra * extra_out);

    // Lifecycle guard (stage 6): an entry that is being read is never unlinked by eviction.
    // The internal lock is expected to be held (every read path holds it); the pair is public so
    // tests can pin an entry and check that eviction walks around it.
    void begin_read(uint64_t id) const;
    void end_read(uint64_t id) const;
    size_t n_inflight() const;

    // full payload read-back, for tests and debugging (keeps the whole payload in RAM)
    bool read_payload(uint64_t id, std::vector<uint8_t> & main_out, server_disk_cache_extra * extra_out) const;

    bool touch(uint64_t id);                         // hit_count++ + last_used = now; index update only
    bool set_last_used(uint64_t id, uint64_t unix_ts); // LRU bookkeeping (stage 6)
    bool remove_entry(uint64_t id);                  // unlink files + drop from index

    // rebuild the index by scanning states/*.meta (headers only - the payload is never read)
    bool rebuild_index();

    // evict up to `n` least-recently-used entries; returns how many were actually evicted
    size_t evict_lru(size_t n);

    // one-line description of the namespace, for the startup summary
    std::string summary() const;

private:
    struct meta_header {
        uint64_t id               = 0;
        uint64_t n_tokens         = 0;
        uint64_t hash_prefix_full = 0;
        uint64_t payload_bytes    = 0;
        uint64_t main_bytes       = 0;
        uint64_t off_dft          = 0;
        uint64_t dft_bytes        = 0;
        uint64_t off_ckpt         = 0;
        uint64_t ckpt_bytes       = 0;
        uint64_t off_spec         = 0;
        uint64_t spec_bytes       = 0;
        uint64_t created_unix     = 0;
        uint64_t last_used_unix   = 0;
        uint32_t hit_count        = 0;
        uint32_t flags            = 0;
    };

    bool open_namespace();
    bool create_namespace(const std::string & dir, bool fresh);

    bool load_index();
    bool save_index() const;
    bool scan_states(std::vector<server_disk_cache_entry> & out) const;
    void drop_tmp_files() const;

    uint64_t save_impl(const std::vector<llama_token> & tokens,
                       const std::vector<uint8_t> * main_payload, // null = stream from ctx
                       llama_context * ctx_tgt, llama_seq_id seq_id,
                       server_disk_cache_extra && extra);

    void drop_orphan_payloads() const;

    bool meta_write(uint64_t id, const server_disk_cache_entry & entry, const std::vector<llama_token> & tokens) const;
    bool meta_read(uint64_t id, server_disk_cache_entry & entry, std::vector<llama_token> * tokens_out) const;
    bool payload_read(uint64_t id, const server_disk_cache_entry & entry,
                      std::vector<uint8_t> * main_out, server_disk_cache_extra * extra_out) const;

    void index_add(const server_disk_cache_entry & entry);
    void index_remove_at(size_t idx);
    void index_rebuild_lookup();
    bool index_set_last_used(uint64_t id, uint64_t ts);

    uint64_t index_add_payload_bytes(uint64_t delta);

    size_t remove_many(const std::vector<uint64_t> & ids); // one pass + one index rebuild
    std::vector<uint64_t> lru_ids() const;                 // ids ordered by last_used (oldest first)

    int64_t  remove_files(uint64_t id) const;  // returns the freed bytes, -1 on error
    uint64_t dir_bytes(const std::string & dir) const;
    void     enforce_limit();

    std::string path_meta(uint64_t id) const;
    std::string path_bin(uint64_t id)  const;
    std::string path_tmp(const std::string & path) const;

    server_disk_cache_config      cfg;
    server_disk_cache_fingerprint fpv;

    std::string ns_dir;
    std::string states_dir;

    bool ready = false;

    mutable std::mutex mtx;

    std::vector<server_disk_cache_entry>          entries;
    std::unordered_map<uint64_t, std::vector<size_t>> by_hash; // hash_prefix_full -> entry indices
    uint64_t next_id    = 1;
    uint64_t n_bytes_   = 0;
    uint64_t root_bytes_ = 0;

    mutable std::unordered_map<uint64_t, int> inflight; // id -> number of readers

    uint64_t new_writes_total_ = 0; // entries actually written (a dedup hit does not count)
    uint64_t evictions_total_  = 0; // entries dropped by the size limit / explicit eviction
};
