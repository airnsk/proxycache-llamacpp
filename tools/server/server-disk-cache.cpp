#include "server-disk-cache.h"

#include "json.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

extern "C" {
#include "hash/sha256/sha256.h"
}

namespace fs = std::filesystem;

//
// helpers
//

static std::string dc_format(const char * fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

static uint64_t dc_now_unix() {
    return (uint64_t) time(nullptr);
}

static std::string dc_time_utc(uint64_t ts) {
    time_t t = (time_t) ts;
    struct tm tm_buf;
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

static std::string dc_hex(const uint8_t * digest, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(2 * len);
    for (size_t i = 0; i < len; ++i) {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 0xf];
    }
    return out;
}

static std::string dc_sha256(const std::string & text) {
    unsigned char digest[SHA256_DIGEST_SIZE];
    sha256_hash(digest, (const unsigned char *) text.data(), text.size());
    return dc_hex(digest, SHA256_DIGEST_SIZE);
}

static std::string dc_slugify(const std::string & name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (ok) {
            out += c;
        } else if (c >= 'A' && c <= 'Z') {
            out += (char) (c - 'A' + 'a');
        } else {
            out += '-';
        }
        if (out.size() >= 48) {
            break;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    while (!out.empty() && out.front() == '-') {
        out.erase(out.begin());
    }
    return out.empty() ? std::string("model") : out;
}

// little-endian serialization (explicit, so index/meta are portable)
static void dc_put_u32(uint8_t * p, uint32_t v) {
    p[0] = (uint8_t) (v & 0xff);
    p[1] = (uint8_t) ((v >> 8) & 0xff);
    p[2] = (uint8_t) ((v >> 16) & 0xff);
    p[3] = (uint8_t) ((v >> 24) & 0xff);
}

static void dc_put_u64(uint8_t * p, uint64_t v) {
    dc_put_u32(p + 0, (uint32_t) (v & 0xffffffffu));
    dc_put_u32(p + 4, (uint32_t) (v >> 32));
}

static uint32_t dc_get_u32(const uint8_t * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint64_t dc_get_u64(const uint8_t * p) {
    return (uint64_t) dc_get_u32(p) | ((uint64_t) dc_get_u32(p + 4) << 32);
}

// crc32 (IEEE 802.3) over a header with its own crc field zeroed
static uint32_t dc_crc32(const void * data, size_t len) {
    static uint32_t table[256];
    static bool table_init = false;
    if (!table_init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_init = true;
    }
    uint32_t crc = 0xffffffffu;
    const uint8_t * p = (const uint8_t *) data;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

static uint64_t dc_file_size(const std::string & path) {
    std::error_code ec;
    const auto sz = fs::file_size(path, ec);
    return ec ? 0 : (uint64_t) sz;
}

//
// hash
//

uint64_t server_disk_cache_hash_step(uint64_t h, llama_token token) {
    return h * SERVER_DISK_CACHE_HASH_PRIME + (uint64_t) (uint32_t) token;
}

uint64_t server_disk_cache_hash_prefix(const std::vector<llama_token> & tokens) {
    uint64_t h = 0;
    for (const llama_token tok : tokens) {
        h = server_disk_cache_hash_step(h, tok);
    }
    return h;
}

//
// fingerprint
//

void server_disk_cache_fp_model(const llama_model * model, const std::string & model_path, server_disk_cache_fp_params & out) {
    auto meta = [model](const char * key) -> std::string {
        std::vector<char> buf(512, 0);
        const int32_t n = llama_model_meta_val_str(model, key, buf.data(), buf.size());
        if (n < 0) {
            return std::string();
        }
        return std::string(buf.data());
    };

    out.arch                 = meta("general.architecture");
    out.name                 = meta("general.name");
    out.model_path           = model_path;
    out.file_type            = meta("general.file_type");
    out.quantization_version = meta("general.quantization_version");
    out.size_label           = meta("general.size_label");

    if (out.name.empty()) {
        out.name = fs::path(model_path).stem().string();
    }

    {
        std::vector<char> buf(256, 0);
        if (llama_model_desc(model, buf.data(), buf.size()) >= 0) {
            out.model_desc = std::string(buf.data());
        }
    }

    out.n_layer   = llama_model_n_layer(model);
    out.n_embd    = llama_model_n_embd(model);
    out.n_head    = llama_model_n_head(model);
    out.n_head_kv = llama_model_n_head_kv(model);
}

server_disk_cache_fingerprint server_disk_cache_fp_make(const server_disk_cache_fp_params & p) {
    server_disk_cache_fingerprint out;

    auto add = [&out](const std::string & k, const std::string & v) {
        out.components.emplace_back(k, v);
    };
    auto add_i = [&add](const std::string & k, int64_t v) {
        add(k, std::to_string(v));
    };
    auto add_b = [&add](const std::string & k, bool v) {
        add(k, v ? "1" : "0");
    };

    // the state_seq container format itself
    add_i("state_seq_magic",   LLAMA_STATE_SEQ_MAGIC);
    add_i("state_seq_version", LLAMA_STATE_SEQ_VERSION);
    add_i("disk_cache_format_version", SERVER_DISK_CACHE_FORMAT_VERSION);

    // model identity
    add("general.architecture",           p.arch);
    add("general.name",                   p.name);
    add("general.file_type",              p.file_type);
    add("general.quantization_version",   p.quantization_version);
    add("general.size_label",             p.size_label);
    add("model.desc",                     p.model_desc);

    // hparams that shape the state
    add_i("hparams.block_count", p.n_layer);
    add_i("hparams.n_embd",      p.n_embd);
    add_i("hparams.n_head",      p.n_head);
    add_i("hparams.n_head_kv",   p.n_head_kv);

    // KV configuration of the context
    add("ctx.type_k",     p.type_k);
    add("ctx.type_v",     p.type_v);
    add("ctx.flash_attn", p.flash_attn);
    add_b("ctx.kv_unified", p.kv_unified);
    add_b("ctx.swa_full",   p.swa_full);

    // checkpointing / speculative content of a disk entry
    add_i("checkpoint.n_ctx",  p.n_ctx_checkpoints);
    add_i("checkpoint.min_step", p.checkpoint_min_step);
    add("spec.type", p.spec_type);

    // adapters
    for (size_t i = 0; i < p.loras.size(); ++i) {
        add(dc_format("lora.%zu", i), p.loras[i]);
    }

    std::string canon;
    for (const auto & kv : out.components) {
        canon += kv.first;
        canon += '=';
        canon += kv.second;
        canon += '\n';
    }

    out.hex  = dc_sha256(canon);
    out.slug = dc_slugify(p.name.empty() ? p.model_path : p.name);
    out.model_path = p.model_path;
    out.model_desc = p.model_desc;
    out.dir_name = out.slug + "-" + out.hex.substr(0, 12);
    return out;
}

std::string server_disk_cache_sha256_file(const std::string & path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::string();
    }

    sha256_t ctx;
    sha256_init(&ctx);

    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), (std::streamsize) buf.size());
        const std::streamsize n = f.gcount();
        if (n > 0) {
            sha256_update(&ctx, (const unsigned char *) buf.data(), (size_t) n);
        }
    }

    unsigned char digest[SHA256_DIGEST_SIZE];
    sha256_final(&ctx, digest);
    return dc_hex(digest, SHA256_DIGEST_SIZE);
}

