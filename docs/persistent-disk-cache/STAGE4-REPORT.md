# Stage 4+5 — врезка persistent disk-кэша в жизненный цикл llama-server (отчёт)

Дата: 15.09.2026. База: master `38a5b42d9` + Stage 2 `0dd1409` + Stage 3 `b7dda21`.
Стенд: `user@stand.local` (RTX 3090 24 GB, 24 ядра, 62 GB RAM), модель
`~/models/Qwen3.8-27B-UD-Q3_K_XL.gguf`, кэш `/mnt/3tb/llama-cache`.

## 1. Изоляция (шаг 0)

```
$ git worktree add -b stage4 /home/node/.openclaw/workspace/llama-disk-cache/llama-stage4 master
Preparing worktree (new branch 'stage4')
HEAD is now at b7dda21 WIP: disk cache stage 3 - streamed save/load of the sequence state
$ git -C llama.cpp worktree list            # после коммита Stage 4+5
.../llama-disk-cache/llama.cpp     b7dda21 [master]
.../llama-disk-cache/llama-stage4  f1a15ac [stage4]
```

Вся правка — только в worktree. master не тронут (проверка: `git -C llama.cpp log --oneline -1` = `b7dda21`).
На стенде — новый каталог `~/llamacpp-diskcache-stage4/llama.cpp`, сборка `build-s4`;
каталог другой сессии `~/llamacpp-diskcache` и его `build-base` не трогались.

Ветка `stage4`, коммит:

```
f1a15ac WIP: disk cache stage 4+5 - slot selection (resident vs disk), save hooks,
        RAM-eviction handover, cost model, metrics
b7dda21 WIP: disk cache stage 3 - streamed save/load of the sequence state
```

```
$ git diff master...stage4 --stat
 tools/server/server-common.h       |  15 ++
 tools/server/server-context.cpp    | 432 ++++++++++++++++++++++++++++++++++++-
 tools/server/server-disk-cache.cpp | 126 +++++++++++
 tools/server/server-disk-cache.h   |  63 ++++++
 tools/server/server-task.cpp       |  67 ++++++
 tools/server/server-task.h         |   6 +
 6 files changed, 708 insertions(+), 1 deletion(-)
```

## 2. Точки врезки (файл:строка в коммите `f1a15ac`)

| Что | Где |
|---|---|
| `server_prefix_candidate` (RESIDENT/DISK, match, bytes, t_*) | `tools/server/server-disk-cache.h:162-175` |
| cost-model: константы, `server_disk_cache_cost`, `estimate`, `prefer_disk` | `server-disk-cache.h:177-210`, реализация `server-disk-cache.cpp:1479-1584` |
| `entry_info()` (index-запись без чтения payload) | `server-disk-cache.cpp:1332` |
| упаковка checkpoint (tgt/dft/spec) в один blob | `server-context.cpp:246` (`dc_ckpt_pack`), `:283` (`dc_ckpt_unpack`) |
| `on_evict` у RAM-кэша + вызовы перед удалением | `server-task.h:630`, `server-task.cpp:1809, 1933, 1953` |
| cost-модель в контексте (`disk_cost`) | `server-context.cpp:1184` |
| glue: `disk_cache_enabled/make_extra/save_slot/save_evicted/try_restore` | `server-context.cpp:1729-2012` |
| **save-хук №1** (перед `prompt_load`) | `server-context.cpp:2118` (`get_available_slot`) |
| **lookup + restore** (после `prompt_cache->update()`) | `server-context.cpp:2131` → `disk_cache_try_restore` |
| **save-хук №2** (idle-слоты, перед `prompt_clear()`) | `server-context.cpp:2924` |
| подключение `on_evict` (при создании RAM-кэша) | `server-context.cpp:1534-1537` |
| наблюдение prefill-tps (переход DONE_PROMPT → GENERATING) | `server-context.cpp:4325` |
| метрики в `server_metrics` | `server-common.h:485-497` |
| вывод в `/metrics` | `server-task.cpp:1566-1640`; gauge `index_entries` — `server-context.cpp:2998` |

Порядок в `get_available_slot()` (как в коде):

```
ret->prompt_save(*prompt_cache);                 // как было
disk_cache_save_slot(*ret, "slot update");       // НОВОЕ: старое состояние слота -> диск
ret->prompt_load(*prompt_cache, task.tokens);    // как было (RAM-кандидат)
prompt_cache->update();                          // как было
disk_cache_try_restore(*ret, task);              // НОВОЕ: resident vs disk + restore
```

