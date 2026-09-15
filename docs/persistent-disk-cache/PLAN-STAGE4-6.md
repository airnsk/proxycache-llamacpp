# Stage 4–6 — план врезки (после Stage 2+3)

Основание: `ARCHITECTURE.md` (Stage 1). Границы Stage 2+3: модуль `tools/server/server-disk-cache.{h,cpp}`
(namespace/manifest/index, атомарная запись, self-test), CLI `--cache-disk*`, startup summary.
Здесь — точный план того, что делать дальше, чтобы не переделывать API модуля.

---

## Привязка к фактическому API модуля (после Stage 2+3, коммиты `0dd1409` + `bbabcae`)

Реализовано в `tools/server/server-disk-cache.{h,cpp}` (283 + 1486 строк):
`ok()`, `fp()`, `config()`, `dir()`, `n_entries()`, `n_bytes()`, `root_bytes()`,
`find(tokens, out) -> best exact-prefix candidate`, `tokens_of(id)`, `save(ctx_tgt, seq_id, tokens, extra) -> id`
(потоковая запись через `llama_state_seq_save_file`, `server-disk-cache.cpp:1112`),
`save_raw(...)` для self-test, `load(ctx_tgt, seq_id, id, extra_out)` (`llama_state_seq_load_file`, `:1307`),
`read_payload`, `touch(id)` (только index), `set_last_used`, `remove_entry`,
`rebuild_index()`, `evict_lru(n)`, `summary()`.
CLI уже есть: `--cache-disk`, `--cache-disk-size` (GiB, на весь корень), `--cache-disk-read-mbps`,
`--cache-disk-min-gain-ms`, `--cache-prefill-tps` (`common/arg.cpp:1770-1807`, поля `common/common.h:636-640`).
Init + startup summary — `server-context.cpp:918, 1354-1421` (RAM 32 GiB в multi-tier: `:1354`).
Self-test: `tools/server/server-disk-cache-selftest.cpp` (`SELFTEST_OK`, `:393`).

⇒ Stage 4 обязан использовать **этот** API: `find()` для кандидата, `tokens_of()` для точной проверки
перед cost-сравнением, `load()` для restore, `touch()` при hit, `save()` после prefill/idle-save.
Дополнительно понадобится (Stage 4):
`estimate`-поля кандидата (bytes/`payload_bytes` уже есть) и метод для «resident-кандидат из другого слота»
(сейчас есть только то, что лежит в RAM-кэше/слотах).

## Stage 4. Lookup и сохранение в существующем жизненном цикле

### 4.1 Точки врезки (фактические, master 38a5b42d9)

1. `server_context::get_available_slot()` — `tools/server/server-context.cpp:~1540-1665`.
   Сейчас: выбор слота (id_slot → LCP-similarity → LRU) → при `update_cache`:
   `ret->prompt_save(*prompt_cache)` → `ret->prompt_load(*prompt_cache, task.tokens)` → `prompt_cache->update()`.
   **Врезка:** после `prompt_load` (RAM-кандидат уже resident в слоте) вычислить resident-LCP и
   сравнить с лучшим disk-кандидатом; если disk выгоднее — перезаписать слот из disk и записать
   в статистику выбора.
2. `server_slot::prompt_save()` — `server-context.cpp:299`.
   **Врезка:** после успешного сохранения в RAM-кэш поставить запись в очередь disk-кэша
   (асинхронно, см. 4.4).
3. Idle-слоты — `server-context.cpp:~2441` (`--cache-idle-slots`): тот же хук, что в (2) —
   сохранять в disk перед `prompt_clear()`.
4. Вытеснение из RAM — `server_prompt_cache::update()` (`server-task.cpp:1870`) и
   `server_prompt_cache::alloc()` (FIFO `pop_front`): **врезка** — перед удалением записи отдать её
   в disk-кэш (это и есть «сэкономленное состояние не теряется»), если включён disk.

### 4.2 Кандидат как единая структура сравнения

Уже в Stage 2+3 модуль должен отдавать список кандидатов; на Stage 4 добавить нормализацию:

```
struct server_prefix_candidate {
    enum source_t { RESIDENT, RAM, DISK };
    source_t source;
    const server_prompt * prompt;   // токены + checkpoints (для RAM/RESIDENT)
    uint64_t state_id;              // для DISK
    size_t   n_tokens_state;        // сколько токенов в состоянии
    size_t   n_tokens_match;        // = n_tokens_state (точный префикс, см. Stage 1 §20)
    uint64_t restore_bytes;
    double   t_restore_est;         // сек
    double   t_tail_est;            // сек
    double   t_total_est;
};
```
Правило: кандидат валиден только если `tokens_state` — **точный префикс** tokens запроса
(никаких «обрезаний» state). LCP кандидата поэтому равен `n_tokens_state`.

### 4.3 Cost model (Stage 5, но формула фиксируется сейчас)

```
tps_prefill = cache_prefill_tps > 0 ? cache_prefill_tps : ema_prefill_tps
t_restore   = restore_bytes / read_bps + t_restore_fixed
t_tail      = (n_prompt - n_tokens_match) / tps_prefill
t_total     = t_restore + t_tail
use_disk ⟺ t_total_disk * (1 + margin) < t_total_resident
             и (t_total_resident - t_total_disk) * 1000 ≥ cache_disk_min_gain_ms
```
* `t_restore_fixed` — константа из замера (Stage 7), до замера 0 и запись «не измерено».
* `margin` — 0.05 (гистерезис 5 % против шума), в CLI не выставляется (внутренний).
* `ema_prefill_tps`: брать из фактических prompt-eval таймингов сервера. Источник — статистика слота
  (`slot.stats.n_prompt_tokens/prompt_ms`, заполняется в `server-context.cpp:~3409`
  `slot.stats.n_prompt_cached = n_past`) → EMA с α=0.2, инициализация «неизвестно».