//
// index file layout (little-endian)
//
//   header 64 bytes:
//     0  magic[8] "LLDCIX01"
//     8  u32 format_version
//    12  u32 header_size (64)
//    16  u32 entry_size  (88)
//    20  u32 n_entries
//    24  u64 next_id
//    32  u64 reserved
//    40  u64 reserved
//    48  u64 reserved
//    56  u32 crc32 of the 64 header bytes with the crc32 field itself zeroed
//    60  u32 reserved
//   entries, entry_size bytes each:
//     0 u64 id | 8 u64 n_tokens | 16 u64 hash_prefix_full | 24 u64 payload_bytes
//    32 u64 main_bytes | 40 u64 dft_bytes | 48 u64 ckpt_bytes | 56 u64 spec_bytes
//    64 u64 last_used_unix | 72 u64 created_unix | 80 u32 hit_count | 84 u32 flags
//

#define DC_INDEX_MAGIC  "LLDCIX01"
#define DC_INDEX_HEADER 64u
#define DC_INDEX_ENTRY  88u

#define DC_META_MAGIC  "LLDCMT01"
#define DC_META_HEADER 200u

static void dc_index_entry_encode(uint8_t * p, const server_disk_cache_entry & e) {
    dc_put_u64(p +  0, e.id);
    dc_put_u64(p +  8, e.n_tokens);
    dc_put_u64(p + 16, e.hash_prefix_full);
    dc_put_u64(p + 24, e.payload_bytes);
    dc_put_u64(p + 32, e.main_bytes);
    dc_put_u64(p + 40, e.dft_bytes);
    dc_put_u64(p + 48, e.ckpt_bytes);
    dc_put_u64(p + 56, e.spec_bytes);
    dc_put_u64(p + 64, e.last_used_unix);
    dc_put_u64(p + 72, e.created_unix);
    dc_put_u32(p + 80, e.hit_count);
    dc_put_u32(p + 84, e.flags);
}

static server_disk_cache_entry dc_index_entry_decode(const uint8_t * p) {
    server_disk_cache_entry e;
    e.id               = dc_get_u64(p +  0);
    e.n_tokens         = dc_get_u64(p +  8);
    e.hash_prefix_full = dc_get_u64(p + 16);
    e.payload_bytes    = dc_get_u64(p + 24);
    e.main_bytes       = dc_get_u64(p + 32);
    e.dft_bytes        = dc_get_u64(p + 40);
    e.ckpt_bytes       = dc_get_u64(p + 48);
    e.spec_bytes       = dc_get_u64(p + 56);
    e.last_used_unix   = dc_get_u64(p + 64);
    e.created_unix     = dc_get_u64(p + 72);
    e.hit_count        = dc_get_u32(p + 80);
    e.flags            = dc_get_u32(p + 84);
    return e;
}

static void dc_meta_encode(uint8_t * p, const char * magic, const std::string & fp_hex, const server_disk_cache_entry & e,
                           uint64_t off_dft, uint64_t off_ckpt, uint64_t off_spec) {
    memset(p, 0, DC_META_HEADER);
    memcpy(p, magic, 8);
    dc_put_u32(p +  8, SERVER_DISK_CACHE_FORMAT_VERSION);
    dc_put_u32(p + 12, DC_META_HEADER);
    memcpy(p + 16, fp_hex.data(), std::min<size_t>(fp_hex.size(), 64));
    dc_put_u64(p +  80, e.id);
    dc_put_u64(p +  88, e.n_tokens);
    dc_put_u64(p +  96, e.hash_prefix_full);
    dc_put_u64(p + 104, e.payload_bytes);
    dc_put_u64(p + 112, e.main_bytes);
    dc_put_u64(p + 120, off_dft);
    dc_put_u64(p + 128, e.dft_bytes);
    dc_put_u64(p + 136, off_ckpt);
    dc_put_u64(p + 144, e.ckpt_bytes);
    dc_put_u64(p + 152, off_spec);
    dc_put_u64(p + 160, e.spec_bytes);
    dc_put_u64(p + 168, e.created_unix);
    dc_put_u64(p + 176, e.last_used_unix);
    dc_put_u32(p + 184, e.hit_count);
    dc_put_u32(p + 188, e.flags);
    dc_put_u32(p + 192, 0); // crc32 placeholder
    dc_put_u32(p + 196, 0);
    dc_put_u32(p + 192, dc_crc32(p, DC_META_HEADER));
}

// returns false if magic/version/crc do not match
static bool dc_meta_decode(const uint8_t * p, const std::string & fp_hex, server_disk_cache_entry & e,
                           uint64_t * off_dft, uint64_t * off_ckpt, uint64_t * off_spec) {
    if (memcmp(p, DC_META_MAGIC, 8) != 0) {
        return false;
    }
    if (dc_get_u32(p + 8) != SERVER_DISK_CACHE_FORMAT_VERSION) {
        return false;
    }
    if (dc_get_u32(p + 12) != DC_META_HEADER) {
        return false;
    }
    if (memcmp(p + 16, fp_hex.data(), 64) != 0) {
        return false;
    }
    {
        std::vector<uint8_t> copy(p, p + DC_META_HEADER);
        const uint32_t crc_stored = dc_get_u32(copy.data() + 192);
        dc_put_u32(copy.data() + 192, 0);
        if (dc_crc32(copy.data(), DC_META_HEADER) != crc_stored) {
            return false;
        }
    }
    e.id               = dc_get_u64(p +  80);
    e.n_tokens         = dc_get_u64(p +  88);
    e.hash_prefix_full = dc_get_u64(p +  96);
    e.payload_bytes    = dc_get_u64(p + 104);
    e.main_bytes       = dc_get_u64(p + 112);
    *off_dft           = dc_get_u64(p + 120);
    e.dft_bytes        = dc_get_u64(p + 128);
    *off_ckpt          = dc_get_u64(p + 136);
    e.ckpt_bytes       = dc_get_u64(p + 144);
    *off_spec          = dc_get_u64(p + 152);
    e.spec_bytes       = dc_get_u64(p + 160);
    e.created_unix     = dc_get_u64(p + 168);
    e.last_used_unix   = dc_get_u64(p + 176);
    e.hit_count        = dc_get_u32(p + 184);
    e.flags            = dc_get_u32(p + 188);
    return true;
}

//
// manifest
//

