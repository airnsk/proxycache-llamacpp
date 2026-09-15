// Self-test for the persistent disk cache (Stage 2 + Stage 3).
//
// Runs without a model: it exercises the namespace/fingerprint/manifest logic, the index,
// atomic writes, rebuilds and eviction using raw payload sections.
//
//   usage: llama-disk-cache-selftest [/tmp/dc-selftest-cache]
//
// Prints "SELFTEST_OK" and exits with 0 on success, prints the failures and exits with 1 otherwise.

#include "server-disk-cache.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int n_fail = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (cond) {                                             \
            printf("  ok   : " __VA_ARGS__);                    \
            printf("\n");                                       \
        } else {                                                \
            printf("  FAIL : " __VA_ARGS__);                    \
            printf("\n");                                       \
            n_fail++;                                           \
        }                                                       \
    } while (0)

static std::vector<llama_token> dc_tokens(std::initializer_list<llama_token> toks) {
    return std::vector<llama_token>(toks);
}

static std::vector<uint8_t> dc_bytes(size_t n, uint8_t seed) {
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = (uint8_t) (seed + i);
    }
    return out;
}

// matches the on-disk section layout: u64 n_blobs + per blob (4 x u64 + bytes)
static const uint64_t DC_EMPTY_SECTION = 8;

static uint64_t dc_section_size(std::initializer_list<size_t> blob_sizes) {
    uint64_t total = 8;
    for (const size_t s : blob_sizes) {
        total += 32 + s;
    }
    return total;
}

static server_disk_cache_fp_params dc_fp_params(const std::string & type_k = "f16", const std::string & type_v = "f16") {
    server_disk_cache_fp_params p;
    p.arch                 = "qwen3";
    p.name                 = "Qwen3.8-27B-Test";
    p.file_type            = "15";
    p.quantization_version = "2";
    p.size_label           = "UD-Q3_K_XL";
    p.model_desc           = "test model";
    p.model_path           = "/tmp/models/Qwen3.8-27B-UD-Q3_K_XL.gguf";
    p.n_layer              = 64;
    p.n_embd               = 5120;
    p.n_head               = 40;
    p.n_head_kv            = 8;
    p.type_k               = type_k;
    p.type_v               = type_v;
    p.flash_attn           = "auto";
    p.kv_unified           = true;
    p.swa_full             = false;
    p.n_ctx_checkpoints    = 32;
    p.checkpoint_min_step  = 8192;
    p.spec_type            = "none";
    p.loras                = {};
    return p;
}

static server_disk_cache_extra dc_extra() {
    server_disk_cache_extra e;
    e.dft.push_back({dc_bytes(64, 0x10), 42, 0, 41});
    e.ckpt.push_back({dc_bytes(128, 0x20), 1024, 0, 1023});
    e.ckpt.push_back({dc_bytes(256, 0x30), 2048, 1024, 2047});
    e.spec.push_back({dc_bytes(32, 0x40), 0, 0, 0});
    return e;
}

static std::string dc_read_file(const std::string & path) {
    std::FILE * f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return std::string();
    }
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

