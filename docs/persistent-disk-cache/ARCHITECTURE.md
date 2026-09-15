# Persistent prefix/KV cache для llama-server — архитектурная записка (Stage 1)

База исследования: **ggml-org/llama.cpp master `38a5b42d9a3e82e0a586bcd1caed121f36c87a73`** (clone 15.09.2026).
Все ссылки — на актуальные файлы этого дерева (не на устаревшие имена из ТЗ).

---

## 1. Что реально есть в коде сейчас

### 1.1 Токены и сравнение префиксов
* `server_tokens` (`tools/server/server-common.h:152`) — обёртка над `llama_tokens` + флаг `has_mtmd`
  (+ `std::map<size_t, mtmd::input_chunk_ptr> map_idx_to_media`).
* `server_tokens::get_common_prefix(const server_tokens & b)` — `tools/server/server-common.cpp:697`.
  Это **точное поэлементное сравнение `llama_token` слева направо** (LCP), не хеш. Для mtmd-случая
  есть отдельная ветка, учитывающая media-чанки.
* **Хешей токенов/префиксов в сервере нет.** grep по `hash` в `server-common.*`, `server-task.*` —
  ноль совпадений. Индексов/деревьев нет. Никакого chunk-hashing нет.
  (В `common/chat-*` есть `common_prefix_len`/`until_common_prefix`, но это парсер чатов, к KV-кэшу
  отношения не имеет.)

### 1.2 RAM prompt cache (L2)
`server_prompt_cache` — `tools/server/server-task.h:612`, реализация `tools/server/server-task.cpp:1689-1900`.

* Хранит `std::list<server_prompt_cache_state>`:
  * `server_prompt { server_tokens tokens; std::list<common_prompt_checkpoint> checkpoints; }`
    (`server-task.h:566`);
  * `server_prompt_data { std::vector<uint8_t> main, drft; }` (`server-task.h:588`) — **сырые
    in-RAM буферы** состояний target- и draft-контекста, полученные `llama_state_seq_get_data_ext()`.
* Лимиты: `limit_size` (байты, из `--cache-ram` MiB) и `limit_tokens` (из `n_ctx`).
  Конструктор: `prompt_cache = std::make_unique<server_prompt_cache>(params_base.cache_ram_mib, n_ctx)`
  (`server-context.cpp:1359`).
* `alloc()` (`server-task.cpp:1711`) — при сохранении: отбрасывает записи, полностью содержащиеся в
  новом промпте, форсит место (`states.pop_front()` — FIFO, не LRU!), затем копирует байты.
* `load()` (`server-task.cpp:1793`) — **линейный проход по всем записям**, для каждой
  `lcp = it->prompt.tokens.get_common_prefix(tokens_new)`;
  оценки `f_keep = lcp / entry_len`, `f_sim = lcp / new_len`;
  фильтр `f_keep_cur < 0.25` (не вытеснять большие промпты); выбор максимизирует обе оценки
  одновременно (`f_keep_best < f_keep_cur && f_sim_best < f_sim_cur`).
  Восстановление — `llama_state_seq_set_data_ext(ctx_tgt, …)` и (если есть) `…(ctx_dft, …)`,
  затем запись **забирается** из кэша (`prompt = std::move(it_best->prompt); states.erase(it_best);`).
  То есть RAM-кэш — «перемещающий»: одна запись обслуживает один слот, потом живёт как resident state слота.
* `update()` (`server-task.cpp:1870`) — вытеснение по лимиту байт/токенов, FIFO по списку.

### 1.3 Resident state слота (L0/L1) и выбор слота
* `server_slot::prompt_save / prompt_load / prompt_clear` — `server-context.cpp:299-340`.
  `prompt_save()` считает `llama_state_seq_get_size_ext()` и кладёт байты в RAM-кэш;
  `prompt_clear()` делает `mem.seq_rm(id, -1, -1)` и чистит `prompt`.
* Выбор слота — `server_context::get_available_slot()` (`server-context.cpp:~1540-1665`), три уровня:
  1. явный `task.id_slot`;
  2. **LCP-similarity** по resident-префиксам всех свободных слотов (`--slot-prompt-similarity`,
     default `0.1`; `f_sim = lcp / task_len`); при `f_keep < 0.5` помечает `update_cache = true`;
  3. LRU слот (`slot.t_last_used`), тоже `update_cache = true`.
  Далее при `update_cache`: `ret->prompt_save(*prompt_cache)` → `ret->prompt_load(*prompt_cache, task.tokens)`
  → `prompt_cache->update()`. Это **единственная существующая точка «сохранить/подобрать best state»** —
  сюда и подключается disk-уровень.