static bool dc_manifest_write(const std::string & path, const server_disk_cache_fingerprint & fp,
                              const std::string & created_utc_str, uint64_t open_count,
                              size_t n_entries, uint64_t n_bytes) {
    common_json j;

    j["format_version"]            = (uint64_t) 1;
    j["disk_cache_format_version"] = (uint64_t) SERVER_DISK_CACHE_FORMAT_VERSION;
    j["state_seq_magic"]           = (uint64_t) LLAMA_STATE_SEQ_MAGIC;
    j["state_seq_version"]         = (uint64_t) LLAMA_STATE_SEQ_VERSION;
    j["fingerprint"]               = fp.hex;
    j["fingerprint_dir"]           = fp.dir_name;
    j["model_slug"]                = fp.slug;
    j["model_path"]                = fp.model_path;
    j["model_desc"]                = fp.model_desc;
    j["created_utc"]               = created_utc_str.empty() ? dc_time_utc(dc_now_unix()) : created_utc_str;
    j["last_opened_utc"]           = dc_time_utc(dc_now_unix());
    j["open_count"]                = open_count;
    j["n_entries"]                 = (uint64_t) n_entries;
    j["n_bytes"]                   = n_bytes;

    common_json comps = common_json::object();
    for (const auto & kv : fp.components) {
        comps[kv.first] = kv.second;
    }
    j["fingerprint_components"] = comps;

    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        const std::string text = j.dump(2);
        f.write(text.data(), (std::streamsize) text.size());
        f.flush();
        if (!f) {
            return false;
        }
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// 0 = missing/unreadable/incompatible, otherwise the open count recorded in the manifest
static uint64_t dc_manifest_check(const std::string & path, const server_disk_cache_fingerprint & fp, std::string * created_utc) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return 0;
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    const common_json j = common_json::parse_no_throw(text);
    if (j.is_discarded() || !j.is_object()) {
        LOG_WRN("disk cache: manifest '%s' is not readable json\n", path.c_str());
        return 0;
    }
    if (j.value("disk_cache_format_version", (uint64_t) 0) != (uint64_t) SERVER_DISK_CACHE_FORMAT_VERSION) {
        LOG_WRN("disk cache: manifest '%s' has disk_cache_format_version %llu, expected %d\n",
                path.c_str(), (unsigned long long) j.value("disk_cache_format_version", (uint64_t) 0),
                SERVER_DISK_CACHE_FORMAT_VERSION);
        return 0;
    }
    if (j.value("state_seq_magic", (uint64_t) 0) != (uint64_t) LLAMA_STATE_SEQ_MAGIC ||
        j.value("state_seq_version", (uint64_t) 0) != (uint64_t) LLAMA_STATE_SEQ_VERSION) {
        LOG_WRN("disk cache: manifest '%s' was created for another state_seq format\n", path.c_str());
        return 0;
    }
    const std::string hex = j.value("fingerprint", std::string());
    if (hex != fp.hex) {
        LOG_WRN("disk cache: manifest '%s' fingerprint mismatch (%s != %s)\n", path.c_str(), hex.c_str(), fp.hex.c_str());
        return 0;
    }
    if (created_utc) {
        *created_utc = j.value("created_utc", std::string());
    }
    return std::max<uint64_t>(1, j.value("open_count", (uint64_t) 1));
}

//
// server_disk_cache
//

server_disk_cache::server_disk_cache(const server_disk_cache_config & cfg_, const server_disk_cache_fingerprint & fp_) : cfg(cfg_), fpv(fp_) {
    if (cfg.root.empty()) {
        LOG_WRN("%s", "disk cache: empty root path\n");
        return;
    }
    ready = open_namespace();
}

server_disk_cache::~server_disk_cache() = default;

bool server_disk_cache::ok() const {
    return ready;
}

const server_disk_cache_fingerprint & server_disk_cache::fp() const {
    return fpv;
}

const server_disk_cache_config & server_disk_cache::config() const {
    return cfg;
}

const std::string & server_disk_cache::dir() const {
    return ns_dir;
}

size_t server_disk_cache::n_entries() const {
    std::lock_guard<std::mutex> lock(mtx);
    return entries.size();
}

uint64_t server_disk_cache::n_bytes() const {
    std::lock_guard<std::mutex> lock(mtx);
    return n_bytes_;
}

uint64_t server_disk_cache::root_bytes() const {
    std::lock_guard<std::mutex> lock(mtx);
    return root_bytes_;
}

std::string server_disk_cache::summary() const {
    std::lock_guard<std::mutex> lock(mtx);
    return dc_format("namespace = '%s', %zu states, %.1f MiB payload, root %.1f MiB",
                     ns_dir.c_str(), entries.size(), (double) n_bytes_ / (1024.0 * 1024.0),
                     (double) root_bytes_ / (1024.0 * 1024.0));
}

std::string server_disk_cache::path_meta(uint64_t id) const {
    return states_dir + "/" + std::to_string(id) + ".meta";
}

std::string server_disk_cache::path_bin(uint64_t id) const {
    return states_dir + "/" + std::to_string(id) + ".bin";
}

std::string server_disk_cache::path_tmp(const std::string & path) const {
    return path + ".tmp";
}

uint64_t server_disk_cache::dir_bytes(const std::string & dir) const {
    std::error_code ec;
    uint64_t total = 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code ec2;
        if (it->is_regular_file(ec2)) {
            total += (uint64_t) it->file_size(ec2);
        }
    }
    return total;
}

void server_disk_cache::drop_tmp_files() const {
    std::error_code ec;
    for (fs::directory_iterator it(states_dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::string name = it->path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            LOG_WRN("disk cache: removing incomplete write '%s'\n", it->path().string().c_str());
            std::error_code ec2;
            fs::remove(it->path(), ec2);
        }
    }
}

void server_disk_cache::drop_orphan_payloads() const {
    // a payload without a meta file is the remains of a crashed write (the meta is written last)
    std::error_code ec;
    std::vector<std::string> bins;
    for (fs::directory_iterator it(states_dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::string name = it->path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".bin") == 0) {
            bins.push_back(name.substr(0, name.size() - 4));
        }
    }
    for (const std::string & stem : bins) {
        if (fs::exists(states_dir + "/" + stem + ".meta", ec)) {
            continue;
        }
        LOG_WRN("disk cache: removing orphan payload '%s'\n", (states_dir + "/" + stem + ".bin").c_str());
        std::error_code ec2;
        fs::remove(states_dir + "/" + stem + ".bin", ec2);
    }
}