static void dc_write_file(const std::string & path, const std::string & text) {
    std::FILE * f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

//
// tests
//

static void test_fingerprint() {
    printf("[fingerprint]\n");

    const auto fp_a = server_disk_cache_fp_make(dc_fp_params());
    const auto fp_b = server_disk_cache_fp_make(dc_fp_params());
    const auto fp_k = server_disk_cache_fp_make(dc_fp_params("q8_0", "f16"));
    const auto fp_v = server_disk_cache_fp_make(dc_fp_params("f16", "q8_0"));

    CHECK(fp_a.hex.size() == 64, "fingerprint is a full sha256 hex (%zu chars)", fp_a.hex.size());
    CHECK(fp_a.hex == fp_b.hex, "fingerprint is stable between runs");
    CHECK(fp_a.hex != fp_k.hex, "fingerprint changes with ct_k (%s vs %s)", fp_a.hex.substr(0, 12).c_str(), fp_k.hex.substr(0, 12).c_str());
    CHECK(fp_a.hex != fp_v.hex, "fingerprint changes with ct_v");
    CHECK(fp_a.dir_name == "qwen3.8-27b-test-" + fp_a.hex.substr(0, 12), "namespace dir is <slug>-<fp12> ('%s')", fp_a.dir_name.c_str());
    CHECK(fp_a.components.size() > 15, "fingerprint has %zu components", fp_a.components.size());
}

static void test_namespace_and_manifest(const std::string & root) {
    printf("[namespace / manifest]\n");

    const auto fp = server_disk_cache_fp_make(dc_fp_params());

    server_disk_cache_config cfg;
    cfg.root = root;

    std::string dir1;
    {
        server_disk_cache c(cfg, fp);
        CHECK(c.ok(), "cache opened");
        CHECK(fs::exists(c.dir() + "/manifest.json"), "manifest.json created");
        CHECK(fs::exists(c.dir() + "/states"), "states/ created");
        CHECK(fs::exists(c.dir() + "/index.bin"), "index.bin created");
        CHECK(c.n_entries() == 0, "fresh namespace is empty");
        dir1 = c.dir();
    }

    {
        server_disk_cache c(cfg, fp);
        CHECK(c.ok() && c.dir() == dir1, "second open reuses the same namespace");
        const std::string manifest = dc_read_file(c.dir() + "/manifest.json");
        CHECK(manifest.find("\"open_count\": 2") != std::string::npos, "manifest counts the opens");
        CHECK(manifest.find("\"fingerprint_components\"") != std::string::npos, "manifest carries the fingerprint components");
    }

    // an incompatible manifest must not be used: a new namespace directory is created and the old
    // one is left alone
    {
        std::string manifest = dc_read_file(dir1 + "/manifest.json");
        const size_t pos = manifest.find("\"fingerprint\":");
        manifest.replace(pos + 15, 64, std::string(64, 'f'));
        dc_write_file(dir1 + "/manifest.json", manifest);

        const std::string marker = dir1 + "/marker.txt";
        dc_write_file(marker, "keep me");

        server_disk_cache c(cfg, fp);
        CHECK(c.ok(), "cache opened with an incompatible neighbour");
        CHECK(c.dir() != dir1, "incompatible namespace was skipped ('%s')", c.dir().c_str());
        CHECK(fs::exists(marker), "the old namespace was not touched");
    }
}

static void test_index_roundtrip(const std::string & root) {
    printf("[index round-trip]\n");

    const auto fp = server_disk_cache_fp_make(dc_fp_params());
    server_disk_cache_config cfg;
    cfg.root = root;

    uint64_t id_a = 0, id_b = 0;
    const uint64_t expect_bytes = 1000 + dc_section_size({64}) + dc_section_size({128, 256}) + dc_section_size({32})
                                + 2000 + 3 * DC_EMPTY_SECTION;
    {
        server_disk_cache c(cfg, fp);
        CHECK(c.ok(), "cache opened");

        server_disk_cache_extra extra = dc_extra();
        id_a = c.save_raw(dc_tokens({1, 2, 3}), dc_bytes(1000, 0x01), std::move(extra));
        id_b = c.save_raw(dc_tokens({1, 2, 3, 4, 5}), dc_bytes(2000, 0x02), server_disk_cache_extra());
        CHECK(id_a != 0 && id_b != 0, "two entries stored (id %llu, %llu)", (unsigned long long) id_a, (unsigned long long) id_b);
        CHECK(c.n_entries() == 2, "index has 2 entries");

        // extra sections survive the round-trip
        std::vector<uint8_t> main;
        server_disk_cache_extra read;
        CHECK(c.read_payload(id_a, main, &read) && main == dc_bytes(1000, 0x01), "payload read back");
        CHECK(read.dft.size() == 1 && read.dft[0].data == dc_bytes(64, 0x10) && read.dft[0].n_tokens == 42, "dft section read back");
        CHECK(read.ckpt.size() == 2 && read.ckpt[1].data == dc_bytes(256, 0x30) && read.ckpt[1].pos_max == 2047, "checkpoint sections read back");
        CHECK(read.spec.size() == 1 && read.spec[0].data == dc_bytes(32, 0x40), "spec section read back");

        std::vector<llama_token> toks;
        CHECK(c.tokens_of(id_b, toks) && toks == dc_tokens({1, 2, 3, 4, 5}), "exact token array stored in .meta");

        // the same state is not stored twice
        CHECK(c.save_raw(dc_tokens({1, 2, 3}), dc_bytes(1000, 0x01), server_disk_cache_extra()) == id_a, "duplicate save is a no-op");
        CHECK(c.n_entries() == 2, "duplicate save did not grow the index");
    }

    {
        server_disk_cache c(cfg, fp);
        CHECK(c.ok(), "cache reopened");
        CHECK(c.n_entries() == 2, "index survived the restart (%zu entries)", c.n_entries());
        CHECK(c.n_bytes() == expect_bytes, "byte counters restored (%llu == %llu)",
              (unsigned long long) c.n_bytes(), (unsigned long long) expect_bytes);
    }
}

static void test_candidate_search(const std::string & root) {
    printf("[candidate search]\n");

    const auto fp = server_disk_cache_fp_make(dc_fp_params());
    server_disk_cache_config cfg;
    cfg.root = root;

    server_disk_cache c(cfg, fp);
    CHECK(c.ok(), "cache opened");

    const uint64_t id3 = c.save_raw(dc_tokens({7, 8, 9}), dc_bytes(64, 0xaa), server_disk_cache_extra());
    const uint64_t id4 = c.save_raw(dc_tokens({7, 8, 9, 10}), dc_bytes(64, 0xbb), server_disk_cache_extra());
    const uint64_t id6 = c.save_raw(dc_tokens({7, 8, 9, 10, 11, 12}), dc_bytes(64, 0xcc), server_disk_cache_extra());
    CHECK(id3 && id4 && id6, "entries stored");

    server_disk_cache_candidate cand;

    CHECK(c.find(dc_tokens({7, 8, 9, 10, 11, 12, 13}), cand) && cand.id == id6 && cand.n_tokens == 6,
          "longest stored prefix wins (id %llu, %llu tokens)", (unsigned long long) cand.id, (unsigned long long) cand.n_tokens);

    CHECK(c.find(dc_tokens({7, 8, 9, 10, 11}), cand) && cand.id == id4 && cand.n_tokens == 4,
          "a stored state longer than the request is not usable (id %llu, %llu tokens)", (unsigned long long) cand.id, (unsigned long long) cand.n_tokens);

    CHECK(c.find(dc_tokens({7, 8, 9}), cand) && cand.id == id3 && cand.n_tokens == 3, "exact length match");

    CHECK(!c.find(dc_tokens({7, 8}), cand), "no entry of that length -> no candidate");
    CHECK(!c.find(dc_tokens({7, 8, 99}), cand), "a diverging token -> no candidate");

    // tampering with the stored tokens must make the entry unusable (this is the exact check that
    // protects against a hash collision)
    {
        const std::string meta = c.dir() + "/states/" + std::to_string(id3) + ".meta";
        std::string raw = dc_read_file(meta);
        CHECK(raw.size() > 200, "meta file present (%zu bytes)", raw.size());
        raw[200] = (char) 0x5a; // first token byte
        dc_write_file(meta, raw);

        server_disk_cache c2(cfg, fp);
        server_disk_cache_candidate cand2;
        CHECK(!c2.find(dc_tokens({7, 8, 9}), cand2) || cand2.id != id3, "tampered entry is not handed out");
    }
}

static void test_crash_recovery(const std::string & root) {
    printf("[crash safety / rebuild]\n");

    const auto fp = server_disk_cache_fp_make(dc_fp_params());
    server_disk_cache_config cfg;
    cfg.root = root;

    std::string dir;
    uint64_t id1 = 0;
    {
        server_disk_cache c(cfg, fp);
        id1 = c.save_raw(dc_tokens({1, 2, 3, 4}), dc_bytes(512, 0x01), server_disk_cache_extra());
        dir = c.dir();
        CHECK(id1 != 0, "entry stored");
    }

    // an interrupted write leaves *.tmp behind and a payload without meta
    dc_write_file(dir + "/states/999.bin.tmp", std::string(128, 'x'));
    dc_write_file(dir + "/states/998.meta.tmp", std::string(128, 'y'));
    dc_write_file(dir + "/states/997.bin", std::string(128, 'z')); // orphan payload

    {
        server_disk_cache c(cfg, fp);
        CHECK(c.ok(), "cache reopened after a simulated crash");
        CHECK(!fs::exists(dir + "/states/999.bin.tmp"), "leftover .tmp was removed");
        CHECK(!fs::exists(dir + "/states/998.meta.tmp"), "leftover .meta.tmp was removed");
        CHECK(!fs::exists(dir + "/states/997.bin"), "orphan payload was removed");
        CHECK(c.n_entries() == 1, "the valid entry is still there");
    }

    // index.bin deleted -> rebuilt from states/*.meta
    {
        fs::remove(dir + "/index.bin");
        server_disk_cache c(cfg, fp);
        CHECK(c.ok() && c.n_entries() == 1, "index rebuilt from .meta after index.bin was deleted (%zu entries)", c.n_entries());
        CHECK(fs::exists(dir + "/index.bin"), "index.bin written again");
    }

    // index.bin corrupted -> rebuilt
    {
        dc_write_file(dir + "/index.bin", std::string(512, '\x7f'));
        server_disk_cache c(cfg, fp);
        CHECK(c.ok() && c.n_entries() == 1, "index rebuilt from .meta after index.bin was corrupted (%zu entries)", c.n_entries());
    }

    // meta header corrupted -> the entry is dropped by the scan
    {
        const std::string meta = dir + "/states/" + std::to_string(id1) + ".meta";
        std::string raw = dc_read_file(meta);
        raw[8] = (char) 0x7f; // format_version byte
        dc_write_file(meta, raw);

        fs::remove(dir + "/index.bin");
        server_disk_cache c(cfg, fp);
        CHECK(c.ok() && c.n_entries() == 0, "an entry with a broken meta header is not indexed (%zu entries)", c.n_entries());
    }
}

static void test_lifecycle(const std::string & root) {
    printf("[lifecycle guard / bulk eviction]\n");

    const auto fp = server_disk_cache_fp_make(dc_fp_params());
    server_disk_cache_config cfg;
    cfg.root = root;

    server_disk_cache c(cfg, fp);
    CHECK(c.ok(), "cache opened");

    std::vector<uint64_t> ids;
    for (int i = 1; i <= 5; i++) {
        const uint64_t id = c.save_raw(dc_tokens({100 + i}), dc_bytes(1000, (uint8_t) i), server_disk_cache_extra());
        CHECK(id != 0, "entry %d stored", i);
        c.set_last_used(id, 1000 + i);
        ids.push_back(id);
    }
    CHECK(c.n_entries() == 5, "five entries before eviction (%zu)", c.n_entries());

    // the entry in the middle of the LRU order is pinned by a reader
    const uint64_t pinned = ids[2];
    c.begin_read(pinned);
    CHECK(c.n_inflight() == 1, "one entry is marked as in use (%zu)", c.n_inflight());

    const size_t evicted = c.evict_lru(10);
    CHECK(evicted == 4, "eviction removed the four unpinned entries (%zu)", evicted);
    CHECK(c.n_entries() == 1, "the pinned entry survived (%zu entries left)", c.n_entries());
    CHECK(fs::exists(c.dir() + "/states/" + std::to_string(pinned) + ".bin"), "the payload of the pinned entry is still on disk");
    CHECK(!fs::exists(c.dir() + "/states/" + std::to_string(ids[0]) + ".bin"), "the oldest entry is gone");
    CHECK(c.n_bytes() == 1000 + 3 * DC_EMPTY_SECTION, "byte counter is exact after the eviction (%llu)", (unsigned long long) c.n_bytes());

    // once the reader is done the entry is evictable, and the index on disk agrees
    c.end_read(pinned);
    CHECK(c.n_inflight() == 0, "no entry is in use anymore");
    CHECK(c.evict_lru(10) == 1, "the released entry is evicted");
    CHECK(c.n_entries() == 0 && c.n_bytes() == 0, "cache is empty and the counters are zero (%zu, %llu)",
          c.n_entries(), (unsigned long long) c.n_bytes());

    {
        server_disk_cache c2(cfg, fp);
        CHECK(c2.ok() && c2.n_entries() == 0, "reopened index agrees on the eviction (%zu entries)", c2.n_entries());
    }

    // bulk eviction cost: with the old O(n^2) picker this loop took quadratic time
    std::vector<uint64_t> many;
    many.reserve(300);
    for (int i = 0; i < 300; i++) {
        const uint64_t id = c.save_raw(dc_tokens({1000 + i}), dc_bytes(512, (uint8_t) (i & 0xff)), server_disk_cache_extra());
        c.set_last_used(id, 5000 + i);
        many.push_back(id);
    }
    CHECK(c.n_entries() == 300, "300 entries stored (%zu)", c.n_entries());

    const int64_t t0 = ggml_time_us();
    const size_t n_evicted = c.evict_lru(300);
    const double ms = (ggml_time_us() - t0) / 1000.0;
    CHECK(n_evicted == 300 && c.n_entries() == 0, "all 300 entries evicted (%zu, %zu left)", n_evicted, c.n_entries());
    printf("   bulk eviction of 300 entries: %.1f ms\n", ms);
}

static void test_eviction(const std::string & root) {
    printf("[eviction / size limit]\n");

    const auto fp = server_disk_cache_fp_make(dc_fp_params());
    server_disk_cache_config cfg;
    cfg.root = root;
    cfg.size_limit_bytes = 6000;

    server_disk_cache c(cfg, fp);
    CHECK(c.ok(), "cache opened");

    const uint64_t id1 = c.save_raw(dc_tokens({1}), dc_bytes(1500, 0x01), server_disk_cache_extra());
    const uint64_t id2 = c.save_raw(dc_tokens({2}), dc_bytes(1500, 0x02), server_disk_cache_extra());
    const uint64_t id3 = c.save_raw(dc_tokens({3}), dc_bytes(1500, 0x03), server_disk_cache_extra());
    CHECK(id1 && id2 && id3, "three entries stored");

    // make the LRU order explicit: id2 is the oldest, id3 the newest
    c.set_last_used(id1, 3000);
    c.set_last_used(id2, 1000);
    c.set_last_used(id3, 2000);

    CHECK(c.n_entries() == 3, "three entries before the limit is hit (%zu)", c.n_entries());
    CHECK(c.n_bytes() == 3 * (1500 + 3 * DC_EMPTY_SECTION), "byte counter tracks the payloads (%llu)", (unsigned long long) c.n_bytes());

    const uint64_t id4 = c.save_raw(dc_tokens({4}), dc_bytes(1500, 0x04), server_disk_cache_extra());
    CHECK(id4 != 0, "entry stored over the limit");

    // the limit is checked against the whole root, so at least the oldest entry must be gone
    CHECK(c.n_entries() < 4, "limit triggered an eviction (%zu entries left)", c.n_entries());
    CHECK(!fs::exists(c.dir() + "/states/" + std::to_string(id2) + ".bin"), "the least recently used entry was evicted");
    CHECK(fs::exists(c.dir() + "/states/" + std::to_string(id1) + ".bin"), "the newest entry survives");
    CHECK(c.root_bytes() <= cfg.size_limit_bytes, "root bytes are under the limit (%llu <= %llu)",
          (unsigned long long) c.root_bytes(), (unsigned long long) cfg.size_limit_bytes);
}

//
// main
//

int main(int argc, char ** argv) {
    std::string root = "/tmp/dc-selftest-cache";
    if (argc > 1) {
        root = argv[1];
    }

    printf("disk cache self-test, root = %s\n", root.c_str());

    std::error_code ec;
    fs::remove_all(root, ec);

    // one root per test group, the eviction test needs its own
    const std::string root_ns   = root + "/ns";
    const std::string root_idx  = root + "/idx";
    const std::string root_cand = root + "/cand";
    const std::string root_crash = root + "/crash";
    const std::string root_evict = root + "/evict";
    const std::string root_life  = root + "/life";

    test_fingerprint();
    test_namespace_and_manifest(root_ns);
    test_index_roundtrip(root_idx);
    test_candidate_search(root_cand);
    test_crash_recovery(root_crash);
    test_eviction(root_evict);
    test_lifecycle(root_life);

    printf("\n");
    if (n_fail == 0) {
        printf("SELFTEST_OK\n");
        return 0;
    }
    printf("SELFTEST_FAILED: %d checks failed\n", n_fail);
    return 1;
}