Весь restore/save — в главном потоке сервера (очередь задач разбирается в `update_slots()`,
там же где `llama_decode`); небезопасных обращений к `llama_context` из других потоков нет.
Никакого параллельного inference-пути: после restore работа идёт обычным путём
(`n_past = slot.prompt.tokens.get_common_prefix(input_tokens)`, `server-context.cpp:~3897`).

## 3. Как выбирается resident vs disk (фактические условия в коде)

1. Кандидат disk берётся из `disk_cache->find(task.tokens.get_tokens(), cand)` — модуль отдаёт
   запись, чей массив токенов **точно** является префиксом запроса (поэлементная проверка,
   `server-disk-cache.cpp:1337-1380`), поэтому `n_tokens_match == n_tokens_state` (никаких «обрезаний»).
2. Resident — то, что лежит в слоте после `prompt_load`: `n_tokens_match = prompt.tokens.get_common_prefix(task.tokens)`.
3. Если `disk.n_tokens_match <= resident.n_tokens_match` → disk отвергается (его хвост не короче).
4. Иначе (`server_prefix_candidate_prefer_disk`, `server-disk-cache.cpp:1513`):

```
tps_known (tps > 0):
    t_restore = restore_bytes / read_bps + t_restore_fixed      // read_bps = --cache-disk-read-mbps * 1e6
    t_tail    = (n_prompt - n_tokens_match) / tps
    t_total   = t_restore + t_tail
    use_disk ⟺ t_total_disk * (1 + 0.05) < t_total_resident
               и (t_total_resident - t_total_disk)*1000 ≥ --cache-disk-min-gain-ms
tps НЕ известен (ТЗ §12):
    if resident.n_tokens_match * 100 ≥ n_prompt * 90   -> disk отвергается
    иначе считается по консервативному floor 20 t/s (policy, не замер):
       gain = ((n_prompt - resident.match) - (n_prompt - disk.match))/20 - t_restore
       disk берётся, если gain*1000 ≥ min_gain_ms
```

5. При отказе disk: `disk_cache_resident_preferred++`, TRACE-строка с причиной, слот не трогается.
6. При победе disk: `disk_cache->load(ctx_tgt, slot.id, id, &extra)` → токены через `tokens_of()`,
   checkpoints из blob'ов, `extra.dft` → `llama_state_seq_set_data_ext(ctx_dft, …)` (как это делает
   RAM-путь), `extra.spec` → `common_speculative_set_state()`, затем `touch(id)`, счётчики
   `restores/restore_bytes/restore_seconds/disk_preferred/saved_prefill_tokens`.
   При ошибке restore: `corrupt_entries++`, запись удаляется, слот чистится (`prompt_clear`) —
   запрос продолжается полным prefill (частично перезаписанному состоянию доверять нельзя).
7. Мультимодальные запросы (`task.tokens.has_mtmd`) disk-кандидата не получают — медиа-чанки в
   записи не хранятся (Stage 6).

### Save-точки (ТЗ §18: не на каждый токен)

* `disk_cache_save_slot(..., "slot update")` — сразу после `prompt_save()`, т.е. до того, как
  `prompt_load` заменит состояние слота (сохраняется ровно то состояние, которое уходит из слота);
* `disk_cache_save_slot(..., "idle slot")` — в idle-цикле `--cache-idle-slots`, перед `prompt_clear()`;
* `disk_cache_save_evicted()` — RAM-вытеснение по лимиту (`alloc()`/`update()`) отдаёт готовые байты
  через `save_raw()` (без доступа к ctx). Вытеснение «запись полностью содержится в новом промпте»
  колбэк не вызывает: это не потеря (новое состояние перекрывает её и сохраняется своим хуком).
  Если колбэк не выставлен (`on_evict` пуст) — RAM-кэш ведёт себя ровно как раньше.

## 4. Откуда берётся tps и что с `t_restore_fixed`

* `tps = --cache-prefill-tps` (если > 0), иначе EMA (α = 0.2) по фактическим prompt-таймингам:
  `disk_cost.observe(slot.stats.n_prompt_processed, slot.stats.t_prompt_ms()/1e3)` в точке перехода
  `SLOT_STATE_DONE_PROMPT → SLOT_STATE_GENERATING` (`server-context.cpp:4325`), т.е. один замер на
  запрос. EMA обновляется только если замер покрыл ≥ 32 токенов
  (`SERVER_DISK_CACHE_TPS_MIN_TOKENS`, `server-disk-cache.h:183`): у полностью восстановленного
  промпта декодируется 1–2 токена, и такая «скорость» — чистый шум. До первого замера EMA = 0
  («неизвестно») → работает ветка floor.