bool server_disk_cache::open_namespace() {
    std::error_code ec;
    fs::create_directories(cfg.root, ec);
    if (ec) {
        LOG_WRN("disk cache: cannot create root '%s': %s\n", cfg.root.c_str(), ec.message().c_str());
        return false;
    }

    // whole-root usage is summed from file sizes only (no payload is read);
    // other namespaces are never modified, only their byte count contributes to the limit
    root_bytes_ = dir_bytes(cfg.root);

    std::string chosen;
    std::string created_utc;
    uint64_t    open_count = 1;
    bool        fresh      = false;

    for (int i = 0; i < 100 && chosen.empty(); ++i) {
        const std::string cand = cfg.root + "/" + fpv.dir_name + (i == 0 ? std::string() : dc_format("-%d", i + 1));
        if (!fs::exists(cand, ec)) {
            chosen = cand;
            fresh  = true;
            break;
        }
        if (!fs::is_directory(cand, ec)) {
            continue;
        }
        std::string cand_created;
        const uint64_t cand_open = dc_manifest_check(cand + "/manifest.json", fpv, &cand_created);
        if (cand_open > 0) {
            chosen      = cand;
            created_utc = cand_created;
            open_count  = cand_open + 1;
        } else {
            LOG_WRN("disk cache: namespace '%s' is not compatible, trying the next one\n", cand.c_str());
        }
    }

    if (chosen.empty()) {
        LOG_WRN("disk cache: cannot find/create a namespace directory under '%s'\n", cfg.root.c_str());
        return false;
    }

    ns_dir     = chosen;
    states_dir = ns_dir + "/states";

    fs::create_directories(states_dir, ec);
    if (ec) {
        LOG_WRN("disk cache: cannot create '%s': %s\n", states_dir.c_str(), ec.message().c_str());
        return false;
    }

    drop_tmp_files();
    drop_orphan_payloads();

    if (fresh) {
        LOG_INF("disk cache: creating namespace '%s'\n", ns_dir.c_str());
    }

    bool have_index = false;
    if (!fresh) {
        have_index = load_index();
    }

    if (have_index) {
        LOG_INF("disk cache: loaded index: %zu states, %.1f MiB\n", entries.size(),
                (double) n_bytes_ / (1024.0 * 1024.0));
    } else if (fresh) {
        save_index(); // a new namespace starts with an empty index
    } else {
        LOG_WRN("disk cache: no usable index.bin in '%s', rebuilding from states/*.meta\n", ns_dir.c_str());
        rebuild_index();
    }

    // record the namespace state (created_utc is preserved from the existing manifest)
    if (!dc_manifest_write(ns_dir + "/manifest.json", fpv, created_utc, open_count, entries.size(), n_bytes_)) {
        LOG_WRN("disk cache: cannot write manifest in '%s'\n", ns_dir.c_str());
        return false;
    }

    LOG_INF("%s\n", summary().c_str());
    return true;
}

//
// index
//

void server_disk_cache::index_rebuild_lookup() {
    by_hash.clear();
    for (size_t i = 0; i < entries.size(); ++i) {
        by_hash[entries[i].hash_prefix_full].push_back(i);
    }
}

void server_disk_cache::index_add(const server_disk_cache_entry & entry) {
    entries.push_back(entry);
    n_bytes_ += entry.payload_bytes;
    index_rebuild_lookup();
    next_id = std::max(next_id, entry.id + 1);
}

void server_disk_cache::index_remove_at(size_t idx) {
    n_bytes_ -= std::min(n_bytes_, entries[idx].payload_bytes);
    entries.erase(entries.begin() + (std::ptrdiff_t) idx);
    index_rebuild_lookup();
}

bool server_disk_cache::index_set_last_used(uint64_t id, uint64_t ts) {
    for (auto & e : entries) {
        if (e.id == id) {
            e.last_used_unix = ts;
            e.hit_count++;
            return true;
        }
    }
    return false;
}

uint64_t server_disk_cache::index_add_payload_bytes(uint64_t delta) {
    n_bytes_ += delta;
    return n_bytes_;
}

bool server_disk_cache::load_index() {
    const std::string path = ns_dir + "/index.bin";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }

    uint8_t header[DC_INDEX_HEADER];
    f.read((char *) header, sizeof(header));
    if (f.gcount() != (std::streamsize) sizeof(header)) {
        LOG_WRN("disk cache: index '%s' is truncated\n", path.c_str());
        return false;
    }
    if (memcmp(header, DC_INDEX_MAGIC, 8) != 0) {
        LOG_WRN("disk cache: index '%s' has a bad magic\n", path.c_str());
        return false;
    }
    if (dc_get_u32(header + 8) != 1 || dc_get_u32(header + 12) != DC_INDEX_HEADER || dc_get_u32(header + 16) != DC_INDEX_ENTRY) {
        LOG_WRN("disk cache: index '%s' has an unsupported layout\n", path.c_str());
        return false;
    }
    {
        uint8_t copy[DC_INDEX_HEADER];
        memcpy(copy, header, sizeof(copy));
        const uint32_t crc_stored = dc_get_u32(copy + 56);
        dc_put_u32(copy + 56, 0);
        if (dc_crc32(copy, sizeof(copy)) != crc_stored) {
            LOG_WRN("disk cache: index '%s' has a bad header checksum\n", path.c_str());
            return false;
        }
    }

    const uint32_t n_entries = dc_get_u32(header + 20);
    std::vector<server_disk_cache_entry> loaded;
    loaded.reserve(n_entries);

    std::vector<uint8_t> buf(DC_INDEX_ENTRY);
    for (uint32_t i = 0; i < n_entries; ++i) {
        f.read((char *) buf.data(), buf.size());
        if (f.gcount() != (std::streamsize) buf.size()) {
            LOG_WRN("disk cache: index '%s' is truncated at entry %u\n", path.c_str(), i);
            return false;
        }
        loaded.push_back(dc_index_entry_decode(buf.data()));
    }

    entries  = std::move(loaded);
    next_id  = std::max<uint64_t>(1, dc_get_u64(header + 24));
    n_bytes_ = 0;
    for (const auto & e : entries) {
        n_bytes_ += e.payload_bytes;
        next_id = std::max(next_id, e.id + 1);
    }
    index_rebuild_lookup();
    return true;
}