* Idle-слоты: при приходе новой задачи (`server-context.cpp:~2441`) при `--cache-idle-slots`
  все необрабатываемые слоты сохраняются в prompt cache, а при `kv_unified` ещё и очищаются
  (`prompt_clear()`, тег `[TAG_IDLE_SLOT_CLEAR]`).

### 1.4 Reuse префикса при обработке промпта
`server-context.cpp:~3217-3300`:
* `n_past = slot.prompt.tokens.get_common_prefix(input_tokens)` при `params.cache_prompt`
  (плюс обрезка по `alora_invocation_start`);
* опциональный chunk-reuse через KV shift (`n_cache_reuse`, требует `llama_memory_can_shift`),
  реализован через `mem.seq_rm/seq_add`;
* далее декодируется только суффикс `input_tokens[n_past..]`.

### 1.5 Checkpoints (важно для hybrid/recurrent)
* `common_prompt_checkpoint` — `common/common.h:1165`: `n_tokens`, `pos_min`, `pos_max`,
  `data_tgt`, `data_dft`, `data_spec` (состояние спекулятивного движка, напр. eagle3), `id_task`.
* Создаются `server_context::create_checkpoint()` (`server-context.cpp:2309`) при
  `--ctx-checkpoints > 0` (`n_ctx_checkpoints`), минимальный шаг `--checkpoint-min-step`
  (`checkpoint_min_step = 8192` по умолчанию, `common/common.h:631`).
* Хранятся **внутри `server_prompt.checkpoints`** и потому уже сейчас сохраняются/восстанавливаются
  вместе с RAM-кэш-записью (`server_prompt_cache_state::size()` учитывает `ckpt.size()`).
  ⇒ Disk-запись обязана содержать и их, иначе hybrid/recurrent prefix не будет восстановлен корректно.

### 1.6 Сериализация состояния
* API: `llama_state_seq_get_size_ext / get_data_ext / set_data_ext / save_file / load_file`
  (`include/llama.h:869-937`), реализация `src/llama-context.cpp:3082-3330`, обёртки `4191-4235`.
* Поток данных: `state_seq_write_data()` → `llama_memory_*`-модули; для hybrid —
  `llama_memory_hybrid::state_write()` (`src/llama-memory-hybrid.cpp:190`) пишет **и attention, и
  recurrent** (`llama_memory_recurrent::state_write`, `src/llama-memory-recurrent.cpp:766`).
  ⇒ GDN/DeltaNet-состояние уже входит в seq-state; отдельного «второго» стейта для сохранения нет.
* `llama_context::state_write_data()` пишет строку `llm_arch_name(model.arch)`; при чтении
  несовпадение архитектуры → исключение. Это **единственная** встроенная проверка совместимости
  состояния (см. §3).
* **Есть настоящий потоковый файловый API**: `llama_state_seq_save_file()`
  (`src/llama-context.cpp:3254`) — `llama_file` + `llama_io_write_file` (класс `llama-context.cpp:2685`),
  формат: `u32 LLAMA_STATE_SEQ_MAGIC, u32 LLAMA_STATE_SEQ_VERSION, u32 n_token_count,
  llama_token[n_token_count], <state_seq_write_data>`. Пишет **сразу в файл, без промежуточного
  гигабайтного буфера**. `llama_state_seq_load_file()` — симметрично.
  Ограничение: этот API не умеет draft-state и checkpoints (только target seq-state) — надо расширять.
* Неприятный факт: `llama_state_seq_get_data_ext()`/`set_data_ext()` — host-буфер (`llama_io_write_host`),
  т.е. путь RAM-кэша сейчас **обязательно** создаёт полную копию состояния в RAM
  (для 150k-промпта Qwen3.8-27B это единицы ГБ). Это ровно то, чего ТЗ просит не делать для disk-пути.
* Абстракции `llama_io_write_i/read_i` (`src/llama-io.h`) — расширяемые: сюда можно добавить
  file/draft/checkpoint-секции без ломки существующих вызовов.

### 1.7 Модель/метаданные/метрики/CLI
* Идентичность модели: `llama_model_meta_val_str / meta_count / meta_key_by_index / meta_val_str_by_index`,
  `llama_model_desc`, `llama_model_size`, `llama_model_n_params` (`include/llama.h:619-647`).
  Готового «fingerprint hash» нет — нужно строить (по GGUFKV + hparams + KV-типы).
* Метрики: `server_metrics` (`tools/server/server-common.h:442`) — `bucket prompt/predict`,
  `n_prompt_cached`, счётчики draft. Точка расширения для disk-метрик.