* `t_restore_fixed = 0` и **помечен как «не измерено»** (`SERVER_DISK_CACHE_T_RESTORE_FIXED_SEC`,
  `server-disk-cache.h:176`): фиксированной накладной стоимости restore мы не мерили, поэтому она
  не может прятать реальную цену. Измеренный restore (0.840 s на 1.756 GiB) уже включает всё, но
  отделить «фиксированную» часть от потоковой по этим данным нельзя.
* Прочие константы помечены как policy, не замер: `margin 0.05`, `floor 20 t/s`, `read_bps` — из CLI.

## 5. Выдержки логов (фактические строки)

### Прогон 1 → сохранение (хук «slot update»), сервер 1

```
0.04.878.222 I srv    load_model: disk cache: fingerprint = 5a0d8e6fc7d2bf049085d1a63818ad1f9997c1eec3c30cb6822a602453cbcd85
0.04.878.226 I srv    load_model: disk cache: 0 states, 0.0 MiB on disk; RAM cache limit = 32768 MiB
0.49.701.227 I disk cache: stored state id=1, 39341 tokens, 1756.17 MiB (1456.92 MiB state + 299.25 MiB extra)
0.49.701.236 I srv  disk_cache_s: disk cache: saved state tokens=39341 id=1 bytes=1841481492 in 1256.6 ms (slot update)
0.52.217.937 I disk cache: stored state id=2, 1699 tokens, 505.34 MiB (206.08 MiB state + 299.25 MiB extra)
0.52.217.946 I srv  disk_cache_s: disk cache: saved state tokens=1699 id=2 bytes=529883644 in 336.5 ms (idle slot)
0.47.025.553 I slot print_timing: id  0 | task 0 | prompt eval time =   40817.83 ms / 39341 tokens (    1.04 ms per token,   963.82 tokens per second)
```

### Рестарт → загрузка индекса, сервер 2

```
0.04.845.578 I disk cache: loaded index: 2 states, 2261.5 MiB
```

(в этом же прогоне `load_model: disk cache: 2 states, 2261.5 MiB on disk; RAM cache limit = 32768 MiB` )

### Прогон 2 → кандидат, решение, restore, тайминги

```
0.06.246.658 I srv  disk_cache_t: disk cache: disk state preferred (disk: 39341 tokens, 1756.17 MiB;
                resident: 0 tokens): prefill tps unknown, floor 20.0 t/s: est. gain 1957843 ms >= 1000 ms
0.07.277.495 I srv  disk_cache_t: disk cache: restored 39341 tokens in 0.840s (1756.17 MiB, 2191.7 MB/s)

0.12.166.935 I slot print_timing: id  3 | task 0 | prompt processing, n_tokens =   4096, progress = 0.81,
                t =   4.82 s / 850.18 tokens per second
0.27.449.297 I slot print_timing: id  3 | task 0 | prompt eval time =   18876.60 ms / 14544 tokens
                (    1.30 ms per token,   770.48 tokens per second)
```

(progress = 0.81 при n_tokens = 4096 из 14544 — это ровно хвост: 39341 токен уже в слоте из disk-записи.)

### Прогон 3 (без `--cache-disk`), тот же промпт

```
1.06.937.281 I slot print_timing: id  3 | task 0 | prompt eval time =   59694.31 ms / 53885 tokens
                (    1.11 ms per token,   902.68 tokens per second)
1.48.168.324 I slot print_timing: id  3 | task 61 | prompt eval time =   41185.62 ms / 39341 tokens
                (    1.05 ms per token,   955.21 tokens per second)
```

(`disk cache`-строк в логе этого сервера нет вообще — `phase3_log: []`.)

### Измеренные числа

| величина | прогон 1 (кэш) | прогон 2 (restore) | прогон 3 (без кэша) |
|---|---|---|---|
| промпт, токенов | 39 341 | 53 885 | 53 885 |
| `timings.prompt_n` (реально декодировано) | 39 341 | **14 544** (хвост) | 53 885 |
| `prompt_ms` | 40 817.8 (40.818 s) | **18 876.6 (18.877 s)** | 59 694.3 (59.694 s) |
| t/s префилла | 963.8 | 770.5 (по хвосту) | 902.7 |
| восстановлено из disk | — | 39 341 токен | — |
| ответ (temp 0, seed 42), sha256(content)[:16] | `e5c11a4b1d1d1876` (1 токен) | `886a9795b4c80df6` (32 токена) | `886a9795b4c80df6` |