bool server_disk_cache::save_index() const {
    const std::string path = ns_dir + "/index.bin";
    const std::string tmp  = path + ".tmp";

    // the header lives in a stack buffer, the entries are streamed one by one
    uint8_t header[DC_INDEX_HEADER];
    memset(header, 0, sizeof(header));
    memcpy(header, DC_INDEX_MAGIC, 8);
    dc_put_u32(header +  8, 1);
    dc_put_u32(header + 12, DC_INDEX_HEADER);
    dc_put_u32(header + 16, DC_INDEX_ENTRY);
    dc_put_u32(header + 20, (uint32_t) entries.size());
    dc_put_u64(header + 24, next_id);
    dc_put_u32(header + 56, 0);
    dc_put_u32(header + 56, dc_crc32(header, DC_INDEX_HEADER));

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f.write((const char *) header, sizeof(header));

        uint8_t buf[DC_INDEX_ENTRY];
        for (const auto & e : entries) {
            dc_index_entry_encode(buf, e);
            f.write((const char *) buf, sizeof(buf));
        }

        f.flush();
        if (!f) {
            return false;
        }
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        LOG_WRN("disk cache: cannot replace index '%s': %s\n", path.c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool server_disk_cache::scan_states(std::vector<server_disk_cache_entry> & out) const {
    std::error_code ec;
    for (fs::directory_iterator it(states_dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::string name = it->path().filename().string();
        if (name.size() < 6 || name.compare(name.size() - 5, 5, ".meta") != 0) {
            continue;
        }

        // header only - the payload is never read while scanning
        std::ifstream f(it->path(), std::ios::binary);
        if (!f) {
            continue;
        }
        uint8_t header[DC_META_HEADER];
        f.read((char *) header, sizeof(header));
        if (f.gcount() != (std::streamsize) sizeof(header)) {
            LOG_WRN("disk cache: '%s' is truncated, skipping\n", it->path().string().c_str());
            continue;
        }
        server_disk_cache_entry e;
        uint64_t off_dft = 0, off_ckpt = 0, off_spec = 0;
        if (!dc_meta_decode(header, fpv.hex, e, &off_dft, &off_ckpt, &off_spec)) {
            LOG_WRN("disk cache: '%s' is not valid for this namespace, skipping\n", it->path().string().c_str());
            continue;
        }
        if (dc_file_size(path_bin(e.id)) != e.payload_bytes) {
            LOG_WRN("disk cache: payload of entry %llu is missing or has the wrong size, skipping\n",
                    (unsigned long long) e.id);
            continue;
        }
        out.push_back(e);
    }
    return true;
}

bool server_disk_cache::rebuild_index() {
    std::lock_guard<std::mutex> lock(mtx);

    std::vector<server_disk_cache_entry> scanned;
    scan_states(scanned);

    entries  = std::move(scanned);
    next_id  = 1;
    n_bytes_ = 0;
    for (const auto & e : entries) {
        n_bytes_ += e.payload_bytes;
        next_id = std::max(next_id, e.id + 1);
    }
    index_rebuild_lookup();

    LOG_INF("disk cache: rebuilt index: %zu states, %.1f MiB\n", entries.size(), (double) n_bytes_ / (1024.0 * 1024.0));
    return save_index();
}

//
// meta / payload
//

bool server_disk_cache::meta_write(uint64_t id, const server_disk_cache_entry & entry, const std::vector<llama_token> & tokens) const {
    const std::string path = path_meta(id);
    const std::string tmp  = path_tmp(path);

    uint8_t header[DC_META_HEADER];
    dc_meta_encode(header, DC_META_MAGIC, fpv.hex, entry, entry.main_bytes,
                   entry.main_bytes + entry.dft_bytes,
                   entry.main_bytes + entry.dft_bytes + entry.ckpt_bytes);

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f.write((const char *) header, sizeof(header));

        // the exact token array is written in chunks, so a long prompt does not need a second copy
        const size_t chunk_tokens = 4096;
        std::vector<uint8_t> chunk(sizeof(uint32_t) * std::min(chunk_tokens, tokens.size()));
        size_t pos = 0;
        while (pos < tokens.size()) {
            const size_t n = std::min(chunk_tokens, tokens.size() - pos);
            for (size_t i = 0; i < n; ++i) {
                dc_put_u32(chunk.data() + i * sizeof(uint32_t), (uint32_t) tokens[pos + i]);
            }
            f.write((const char *) chunk.data(), (std::streamsize) (n * sizeof(uint32_t)));
            pos += n;
        }

        f.flush();
        if (!f) {
            return false;
        }
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        LOG_WRN("disk cache: cannot rename '%s': %s\n", tmp.c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool server_disk_cache::meta_read(uint64_t id, server_disk_cache_entry & entry, std::vector<llama_token> * tokens_out) const {
    const std::string path = path_meta(id);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }

    uint8_t header[DC_META_HEADER];
    f.read((char *) header, sizeof(header));
    if (f.gcount() != (std::streamsize) sizeof(header)) {
        return false;
    }

    uint64_t off_dft = 0, off_ckpt = 0, off_spec = 0;
    if (!dc_meta_decode(header, fpv.hex, entry, &off_dft, &off_ckpt, &off_spec)) {
        return false;
    }

    if (tokens_out) {
        tokens_out->resize(entry.n_tokens);
        std::vector<uint8_t> buf(sizeof(uint32_t));
        for (uint64_t i = 0; i < entry.n_tokens; ++i) {
            f.read((char *) buf.data(), buf.size());
            if (f.gcount() != (std::streamsize) buf.size()) {
                tokens_out->clear();
                return false;
            }
            (*tokens_out)[i] = (llama_token) dc_get_u32(buf.data());
        }
    }
    return true;
}

static uint64_t dc_section_bytes(const std::vector<server_disk_cache_blob> & blobs) {
    uint64_t total = 8;
    for (const auto & b : blobs) {
        total += 4 * 8 + b.data.size();
    }
    return total;
}

static void dc_section_write(std::ostream & f, const std::vector<server_disk_cache_blob> & blobs) {
    std::vector<uint8_t> buf(8);
    dc_put_u64(buf.data(), blobs.size());
    f.write((const char *) buf.data(), 8);
    for (const auto & b : blobs) {
        buf.resize(32);
        dc_put_u64(buf.data() +  0, b.n_tokens);
        dc_put_u64(buf.data() +  8, b.pos_min);
        dc_put_u64(buf.data() + 16, b.pos_max);
        dc_put_u64(buf.data() + 24, b.data.size());
        f.write((const char *) buf.data(), 32);
        if (!b.data.empty()) {
            f.write((const char *) b.data.data(), (std::streamsize) b.data.size());
        }
    }
}

static bool dc_section_read(std::istream & f, uint64_t bytes, std::vector<server_disk_cache_blob> & out) {
    out.clear();
    if (bytes == 0) {
        return true;
    }

    uint64_t left = bytes;
    auto read_exact = [&f](void * dst, size_t n) -> bool {
        f.read((char *) dst, (std::streamsize) n);
        return (size_t) f.gcount() == n;
    };

    uint8_t buf[32];
    if (!read_exact(buf, 8)) {
        return false;
    }
    const uint64_t n_blobs = dc_get_u64(buf);
    left -= 8;

    for (uint64_t i = 0; i < n_blobs; ++i) {
        if (left < 32 || !read_exact(buf, 32)) {
            return false;
        }
        left -= 32;
        server_disk_cache_blob b;
        b.n_tokens = dc_get_u64(buf +  0);
        b.pos_min  = dc_get_u64(buf +  8);
        b.pos_max  = dc_get_u64(buf + 16);
        const uint64_t size = dc_get_u64(buf + 24);
        if (size > left) {
            return false;
        }
        b.data.resize(size);
        if (size > 0) {
            if (!read_exact(b.data.data(), (size_t) size)) {
                return false;
            }
            left -= size;
        }
        out.push_back(std::move(b));
    }
    return left == 0;
}

uint64_t server_disk_cache::save_impl(const std::vector<llama_token> & tokens,
                                      const std::vector<uint8_t> * main_payload,
                                      llama_context * ctx_tgt, llama_seq_id seq_id,
                                      server_disk_cache_extra && extra) {
    if (tokens.empty()) {
        LOG_WRN("%s", "disk cache: refusing to save an empty token sequence\n");
        return 0;
    }

    std::lock_guard<std::mutex> lock(mtx);
    if (!ready) {
        return 0;
    }

    const uint64_t hash = server_disk_cache_hash_prefix(tokens);

    // dedup: same prefix hash + same length + exact same tokens -> nothing to write
    {
        auto it = by_hash.find(hash);
        if (it != by_hash.end()) {
            for (const size_t idx : it->second) {
                const server_disk_cache_entry & e = entries[idx];
                if (e.n_tokens != tokens.size()) {
                    continue;
                }
                server_disk_cache_entry have;
                std::vector<llama_token> have_tokens;
                if (meta_read(e.id, have, &have_tokens) && have_tokens == tokens) {
                    index_set_last_used(e.id, dc_now_unix());
                    save_index();
                    return e.id;
                }
            }
        }
    }

    const uint64_t id = next_id;

    const std::string path = path_bin(id);
    const std::string tmp  = path_tmp(path);

    uint64_t main_bytes = 0;
    if (main_payload) {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            LOG_WRN("disk cache: cannot create '%s'\n", tmp.c_str());
            return 0;
        }
        if (!main_payload->empty()) {
            f.write((const char *) main_payload->data(), (std::streamsize) main_payload->size());
        }
        f.flush();
        if (!f) {
            LOG_WRN("disk cache: cannot write '%s'\n", tmp.c_str());
            std::error_code ec;
            fs::remove(tmp, ec);
            return 0;
        }
        main_bytes = main_payload->size();
    } else {
        // streamed straight into the payload file: no host copy of the target state
        main_bytes = (uint64_t) llama_state_seq_save_file(ctx_tgt, tmp.c_str(), seq_id, tokens.data(), tokens.size());
        if (main_bytes == 0) {
            LOG_WRN("disk cache: failed to save the target state of entry %llu\n", (unsigned long long) id);
            std::error_code ec;
            fs::remove(tmp, ec);
            return 0;
        }
    }

    const uint64_t dft_bytes  = dc_section_bytes(extra.dft);
    const uint64_t ckpt_bytes = dc_section_bytes(extra.ckpt);
    const uint64_t spec_bytes = dc_section_bytes(extra.spec);

    // the extra sections are written one at a time, so the peak RAM is one section
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::app);
        if (!f) {
            LOG_WRN("disk cache: cannot append to '%s'\n", tmp.c_str());
            std::error_code ec;
            fs::remove(tmp, ec);
            return 0;
        }
        dc_section_write(f, extra.dft);
        extra.dft.clear();
        extra.dft.shrink_to_fit();

        dc_section_write(f, extra.ckpt);
        extra.ckpt.clear();
        extra.ckpt.shrink_to_fit();

        dc_section_write(f, extra.spec);
        extra.spec.clear();
        extra.spec.shrink_to_fit();

        f.flush();
        if (!f) {
            LOG_WRN("disk cache: cannot write the sections of entry %llu\n", (unsigned long long) id);
            std::error_code ec;
            fs::remove(tmp, ec);
            return 0;
        }
    }

    {
        std::error_code ec;
        fs::rename(tmp, path, ec);
        if (ec) {
            LOG_WRN("disk cache: cannot rename '%s': %s\n", tmp.c_str(), ec.message().c_str());
            fs::remove(tmp, ec);
            return 0;
        }
    }

    server_disk_cache_entry entry;
    entry.id               = id;
    entry.n_tokens         = tokens.size();
    entry.hash_prefix_full = hash;
    entry.main_bytes       = main_bytes;
    entry.dft_bytes        = dft_bytes;
    entry.ckpt_bytes       = ckpt_bytes;
    entry.spec_bytes       = spec_bytes;
    entry.payload_bytes    = main_bytes + dft_bytes + ckpt_bytes + spec_bytes;
    entry.created_unix     = dc_now_unix();
    entry.last_used_unix   = entry.created_unix;
    entry.hit_count        = 0;
    entry.flags            = 0;
    if (dft_bytes > 8) {
        entry.flags |= SERVER_DISK_CACHE_FLAG_HAS_DFT;
    }
    if (ckpt_bytes > 8) {
        entry.flags |= SERVER_DISK_CACHE_FLAG_HAS_CKPT;
    }
    if (spec_bytes > 8) {
        entry.flags |= SERVER_DISK_CACHE_FLAG_HAS_SPEC;
    }

    // meta first: an entry without meta is invisible to a scan, while an index entry without files
    // would be unusable (the index is only updated after both files are on disk)
    if (!meta_write(id, entry, tokens)) {
        LOG_WRN("disk cache: cannot write '%s'\n", path_meta(id).c_str());
        std::error_code ec;
        fs::remove(path, ec);
        return 0;
    }

    index_add(entry);
    root_bytes_ += entry.payload_bytes;

    if (!save_index()) {
        LOG_WRN("%s", "disk cache: index update failed, the entry stays on disk and will be picked up by a rebuild\n");
    }

    LOG_INF("disk cache: stored state id=%llu, %llu tokens, %.2f MiB (%.2f MiB state + %.2f MiB extra)\n",
            (unsigned long long) id, (unsigned long long) entry.n_tokens,
            (double) entry.payload_bytes / (1024.0 * 1024.0), (double) main_bytes / (1024.0 * 1024.0),
            (double) (dft_bytes + ckpt_bytes + spec_bytes) / (1024.0 * 1024.0));

    enforce_limit();

    return id;
}

uint64_t server_disk_cache::save(llama_context * ctx_tgt, llama_seq_id seq_id,
                                 const std::vector<llama_token> & tokens,
                                 server_disk_cache_extra && extra) {
    if (ctx_tgt == nullptr) {
        LOG_WRN("%s", "disk cache: no target context\n");
        return 0;
    }
    return save_impl(tokens, nullptr, ctx_tgt, seq_id, std::move(extra));
}

uint64_t server_disk_cache::save_raw(const std::vector<llama_token> & tokens,
                                     const std::vector<uint8_t> & main_payload,
                                     server_disk_cache_extra && extra) {
    return save_impl(tokens, &main_payload, nullptr, 0, std::move(extra));
}

bool server_disk_cache::payload_read(uint64_t id, const server_disk_cache_entry & entry,
                                     std::vector<uint8_t> * main_out, server_disk_cache_extra * extra_out) const {
    std::ifstream f(path_bin(id), std::ios::binary);
    if (!f) {
        return false;
    }

    if (main_out) {
        main_out->resize(entry.main_bytes);
        if (entry.main_bytes > 0) {
            f.read((char *) main_out->data(), (std::streamsize) entry.main_bytes);
            if ((uint64_t) f.gcount() != entry.main_bytes) {
                return false;
            }
        }
    }

    if (extra_out) {
        const uint64_t off_dft  = entry.main_bytes;
        const uint64_t off_ckpt = off_dft + entry.dft_bytes;
        const uint64_t off_spec = off_ckpt + entry.ckpt_bytes;

        if (entry.dft_bytes > 0) {
            f.seekg((std::streamoff) off_dft);
            if (!dc_section_read(f, entry.dft_bytes, extra_out->dft)) {
                return false;
            }
        }
        if (entry.ckpt_bytes > 0) {
            f.seekg((std::streamoff) off_ckpt);
            if (!dc_section_read(f, entry.ckpt_bytes, extra_out->ckpt)) {
                return false;
            }
        }
        if (entry.spec_bytes > 0) {
            f.seekg((std::streamoff) off_spec);
            if (!dc_section_read(f, entry.spec_bytes, extra_out->spec)) {
                return false;
            }
        }
    }
    return true;
}

void server_disk_cache::begin_read(uint64_t id) const {
    // lifecycle guard (stage 6): an entry that is being read must not be unlinked by eviction.
    // The caller holds mtx (every read path holds it).
    inflight[id]++;
}

void server_disk_cache::end_read(uint64_t id) const {
    const auto it = inflight.find(id);
    if (it != inflight.end() && --it->second <= 0) {
        inflight.erase(it);
    }
}

size_t server_disk_cache::n_inflight() const {
    std::lock_guard<std::mutex> lock(mtx);
    return inflight.size();
}

bool server_disk_cache::read_payload(uint64_t id, std::vector<uint8_t> & main_out, server_disk_cache_extra * extra_out) const {
    std::lock_guard<std::mutex> lock(mtx);

    begin_read(id);

    server_disk_cache_entry entry;
    if (!meta_read(id, entry, nullptr)) {
        end_read(id);
        return false;
    }
    const bool ok = payload_read(id, entry, &main_out, extra_out);
    end_read(id);
    return ok;
}

namespace {
// pins an entry for the duration of a read (stage 6): eviction walks around in-flight entries
struct dc_read_guard {
    const server_disk_cache * cache;
    uint64_t id;
    dc_read_guard(const server_disk_cache & c, uint64_t id_) : cache(&c), id(id_) { cache->begin_read(id); }
    ~dc_read_guard() { cache->end_read(id); }
};
}

bool server_disk_cache::load(llama_context * ctx_tgt, llama_seq_id seq_id, uint64_t id, server_disk_cache_extra * extra_out) {
    if (ctx_tgt == nullptr) {
        LOG_WRN("%s", "disk cache: no target context\n");
        return false;
    }

    std::lock_guard<std::mutex> lock(mtx);
    if (!ready) {
        return false;
    }

    dc_read_guard guard(*this, id);

    server_disk_cache_entry entry;
    if (!meta_read(id, entry, nullptr)) {
        LOG_WRN("disk cache: no usable meta for entry %llu\n", (unsigned long long) id);
        return false;
    }
    if (dc_file_size(path_bin(id)) != entry.payload_bytes) {
        LOG_WRN("disk cache: payload of entry %llu is missing or has the wrong size\n", (unsigned long long) id);
        return false;
    }

    std::vector<llama_token> tokens(entry.n_tokens);
    size_t n_tokens_out = 0;
    const size_t res = llama_state_seq_load_file(ctx_tgt, path_bin(id).c_str(), seq_id,
                                                 tokens.data(), tokens.size(), &n_tokens_out);
    if (res == 0 || n_tokens_out != entry.n_tokens) {
        LOG_WRN("disk cache: failed to restore entry %llu (%zu tokens read)\n", (unsigned long long) id, n_tokens_out);
        return false;
    }
    tokens.clear();
    tokens.shrink_to_fit();

    if (extra_out && !payload_read(id, entry, nullptr, extra_out)) {
        LOG_WRN("disk cache: failed to read the extra sections of entry %llu\n", (unsigned long long) id);
        return false;
    }

    return true;
}

bool server_disk_cache::tokens_of(uint64_t id, std::vector<llama_token> & out) const {
    std::lock_guard<std::mutex> lock(mtx);
    server_disk_cache_entry entry;
    return meta_read(id, entry, &out);
}

bool server_disk_cache::entry_info(uint64_t id, server_disk_cache_entry & out) const {
    std::lock_guard<std::mutex> lock(mtx);

    for (const auto & e : entries) {
        if (e.id == id) {
            out = e;
            return true;
        }
    }

    // not in the index (e.g. the index was rebuilt while this entry was in flight): fall back to
    // the meta file, still without reading any payload
    return meta_read(id, out, nullptr);
}

bool server_disk_cache::find(const std::vector<llama_token> & tokens, server_disk_cache_candidate & out) const {
    std::lock_guard<std::mutex> lock(mtx);

    bool found = false;
    uint64_t h = 0;

    // one pass over the request: the hash of every prefix length that exists in the index
    for (size_t i = 0; i < tokens.size(); ++i) {
        h = server_disk_cache_hash_step(h, tokens[i]);

        auto it = by_hash.find(h);
        if (it == by_hash.end()) {
            continue;
        }
        for (const size_t idx : it->second) {
            const server_disk_cache_entry & e = entries[idx];
            if (e.n_tokens != i + 1) {
                continue;
            }
            if (found && e.n_tokens <= out.n_tokens) {
                continue;
            }

            // exact check against the stored token array - a hash collision must not be able to
            // hand out a foreign state
            server_disk_cache_entry have;
            std::vector<llama_token> have_tokens;
            if (!meta_read(e.id, have, &have_tokens)) {
                continue;
            }
            if (have_tokens.size() != e.n_tokens) {
                continue;
            }
            bool prefix = true;
            for (size_t t = 0; t < have_tokens.size(); ++t) {
                if (have_tokens[t] != tokens[t]) {
                    prefix = false;
                    break;
                }
            }
            if (!prefix) {
                continue;
            }

            out.id            = e.id;
            out.n_tokens      = e.n_tokens;
            out.payload_bytes = e.payload_bytes;
            found             = true;
        }
    }
    return found;
}

bool server_disk_cache::touch(uint64_t id) {
    return set_last_used(id, dc_now_unix());
}

bool server_disk_cache::set_last_used(uint64_t id, uint64_t unix_ts) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!index_set_last_used(id, unix_ts)) {
        return false;
    }
    return save_index();
}

