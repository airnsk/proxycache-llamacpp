# Проверка Stage 2+3 (независимая, по факту на стенде)

Дата: 15.09.2026, стенд `user@stand.local` (RTX 3090, 24 ядра, 62 GB RAM).
База: master `38a5b42d9` + коммиты Stage 2 `0dd1409`, Stage 3 `bbabcae`.
Каталог кэша: `/mnt/3tb/llama-cache`. Дерево: `~/llamacpp-diskcache/llama.cpp` (build-base).

## Что проверено (факты, не пересказ)

1. **Сборка.** `BUILD_DC_DONE` в `/tmp/dc-build.log`; бинари:
   `build-base/bin/llama-server` (26 312 B, 20:47), `build-base/bin/llama-disk-cache-selftest` (192 096 B).
   `llama-server --version` → `version: 0.4.1-dev (build 0, commit unknown)`, GNU 13.3.0.
   (`commit unknown` — потому что на стенде дерево без `.git`, это ожидаемо.)

2. **Self-test модуля** (без модели), `/tmp/dc-selftest.log`, хвост:
   `ok: cache opened` / `three entries stored` / `byte counter tracks the payloads (4572)` /
   `entry stored over the limit` / `limit triggered an eviction (3 entries left)` /
   `the least recently used entry was evicted` / `the newest entry survives` /
   `root bytes are under the limit (4368 <= 6000)` → **`SELFTEST_OK` / `SELFTEST_EXIT=0`**.

3. **Реальный запуск сервера (run 1)**, `/tmp/dc-srv-run1.log`:
   ```
   disk cache: creating namespace '/mnt/3tb/llama-cache/qwen3.8-27b-d9cb2c2eabed'
   disk cache: no usable index.bin in '...', rebuilding from states/*.meta
   disk cache: rebuilt index: 0 states, 0.0 MiB
   load_model: disk cache: enabled, path = '/mnt/3tb/llama-cache'
   load_model: disk cache: namespace = '/mnt/3tb/llama-cache/qwen3.8-27b-d9cb2c2eabed'
   load_model: disk cache: fingerprint = d9cb2c2eabedfd20165bc1b61b23831f7fd2f5e1cbfbe49a9c88d658022a56c9
   load_model: disk cache: limit = 500.0 GiB (whole cache root), read speed = 200 MB/s, min gain = 1000 ms, prefill = auto
   load_model: disk cache: 0 states, 0.0 MiB on disk; RAM cache limit = 32768 MiB
   ```
   ⇒ multi-tier дефолт RAM 32 GiB включается только вместе с `--cache-disk` ✔ (согласованное решение).

4. **Manifest** (`/mnt/3tb/llama-cache/qwen3.8-27b-d9cb2c2eabed/manifest.json`, 1235 B, прочитан целиком):
   `format_version=1`, `disk_cache_format_version=1`, `state_seq_magic=1734833009`, `state_seq_version=3`,
   `fingerprint` (64 hex), `fingerprint_dir`, `model_slug=qwen3.8-27b`, `model_path`, `model_desc`,
   `created_utc`/`last_opened_utc`, `open_count`, `n_entries`, `n_bytes`,
   `fingerprint_components`: `general.architecture=qwen35`, `general.name=Qwen3.8-27B`,
   `general.file_type=13`, `general.quantization_version=2`, `general.size_label=27B`,
   hparams `block_count=64`, `n_embd=5120`, `n_head=24`, `n_head_kv=4`,
   `ctx.type_k=f16`, `ctx.type_v=f16`, `ctx.flash_attn=auto`, `ctx.kv_unified=1`, `ctx.swa_full=0`,
   `checkpoint.n_ctx=32`, `checkpoint.min_step=8192`, `spec.type=none`.
   ⇒ разделение по KV-типам (требование ТЗ §5) обеспечено: `type_k`/`type_v` в хеше.

5. **Формат index.bin — независимая проверка (не глазами автора кода).**
   `xxd` файла (64 B) и разбор по документированной раскладке (`server-disk-cache.cpp:285-300`):
   magic `LLDCIX01`, `format_version=1`, `header_size=64`, `entry_size=88`, `n_entries=0`, `next_id=1`,
   `crc32` в смещении 56 = `0x9eef841e`.
   Пересчитал crc32 (IEEE) по первым 64 байтам с обнулённым полем crc: **`0x9eef841e` — MATCH**.
   ⇒ заголовок индекса защищён и читается согласно спецификации.

6. **Артефакты состояния**: `states/` пока пуст (0 файлов) — ожидаемо: путь сохранения подключается
   на Stage 4, в этом прогоне `save()` вызывается только self-test'ом в отдельном temp-каталоге.

7. **CLI** (`common/arg.cpp:1770-1807`, поля `common/common.h:636-640`):
   `--cache-disk PATH`, `--cache-disk-size N` (GiB, лимит на весь корень — сказано в help),
   `--cache-disk-read-mbps N`, `--cache-disk-min-gain-ms N`, `--cache-prefill-tps N`.

## Что НЕ проверено (и почему)
- Повторный запуск с загрузкой **непустого** индекса (run 2) — на момент проверки `states/` пуст,
  писать состояния сервер на Stage 2+3 ещё не умеет; это критерий Stage 4 (после save-хуков).
- Кросс-модельная/кросс-KV изоляция на реальных прогонах (проверена только композицией fingerprint
  в manifest — фактической сменой `-ctk/-ctv` на сервере не гонялось).
- RESTART с реальным reuse префикса, стоимость restore, eviction на живом сервере — Stage 4-6.

## Параллельно подготовлено (этот же день)
- `~/dcbench/bench_disk_cache.py`, `bench_dc.sh` — бенч-харнесс ТЗ §35; `report` проверен на синтетике;
  функционально проверен на живом сервере (842 токена промпта, `prompt_eval_ms`/`tokens_cached`
  разобраны: `/tmp/dc_harness_check.log` → `SERVER_UP`, ответ распарсен).
  При проверке был найден реальный баг харнесса: ожидание готовности искало `server is listening`,
  а текущий master печатает `listening on` — исправлено на `grep -E "listening on|server is listening"`.
- `~/dcbench/disk_cache_e2e.py` — сценарии A/C/H/I/J (HTTP-уровень, без знания внутренностей модуля).