* **speedup** (один и тот же промпт 53 885 токенов): 59.694 s / 18.877 s = **3.16×**;
  из 39 341 «дисковых» токенов префилл не делался вовсе.
* **коэффициент восстановления**: 1 841 481 492 B за 0.840 s = 2191.7 MB/s (файл только что записан
  этой же ОС → горячий page cache; для «холодного» диска цифра будет ниже, и cost-model считает по
  `--cache-disk-read-mbps 200`, а не по измеренной). холодное чтение здесь не мерилось.
* **корректность**: содержимое прогона 2 совпало с прогоном без кэша **бит-в-бит**
  (`correctness_content_equal: true`, `run2_content_sha == run3_content_sha` = `886a9795b4c80df6`) —
  состояние из disk-записи даёт тот же результат, что полный префилл (temperature 0, seed 42).
* **регрессия «кэш выключен»**: тот же промпт без `--cache-disk` = 59.694 s (базовая линия полного
  префилла); на 39 341-токенном промпте с включённым кэшем (записи ещё нет) 40.818 s против 41.186 s
  без кэша (разница 0.9 %) — включённый disk-кэш сам по себе префилл не замедляет.
* **метрики** `/metrics` после прогона 2 (строки без `#`):
  `disk_cache_hits_total 1`, `misses 0`, `restores_total 1`, `restore_bytes_total 1.84148e+09`,
  `restore_seconds_total 0.840225`, `resident_preferred_total 0`, `disk_preferred_total 1`,
  `saved_prefill_tokens_total 39341`, `index_entries 2`, `writes_total 0`, `corrupt_entries_total 0`.
  (`writes_total 0` — запись была в предыдущем процессе, счётчики сервера cumulative-per-process.)

### Отдельный прогон cost-model с известным tps (`stage4_cost.py`)

Три запроса в одном процессе, каждый на свой слот (resident пуст → решение принимается по disk-кандидату):

```
A: 29 741 токен за 29.498 s = 1008 t/s          (заполняет EMA; кандидата нет)
B: 31 084 токен за 31.089 s = 1000 t/s          (при запуске B состояние A уходит на диск) -> EMA = 1006.4 t/s
C: 32 427 токен, слот пуст, disk-кандидат 29 741 токен
0.38.039.054 I disk_cache_s: disk cache: saved state tokens=29741 id=5 bytes=1506979092 in 1021.4 ms (idle slot)
1.09.369.478 I srv disk_cache_t: disk cache: disk state preferred (disk: 29741 tokens, 1437.17 MiB;
               resident: 0 tokens): t_total disk 10.203 s vs resident 32.214 s (est. gain 22011 ms >= 1000 ms)
1.10.245.255 I srv disk_cache_t: disk cache: restored 29741 tokens in 0.685s (1437.17 MiB, 2200.8 MB/s)
C фактически: prompt_n = 2686, prompt_s = 3.962 s   (вместо 32.4 s полного префилла)
```

Формула воспроизводит лог точно: EMA после A и B = 1006.4 t/s; resident = 32 427/1006.4 = 32.214 s;
disk = 1 506 979 092/2e8 = 7.535 s + (32 427−29 741)/1006.4 = 2.669 s → 10.203 s; с margin 5 % = 10.713 s < 32.214 s.
(В строке лога печатается `t_total` **без** margin — сам margin применяется до сравнения.)

### Что лежит на диске (namespace `qwen3.8-27b-5a0d8e6fc7d2`)

```
1.bin 1841481492  1.meta   157564    (39 341 токен, "slot update" + хук idle)
2.bin  529883644  2.meta     6996    (1 699 токен,  "idle slot" — дивергентный запрос)
3.bin 2663121688  3.meta   215864    (53 916 токен, "idle slot" — прогон 2, 598.50 MiB extra)
4.bin  157034168  4.meta      216    (4 токена,    "idle slot")
итого 4.9 GB (по 4 записям), index.bin — только метаданные
```

Скорость записи (из тех же логов): 1256.6 ms на 1 841 481 492 B (1465 MB/s), 336.5 ms на 529 883 644 B
(1575 MB/s), 1775.8 ms на 2 663 121 688 B (1500 MB/s) — в главном потоке сервера.

## 6. Что осталось