int64_t server_disk_cache::remove_files(uint64_t id) const {
    std::error_code ec;
    const std::string meta = path_meta(id);
    const std::string bin  = path_bin(id);

    int64_t freed = 0;
    for (const std::string & p : { meta, bin, path_tmp(meta), path_tmp(bin) }) {
        freed += (int64_t) dc_file_size(p);
        std::error_code ec2;
        if (fs::exists(p, ec2)) {
            fs::remove(p, ec2);
        }
    }
    (void) ec;
    return freed;
}

bool server_disk_cache::remove_entry(uint64_t id) {
    std::lock_guard<std::mutex> lock(mtx);

    if (inflight.count(id) != 0) {
        LOG_WRN("disk cache: entry %llu is being read, not removing it\n", (unsigned long long) id);
        return false;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id != id) {
            continue;
        }
        const int64_t freed = remove_files(id);
        index_remove_at(i);
        if (freed > 0) {
            root_bytes_ -= std::min(root_bytes_, (uint64_t) freed);
        }
        return save_index();
    }
    return false;
}

size_t server_disk_cache::evict_lru(size_t n) {
    std::lock_guard<std::mutex> lock(mtx);

    if (n == 0 || entries.empty()) {
        return 0;
    }

    std::vector<uint64_t> ids = lru_ids();
    if (ids.size() > n) {
        ids.resize(n);
    }

    return remove_many(ids);
}