* CLI: `-cram/--cache-ram` (`common/arg.cpp:1713`, MiB, default **8192**, `-1` = без лимита, `0` = выкл.),
  `--cache-idle-slots` (`arg.cpp:1721`), `--cache-prompt/--no-cache-prompt` (`arg.cpp:3561`),
  `-sps/--slot-prompt-similarity` (`arg.cpp:3783`), `-ctxcp/--ctx-checkpoints`,
  `--checkpoint-min-step` (`arg.cpp:1700`). Все — через `common_arg` + `set_env("LLAMA_ARG_*")` +
  `set_examples({LLAMA_EXAMPLE_SERVER})`; этим же механизмом добавляются новые `--cache-disk*`.
* Тесты сервера: `tools/server/tests/` (pytest: `unit/`, `tests.sh`, `conftest.py`) — подходящая площадка
  для интеграционных тестов disk-кэша.

---

## 2. Точки интеграции (минимум вмешательства)

| Что | Где | Действие |
|---|---|---|
| Выбор state при старте задачи | `server_context::get_available_slot()` | после `prompt_load` из RAM-кэша — попытка disk-candidate, cost-сравнение resident vs disk |
| Сохранение состояния | `server_slot::prompt_save()`, idle-путь `server-context.cpp:~2441` | дополнительно класть запись в disk-очередь (асинхронная запись) |
| RAM-вытеснение | `server_prompt_cache::update()` | перед вытеснением — «не потерять»: отдать запись в disk-кэш |
| Метаданные совместимости | старт `server_context::load_model()` (`~1359`) | построить namespace-fingerprint, открыть/проверить manifest, загрузить index |
| Метрики | `server_metrics` + `/metrics` | добавить disk_* счётчики |
| CLI | `common/arg.cpp` | `--cache-disk`, `--cache-disk-size`, `--cache-disk-read-mbps`, `--cache-ram` (см. §6) |

Новых механизмов prefix-matching не вводим: disk-запись хранит **тот же** `server_tokens` и
проверка кандидата идёт через `server_tokens::get_common_prefix()` (точное сравнение токенов —
обязательно перед restore, чтобы хеш-коллизия не могла подсунуть чужой state).

---

## 3. Namespace / fingerprint

Композиция (все компоненты — из фактического кода):

1. `state_seq` формат: `LLAMA_STATE_SEQ_MAGIC/VERSION` (из `llama-context.cpp`) — при смене версии
   старый namespace недействителен;
2. строковая архитектура + hparams: `llm_arch_name(model.arch)`, block_count, n_embd, n_head, n_head_kv,
   n_ctx_train (из GGUFKV `general.architecture` / `*.block_count` и т.п.);
3. GGUFKV identity: `general.name`, `general.file_type`, `general.quantization_version`, `general.size_label`;
4. KV-конфигурация: `type_k`/`type_v` (⇒ q8_0/q8_0 ≠ q4_0/q4_0), `flash_attn`, `kv_unified`,
   `swa_full` — только те, что реально входят в layout состояния;
5. recurrent/hybrid: признак наличия recurrent-модуля + его размерности (входят в state),
   число checkpoint-слотов (`n_ctx_checkpoints`) и `checkpoint_min_step` — влияют на содержимое записи;
6. спекулятивный движок: тип (`--spec-type`) и (для сохранённого `data_spec`/draft-state) его версия;
7. LoRA/adapters: список применённых адаптеров (путь + sha256 файла) — hidden/KV зависят от весов;
8. версия нашей disk-cache-структуры (собственный `format_version`).

Fingerprint = `sha256` от канонизированной строки выше (без чтения гигабайтного payload;
GGUF-хиши берём при загрузке модели через `llama_model_meta_val_str`).
Каталог: `<root>/<human-name>-<fp-12hex>/{manifest.json, index.bin, states/…}`.

Никаких «на авось»: при отсутствии/несовпадении manifest запись не читается, namespace создаётся заново;
старые файлы не удаляются.

---

## 4. Формат disk-записи

```
states/<prefix_hash16>-<n_tokens>.meta    # маленький self-describing header (магic, version, fp, n_tokens, payload_size, sha256 первых/последних блоков, флаги)
states/<prefix_hash16>-<n_tokens>.bin     # непрерывный payload: [main state][draft state][checkpoints]
states/*.tmp                              # незавершённые записи (игнорируются/удаляются при скане)
index.bin                                 # компактный бинарный индекс (mmap-able): записи {prefix_hash, n_tokens, file_size, last_used, created, hits, payload_offset, flags}
```

* Запись атомарна: `*.tmp` → flush → `rename()` → только потом обновление index (index crash-safe:
  его всегда можно пересобрать сканом `*.meta` — payload не читается).