**Stage 6** (не делалось):
* eviction/LRU: `evict_lru()`/`enforce_limit()` в модуле есть, но политика (что вытеснять, порог
  минимального размера записи) не выверена. Наблюдение: сохраняется и мусорно-мелкое состояние —
  запись `4.bin` на 4 токена занимает 157 MiB (полное recurrent-состояние), гейта «не писать < N токенов» нет.
* concurrency/lifecycle: refcount/ACTIVE_READ/PENDING_DELETE, второй writer не защищён — сейчас
  запись/чтение payload вне лока, индекс под mutex. Параллельный второй сервер на тот же namespace
  не проверялся (на стенде одновременно живёт один сервер).
* crash-recovery: `*.tmp` чистятся, `rebuild_index()` есть; сценарии падения в момент записи не гонялись.
* асинхронность: сейчас `save()` — синхронная потоковая запись в главном потоке: 0.34–1.78 s на
  0.53–2.66 GB состояния (видно в логах `saved state ... in N ms`). Это осознанный выбор плана (§4.4),
  но на длинных состояниях это пауза сервера; вынести в worker (`.tmp` в главном потоке, rename+индекс
  в worker'е) — Stage 6.
* медиа-чанки (mtmd): запись их не хранит, такие запросы кандидата не получают.

**Stage 7** (не делалось): pytest-сценарии A–L (`tools/server/tests/`), бенчмарк на 150k токенов,
замер `t_restore_fixed` (сейчас 0 = «не измерено»), README/справка.

## 7. Приложения

* Скрипты e2e: `llama-disk-cache/stage4/stage4_e2e.py` (save→рестарт→restore→correctness→регрессия),
  `stage4_cost.py` (cost-model с известным tps), `remote.py` (SFTP/exec на стенд), `build_stage4.sh`.
* Логи: `llama-disk-cache/stage4/logs/` (`s4-srv1.log`, `s4-srv2.log`, `s4-srv3.log`, `s4-e2e-result.json`,
  `s4-cost.log`, `s4-cost-result.json`).
* Бинарь: `~/llamacpp-diskcache-stage4/llama.cpp/build-s4/bin/llama-server` (сборка `BUILD_S4_DONE`,
  дерево на стенде без `.git` — это gitless sync §5 скилла `llama-stand-remote`).
* Провенанс: все 6 изменённых файлов на стенде побайтово совпадают с коммитом `f1a15ac`
  (`md5sum` локально и на стенде: OK для каждого из шести), e2e- и cost-прогоны в этом отчёте сделаны
  бинарём, собранным из этого дерева (`build-s4`).

### Оговорки (чтобы не выдать догадку за факт)

* `tokens_cached` в ответе `/completion` — **не** «токенов из кэша»: в этом дереве
  `res->n_tokens_cached = slot.prompt.n_tokens()` (`server-context.cpp:2594`), т.е. число токенов в
  состоянии слота в момент сборки ответа (в этих прогонах — на 1 меньше итоговой длины последовательности,
  напр. `total time = 41102.39 ms / 39342 tokens` при `tokens_cached 39341`). Критерий
  «tokens_cached > 0» выполняется, но доказательство restore — это `timings.prompt_n` (14 544 вместо
  53 885), строка `restored 39341 tokens in 0.840s` и метрика `saved_prefill_tokens_total 39341`.
* Скорость restore 2180–2200 MB/s измерена на горячем page cache этой же машины сразу после записи;
  холодное чтение не мерилось.
* `prompt_n`/`prompt_ms` прогонов 1/2/3 — из ответов сервера; t/s = prompt_n/(prompt_ms/1000).
* Отдельные замеры (0.34–1.78 s на запись) включают и запись checkpoint-секций (299 MiB на состояние).
* **RAM-вытеснение в диск (`on_evict` → `save_raw`) в прогонах не сработало**: лимит RAM-кэша был
  32 768 MiB, а суммарный размер состояний в RAM до него не доходил — в логах нет ни одной строки
  `making room` / `limit reached`. Хук подключён и вызывается (код + контракт «колбэк не задан →
  старое поведение»), но его рантайм-проверка — Stage 6/7 (маленький `--cache-ram` + несколько больших промптов).
* Из живых хуков в прогонах зафиксированы оба save-хука: `saved state ... (slot update)` и
  `saved state ... (idle slot)` в `s4-srv1.log`/`s4-srv2.log`.
* Всё измеренное — на одном стенде (3090, q8_0 KV, `-c 160000`), переносить на другое железо нельзя.