// ids ordered by last_used, oldest first. The caller holds mtx.
std::vector<uint64_t> server_disk_cache::lru_ids() const {
    std::vector<std::pair<uint64_t, uint64_t>> keyed; // (last_used, id)
    keyed.reserve(entries.size());
    for (const auto & e : entries) {
        keyed.emplace_back(e.last_used_unix, e.id);
    }
    std::sort(keyed.begin(), keyed.end());

    std::vector<uint64_t> ids;
    ids.reserve(keyed.size());
    for (const auto & kv : keyed) {
        ids.push_back(kv.second);
    }
    return ids;
}

// removes the given ids in one pass: a single index rebuild instead of one rebuild per victim.
// The caller holds mtx. In-flight entries are skipped, never unlinked.
size_t server_disk_cache::remove_many(const std::vector<uint64_t> & ids) {
    if (ids.empty()) {
        return 0;
    }

    std::unordered_map<uint64_t, char> victim;
    victim.reserve(ids.size());
    for (const uint64_t id : ids) {
        victim[id] = 1;
    }

    size_t done = 0;
    std::vector<server_disk_cache_entry> keep;
    keep.reserve(entries.size());

    for (const auto & e : entries) {
        if (victim.count(e.id) == 0) {
            keep.push_back(e);
            continue;
        }
        if (inflight.count(e.id) != 0) {
            LOG_WRN("disk cache: entry %llu is in use, eviction skipped it\n", (unsigned long long) e.id);
            keep.push_back(e);
            continue;
        }
        const int64_t freed = remove_files(e.id);
        if (freed < 0) {
            keep.push_back(e);
            continue;
        }
        root_bytes_ -= std::min<uint64_t>(root_bytes_, (uint64_t) freed);
        n_bytes_    -= std::min<uint64_t>(n_bytes_, e.payload_bytes);
        LOG_INF("disk cache: evicted entry %llu (%lld bytes freed)\n", (unsigned long long) e.id, (long long) freed);
        done++;
    }

    if (done > 0) {
        entries.swap(keep);
        index_rebuild_lookup();
        save_index();
    }

    return done;
}