* Index: цепочка **префиксных хешей с растущей детализацией** (например, хеш каждых 256 токенов,
  как в chunk-подходе), поиск кандидатов = самый длинный совпавший chunk-префикс, затем **точная**
  проверка токенов по `.meta`/sidecar перед restore. Полные 150k-массивы токенов для каждой записи в
  RAM не держим (компактный sidecar + mmap).
* Заголовок записи не дублирует уже существующую в файле мета-информацию `state_seq` (magic/version
  остаются внутри payload), а добавляет то, чего там нет: fingerprint namespace, n_tokens, размеры,
  чек-суммы, сведения о checkpoint-секциях.

## 5. Cost model

```
t_resident = (n_prompt − lcp_resident) / tps_prefill
t_disk     = restore_bytes / read_bps + (n_prompt − lcp_disk) / tps_prefill + c_overhead
use disk   ⟺ t_disk * (1 + margin) < t_resident   (margin default 0.05, пока настраиваемый)
```
* `read_bps` — из `--cache-disk-read-mbps` (default **200 MB/s**, десятичные МБ — так и в help).
* `tps_prefill` — EMA по фактическим prompt-eval таймингам сервера (в коде это
  `slot.stats.prompt_ms/n_prompt_tokens`, `slot.t_stats`), с override `--cache-prefill-tps`;
  до накопления статистики — консервативная оценка + правило «resident выигрывает при неопределённости»
  (ТЗ §12). Никаких захардкоженных 500 tok/s.
* Минимальный выигрыш: `--cache-disk-min-gain-ms` (default ≈ 1000 ms), чтобы не читать гигабайты
  ради десятка токенов.

## 6. CLI (проектные конвенции: MiB/байты, env LLAMA_ARG_*, только LLAMA_EXAMPLE_SERVER)

| Флаг | Default | Смысл |
|---|---|---|
| `--cache-disk PATH` | нет (disk-кэш выключен) | корень persistent-кэша; у нас `/mnt/3tb/llama-cache` |
| `--cache-disk-size N` | 500 GiB | лимит **всего** корня (сумма по всем namespace), суффиксы по конвенциям проекта (GiB) |
| `--cache-disk-read-mbps N` | 200 | оценочная скорость последовательного чтения (десятичные МБ/с) |
| `--cache-disk-min-gain-ms N` | 1000 | минимальный ожидаемый выигрыш |
| `--cache-prefill-tps N` | 0 (авто) | override оценки prefill tok/s |
| `-cram/--cache-ram` | **8192 MiB (как сейчас)**; 32 GiB применяется, когда задан `--cache-disk` и `-cram` не указан явно | RAM-лимит |

Обоснование по §14 ТЗ: менять дефолт `-cram` глобально нельзя (backward-compat), поэтому 32 GiB —
дефолт «multi-tier»-конфигурации (активируется вместе с `--cache-disk`), а не всей программы.

## 7. Concurrency / crash-safety / события

* Один mutex только на index/refcount; payload читается/пишется **без** глобального лока.
* Refcount + `lifecycle` (ACTIVE_READ / PENDING_DELETE): eviction не удаляет читаемую запись.
* Дедупликация: ключ = (fingerprint, prefix-hash, n_tokens); повторная запись того же состояния — no-op
  (сравнение по exact-токенам/размеру, не по одному хешу).
* Запись — в фоне (отдельный worker-thread сервера), не на критическом пути запроса; корректное
  завершение при shutdown (дренаж очереди, `*.tmp` остаются невидимыми).
* Обновление `last_used` — только в index (маленькая запись), **никогда** не перезапись payload.

## 8. Этапы и изменяемые файлы

1. **Stage 2**: `tools/server/server-disk-cache.{h,cpp}` (namespace, manifest, index, атомарная запись),
   CLI в `common/arg.cpp` + поля в `common/common.h`, init в `server-context.cpp:load_model()`, тесты рестарта.
2. **Stage 3**: save/load состояния в файл (stream), расширение сериализации
   (`src/llama-context.cpp`/`src/llama-io.h` — file-sink с draft/checkpoint-секциями), тест multi-GB/RAM-peak.
3. **Stage 4**: prefix-lookup в `get_available_slot()` + save-хуки (`prompt_save`, idle-путь).
4. **Stage 5**: cost-model + метрики (`server_common.h` `server_metrics`).
5. **Stage 6**: eviction/LRU + concurrency/lifecycle + crash-recovery (rebuild index).
6. **Stage 7**: pytest-сценарии A–L (`tools/server/tests/`), bench-скрипт, README/справка.

Репозиторий на стенде: `~/llamacpp-diskcache/llama.cpp` (sync из контейнера),
кэш: `/mnt/3tb/llama-cache`, сборка и прогоны — на 3090.