* **При неизвестном tps_prefill**: disk не выбирается, если resident-LCP ≥ 90 % промпта;
  иначе допускается disk при явном преимуществе по байтам (t_restore < t_tail при консервативной
  нижней оценке tps). Это и есть требование ТЗ §12.

### 4.4 Асинхронность записи

* Один worker-thread в `server_disk_cache` (создаётся при init, join при shutdown).
* Очередь `save_queue` (bounded, например 8 записей; при переполнении — drop oldest с WARN, не блокировать).
* Данные для записи: **слот может быть переиспользован**, поэтому нельзя писать «потом из слота»:
  в очередь кладётся либо (а) вызов-замыкание, выполняющий `llama_state_seq_save_file()` в файл
  **синхронно в worker'е** (нужен `llama_context`, а он не потокобезопасен!), либо (б) заранее
  сериализованный payload.
* Решение (важно, оценить при реализации Stage 4): `llama_context` обслуживается только основным
  потоком сервера, поэтому безопасный вариант — **сериализовать в файл `.tmp` в основном потоке**
  (это запись на диск, но без RAM-копии и без ожидания fsync), а в worker'е делать `rename`+index
  update. Тогда «дорогая» часть (rename/индексация/eviction) — вне критического пути, а небезопасный
  доступ к ctx остаётся в своём потоке. Альтернатива (сериализация в worker'е) требует внешнего
  mutex'а на ctx — отвергается, пока не подтверждена необходимость.

### 4.5 Restore

* `llama_state_seq_load_file(ctx_tgt, path, id_slot, tokens_out, cap, &count)` для main-секции
  (потоково, без RAM-копии), затем секции draft/checkpoints (`set_data_ext` по одной за раз).
* После restore: `slot.prompt.tokens = entry.tokens`, `slot.prompt.checkpoints = entry.checkpoints`,
  и дальше работает существующий путь (`server-context.cpp:~3217`: `n_past = get_common_prefix(...)`).
  Никакого параллельного inference-пути.
* Ошибка/чек-сумма/несовпадение токенов → entry помечается corrupt, файлы удаляются (или
  переносятся в `states/corrupt/`), счётчик `disk_cache_corrupt_entries++`, запрос продолжается
  полным prefill.

---

## Stage 5. Метрики

`tools/server/server-common.h:442 struct server_metrics` + вывод в `/metrics`:
`disk_cache_hits`, `disk_cache_misses`, `disk_cache_restores`, `disk_cache_restore_bytes`,
`disk_cache_restore_seconds`, `disk_cache_writes`, `disk_cache_write_bytes`, `disk_cache_evictions`,
`disk_cache_corrupt_entries`, `disk_cache_resident_preferred`, `disk_cache_disk_preferred`,
`disk_cache_saved_prefill_tokens`, `disk_cache_index_entries`, `disk_cache_index_bytes`.

Логи (уровни: INFO — события, TRACE — детали), формат из ТЗ §26, без per-token спама и **без
текста промптов** (только числа/хеши/имена файлов).

---

## Stage 6. Eviction, concurrency, crash-recovery

* Index под mutex'ом; payload — нет. `refcount` + `state == ACTIVE_READ|PENDING_DELETE`;
  eviction пропускает записи с refcount>0 и повторяет проход.
* LRU по `last_used_unix` (обновляется в index при hit; запись index — батчами, атомарно через `.tmp`+rename).
* Лимит — по фактической сумме `payload_bytes` всех namespace (ТЗ §42, вариант A), учёт — при скане
  корня на старте (только `*.meta`/index, без payload).
* Crash-recovery: при старте `*.tmp` удаляются; нечитаемый `index.bin` → WARN + `rebuild_index()`
  из `states/*.meta`; `.meta` без `.bin` и наоборот → запись удаляется.
* Дедупликация: ключ (fingerprint, n_tokens, hash_prefix_full) + точное сравнение токенов `.meta`
  перед повторной записью (одинаковое состояние — не писать второй раз).
* Shutdown: остановка worker'а, дренаж очереди с таймаутом, `index` сохраняется.

---

## Stage 7. Тесты и бенчмарк

Сценарии A–L из ТЗ §34 → `tools/server/tests/` (pytest, есть `conftest.py`, `tests.sh`) + self-test
бинарь для сценариев без модели (A/B/C/F/G/H/I/J конструируются на уровне модуля).
Модельные: D/E (cost model) и K (hybrid/recurrent) — на стенде, модель `Qwen3.8-27B-UD-Q3_K_XL.gguf`
(hybrid+recurrent), контекст `-c 160000 -ctk q8_0 -ctv q8_0 -fa on` (проверенный потолок стенда).
Бенчмарк (ТЗ §35): скрипт `bench_disk_cache.sh` — первый прогон (prefill), рестарт, второй прогон
(restore + suffix), вывод: bytes, restore_s, suffix_tokens, prefill_s, speedup.
Критерии приёмки (ТЗ §55-56): выключенный кэш — без регрессии; hit — без disk I/O при resident;
startup — без чтения payload; корректность — state после restore даёт тот же результат, что полный prefill
(проверка sha256 `content` при temp 0 / фиксированном seed).