//
// Stage 4+5: the cost model
//

void server_disk_cache_cost::observe(size_t n_tokens, double t_seconds) {
    if (n_tokens < SERVER_DISK_CACHE_TPS_MIN_TOKENS || t_seconds <= 0.0) {
        return;
    }

    const double cur = (double) n_tokens / t_seconds;

    // first observation initializes the EMA, later ones move it
    ema_tps = ema_tps > 0.0 ? (alpha * cur + (1.0 - alpha) * ema_tps) : cur;
    n_obs++;
}

double server_disk_cache_cost::tps(int32_t cfg_prefill_tps) const {
    if (cfg_prefill_tps > 0) {
        return (double) cfg_prefill_tps;
    }

    return ema_tps; // 0 = unknown
}

void server_prefix_candidate_estimate(server_prefix_candidate & cand, size_t n_prompt,
                                      double tps, double read_bps) {
    const size_t n_tail = n_prompt > cand.n_tokens_match ? n_prompt - cand.n_tokens_match : 0;

    cand.t_restore_est = read_bps > 0.0 ? (double) cand.restore_bytes / read_bps : 0.0;
    if (cand.restore_bytes > 0) {
        cand.t_restore_est += SERVER_DISK_CACHE_T_RESTORE_FIXED_SEC;
    }

    cand.t_estimate_valid = tps > 0.0;
    cand.t_tail_est       = cand.t_estimate_valid ? (double) n_tail / tps : 0.0;
    cand.t_total_est      = cand.t_restore_est + cand.t_tail_est;
}

bool server_prefix_candidate_prefer_disk(const server_prefix_candidate & disk,
                                         const server_prefix_candidate & resident,
                                         size_t n_prompt, bool tps_known, int32_t min_gain_ms,
                                         std::string * reason) {
    auto fail = [reason](const std::string & why) {
        if (reason) {
            *reason = why;
        }
        return false;
    };
    auto pass = [reason](const std::string & why) {
        if (reason) {
            *reason = why;
        }
        return true;
    };

    if (disk.source != SERVER_PREFIX_SOURCE_DISK) {
        return fail("not a disk candidate");
    }

    // the state must cover an exact prefix (the caller guarantees it via find()), but a candidate that
    // does not beat the resident prefix cannot win: its tail is at least as long
    if (disk.n_tokens_match <= resident.n_tokens_match) {
        return fail(dc_format("disk prefix (%zu) is not longer than the resident one (%zu)",
                              disk.n_tokens_match, resident.n_tokens_match));
    }

    if (disk.restore_bytes == 0) {
        return fail("disk entry has no payload");
    }

    const double gain_ms_min = (double) min_gain_ms;

    if (!tps_known) {
        // no measurement yet (Stage 5, ТЗ §12): the resident path wins unless it is far from covering
        // the request, and even then the win must hold at the conservative floor rate
        if (resident.n_tokens_match * 100 >= n_prompt * 90) {
            return fail(dc_format("prefill tps unknown and the resident state covers %zu/%zu tokens (>= 90%%)",
                                  resident.n_tokens_match, n_prompt));
        }

        const double t_res = (double) (n_prompt - resident.n_tokens_match) / SERVER_DISK_CACHE_TPS_FLOOR;
        const double t_dis = (double) (n_prompt - disk.n_tokens_match)     / SERVER_DISK_CACHE_TPS_FLOOR
                           + disk.t_restore_est;
        const double gain_ms = (t_res - t_dis) * 1e3;

        if (gain_ms >= gain_ms_min) {
            return pass(dc_format("prefill tps unknown, floor %.1f t/s: est. gain %.0f ms >= %d ms",
                                  SERVER_DISK_CACHE_TPS_FLOOR, gain_ms, min_gain_ms));
        }

        return fail(dc_format("prefill tps unknown, floor %.1f t/s: est. gain %.0f ms < %d ms",
                              SERVER_DISK_CACHE_TPS_FLOOR, gain_ms, min_gain_ms));
    }

    const double t_total_res = resident.t_total_est;
    const double t_total_dis = disk.t_total_est * (1.0 + SERVER_DISK_CACHE_COST_MARGIN);
    const double gain_ms     = (t_total_res - disk.t_total_est) * 1e3;

    if (!(t_total_dis < t_total_res)) {
        return fail(dc_format("t_total disk %.3f s (with %.0f%% margin) >= resident %.3f s",
                              t_total_dis, 100.0 * SERVER_DISK_CACHE_COST_MARGIN, t_total_res));
    }

    if (!(gain_ms >= gain_ms_min)) {
        return fail(dc_format("est. gain %.0f ms < %d ms", gain_ms, min_gain_ms));
    }

    return pass(dc_format("t_total disk %.3f s vs resident %.3f s (est. gain %.0f ms >= %d ms)",
                          disk.t_total_est, t_total_res, gain_ms, min_gain_ms));
}

void server_disk_cache::enforce_limit() {
    if (cfg.size_limit_bytes == 0 || root_bytes_ <= cfg.size_limit_bytes) {
        return; // no limit, or already inside it
    }

    // stage 6: the victims are picked in ONE sorted pass. The previous version scanned the whole
    // table for every victim, which made a full eviction O(n^2).
    std::unordered_map<uint64_t, uint64_t> size_of;
    size_of.reserve(entries.size());
    for (const auto & e : entries) {
        size_of[e.id] = e.payload_bytes;
    }

    const uint64_t over = root_bytes_ - cfg.size_limit_bytes;
    std::vector<uint64_t> victims;
    uint64_t planned = 0;

    for (const uint64_t id : lru_ids()) {
        if (planned >= over) {
            break;
        }
        const auto it = size_of.find(id);
        if (it == size_of.end()) {
            continue;
        }
        planned += it->second;
        victims.push_back(id);
    }

    const size_t done = remove_many(victims);
    if (done > 0) {
        LOG_INF("disk cache: evicted %zu entries (%llu bytes planned) to stay under the %.1f GiB limit\n",
                done, (unsigned long long) planned,
                (double) cfg.size_limit_bytes / (1024.0 * 1024.0 * 1024.0));
    }
}
