# Измеренные результаты (по фактам со стенда stand.local, 15.09.2026)

Все числа — из логов стенда/прямых замеров, не оценки. Команды/логи указаны, чтобы можно было перепроверить.

## Stage 4 e2e: prefill → save → restart → restore (Stage 4-5 session)

Конфигурация сервера (`ps` на стенде, 21:11):
`llama-server -m ~/models/Qwen3.8-27B-UD-Q3_K_XL.gguf -c 160000 -ngl 99 -ctk q8_0 -ctv q8_0 -fa on
--cache-ram 32768 --cache-disk /mnt/3tb/llama-cache --cache-idle-slots --metrics`
Бинарь: `~/llamacpp-diskcache-stage4/llama.cpp/build-s4/bin/llama-server` (ветка `stage4`).

Прогон 1 (полный prefill, сохранение на диск; `/tmp/s4-srv1.log`):
```
disk cache: stored state id=1, 39341 tokens, 1756.17 MiB (1456.92 MiB state + 299.25 MiB extra)
disk_cache_s: disk cache: saved state tokens=39341 id=1 bytes=1841481492 in 1238.5 ms (idle slot)
```
Файлы: `states/1.bin` 1 841 481 492 B + `states/1.meta` 157 564 B.
⇒ ~46.8 КиБ на токен состояния (1 841 481 492 / 39 341) при q8_0 KV на этой hybrid-модели.

Рестарт сервера (`/tmp/s4-srv2.log`):
```
disk cache: loaded index: 1 states, 1756.2 MiB
load_model: disk cache: 1 states, 1756.2 MiB on disk; RAM cache limit = 32768 MiB
```

Прогон 2 (restore + суффикс):
```
disk_cache_t: disk cache: disk state preferred (disk: 39341 tokens, 1756.17 MiB; resident: 0 tokens):
              prefill tps unknown, floor 20.0 t/s: est. gain 1957843 ms >= 1000 ms
disk_cache_t: disk cache: restored 39341 tokens in 0.841s (1756.17 MiB, 2188.9 MB/s)
slot print_timing: prompt eval time = 18859.04 ms / 14544 tokens (771.20 tokens per second)
```
⇒ 39 341 токен пришли из disk-состояния, реально посчитан суффикс 14 544 токена (18.86 с при 771 t/s).

## Холодное чтение state-файла (мой независимый замер, `dd iflag=direct`)

```
file=/mnt/3tb/llama-cache/qwen3.8-27b-5a0d8e6fc7d2/states/1.bin size=1841481492
O_DIRECT (bypasses page cache): 1841481492 bytes copied, 9.98944 s, 184 MB/s
normal read (page cache):       1841481492 bytes copied, 0.298347 s, 6.2 GB/s
device: /dev/sda2 2.7T ... /mnt/3tb
```
⇒ **реальный HDD: 184 MB/s** (O_DIRECT, без page cache) → 1.84 GB состояния ≈ **10.0 с** холодного чтения.
Заявленный в cost-model default 200 MB/s отличается от измеренного на 8 % — дефолт адекватный.
⇒ наблюдённые `2188.9 MB/s` и `0.841 s` в restore — **page cache** (файл только что записан, кэш переживает
перезапуск процесса), а не HDD. Как холодный disk-read это подавать нельзя.

## Что ещё НЕ измерено / не проверено (на момент записи)
- ~~Холодный restore через сервер~~ — **измерен** 15.09.2026 21:34 UTC, см. раздел «ХОЛОДНЫЙ restore ЧЕРЕЗ СЕРВЕР» ниже.
- sha-сверка ответа прогона 2 против полного prefill (temp 0, seed 42).
- Регрессия «сервер без `--cache-disk`» на этом же бинаре.
- Поведение при переполнении лимита на живом сервере, конкурентные restore/eviction (Stage 6).
- `/metrics`: с `--metrics` эндпоинт отдаёт Prometheus-текст; e2e-скрипт сессии падал на `json.loads()` — это
  дефект скрипта (передано сессии), не модуля.

---

# Дополнение: полный e2e Stage 4 (сессия disk-cache-stage45), лог `/tmp/s4-e2e.log`, маркер `S4_E2E_DONE`

Конфиг: `-c 160000 -ngl 99 -ctk q8_0 -ctv q8_0 -fa on --cache-ram 32768 --cache-disk /mnt/3tb/llama-cache --cache-idle-slots --metrics`,
модель `Qwen3.8-27B-UD-Q3_K_XL.gguf`, бинарь `build-s4/bin/llama-server` (ветка `stage4`).

## Прогон 1 (без кэша, полный prefill)
`long_prompt_tokens=39341`, `prompt_ms=40632.669` (→ 40.633 s), `sha=e5c11a4b1d1d1876`.
Сохранения:
```
disk cache: stored state id=1, 39341 tokens, 1756.17 MiB (1456.92 MiB state + 299.25 MiB extra)
disk_cache_s: saved state tokens=39341 id=1 bytes=1841481492 in 1245.5 ms (slot update)
disk cache: stored state id=2, 1699 tokens, 505.34 MiB (206.08 MiB state + 299.25 MiB extra)
disk_cache_s: saved state tokens=1699 id=2 bytes=529883644 in 333.2 ms (idle slot)
```

## Рестарт (phase2 startup)
```
disk cache: loaded index: 2 states, 2261.5 MiB
load_model: disk cache: 2 states, 2261.5 MiB on disk; RAM cache limit = 32768 MiB
```

## Прогон 2 (тот же префикс + хвост → disk restore)
`prompt2_tokens=53885`, `prompt2_prefix_is_run1=true`
```
disk_cache_t: disk state preferred (disk: 39341 tokens, 1756.17 MiB; resident: 0 tokens): prefill tps unknown, floor 20.0 t/s: est. gain 1957843 ms >= 1000 ms
disk_cache_t: restored 39341 tokens in 0.845s (1756.17 MiB, 2180.2 MB/s)
```
`prompt_n=14544`, `prompt_s=18.799`, `tokens_cached=53916`, `predicted_per_s=24.52`, `sha=886a9795b4c80df6`.

## Корректность (главное)
`run3_content_sha = 886a9795b4c80df6`, `correctness_content_equal = true`, `correctness_sha_equal = true`
⇒ ответ после restore с диска **побитово совпал** с ответом на том же промпте по обычному пути.
Регрессия «без диска»: `run3b_nocache_short`: `prompt_n=39341`, `prompt_ms=41102.387` (41.102 s), `sha=e5c11a4b1d1d1876`
— совпадает с sha прогона 1 ⇒ выключенный disk-кэш воспроизводит исходное поведение.

## Метрики `/metrics` (Prometheus, с `--metrics`)
`disk_cache_hits_total=1`, `misses_total=0`, `restores_total=1`, `restore_bytes_total=1.84148e+09`,
`restore_seconds_total=0.844629`, `writes_total=0` (в этом окне), `evictions_total=0`, `corrupt_entries_total=0`,
`resident_preferred_total=0`, `disk_preferred_total=1`, `saved_prefill_tokens_total=39341`, `index_entries=2`.

## Сравнение (только измеренные величины)
- полный prefill 39 341 токенов: **40.633 s** (прогон 1);
- restore 39 341 токенов + prefill суффикса 14 544: **0.845 s + 18.799 s = 19.64 s** (прогон 2, тёплый page cache);
- холодная оценка по замеру `dd iflag=direct` (184 MB/s): restore ≈ **10.0 s**, итого ≈ 28.8 s.

---

# Наблюдение (код + лог): одиночный запрос НЕ сохраняет состояние

Проверено по коду ветки `stage4`: `disk_cache_save_slot()` вызывается ровно в двух точках —
`server-context.cpp:2118` (путь «slot update» в `get_available_slot`, т.е. при обработке СЛЕДУЮЩЕЙ задачи)
и `:2924` (idle-slot при приходе новой задачи). Это зеркало штатных `prompt_save()` в master
(`llama.cpp` master: 1706 «slot update», 2500 «idle slot»).
Экспериментально: после ОДНОГО `/completion` в логе нет ни `stored state`, ни `saved state`
(мой прогон `/tmp/cold_restore_srv1.log`, 21:26 UTC).

Следствие: сценарий ТЗ §58 «первый длинный request → cache state сохраняется» в текущем виде
не выполняется, если клиент сделал один запрос и сервер перезапустили — состояния на диске не будет.
Нужна точка сохранения по завершении задачи (или при освобождении слота), а не только по приходу следующей.
Пункт передан сессии Stage 4-5.

# Наблюдение: общий каталог кэша

`/mnt/3tb/llama-cache` использовался одновременно моим тестом и e2e-сессией; во время моего прогона
файл состояния исчез (`FileNotFoundError` на `states/7.bin`). Для своих замеров перешёл на отдельный корень
`/mnt/3tb/llama-cache-cold`. Тестам следует чистить только свой namespace.

---

# ХОЛОДНЫЙ restore ЧЕРЕЗ СЕРВЕР — измерено (`/tmp/cold_restore.out`, запуск 21:34:40 UTC)

Протокол: один длинный промпт → сохранение состояния (триггерится вторым, коротким запросом) →
останов → **выбивание файла состояния из page cache** (`sync` + `posix_fadvise(POSIX_FADV_DONTNEED)`,
без root) → рестарт сервера → тот же префикс + хвост.
Отдельный корень кэша: `/mnt/3tb/llama-cache-cold` (чтобы не пересекаться с тестами другой сессии).

```
phase1: prompt_n=31062, prompt_s=31.012
stored state id=1, 31062 tokens, 1481.06 MiB (1181.81 MiB state + 299.25 MiB extra)
saved state tokens=31062 id=1 bytes=1553008016 in 1049.6 ms (idle slot)
state_file=/mnt/3tb/llama-cache-cold/qwen3.8-27b-5a0d8e6fc7d2/states/1.bin  (1 553 008 016 B)
dd iflag=direct: 1553008016 bytes copied, 8.50702 s, 183 MB/s      <- устройство, без page cache

после рестарта (page cache выбит):
loaded index: 1 states, 1481.1 MiB
disk cache: disk state preferred (disk: 31062 tokens, 1481.06 MiB; resident: 0 tokens):
            prefill tps unknown, floor 20.0 t/s: est. gain 1545335 ms >= 1000 ms
disk cache: restored 31062 tokens in 8.745s (1481.06 MiB, 177.6 MB/s)

phase2: prompt_n=29 (только хвост), prompt_s=0.480, tokens_cached=31094
```

Сопоставление (все величины измерены, кроме явно помеченного прогноза):
* устройство: 183 MB/s → 1.553 GB = 8.49 s; путь restore через сервер: **8.745 s (177.6 MB/s)** — совпадение в пределах 3 %, т.е. restore идёт на скорости устройства, без лишних копий;
* мой ранний прогноз по 184 MB/s давал ~8.4 s — сошёлся;
* фаза промпта: полный prefill 31.012 s против «restore 8.745 s + хвост 0.480 s = 9.225 s» ⇒ **×3.36**.

Оговорка: `posix_fadvise(DONTNEED)` выбивает страницы файла, но не гарантирует «холодный» диск
на 100 % (часть страниц могла остаться/перечитаться из кэша ОС); скорость 177.6 MB/s практически равна
O_DIRECT-замеру 183 MB/s, поэтому расхождение с «настоящим холодным» чтением здесь в пределах шума.

---

# Открытый пункт к слиянию: `cache_prompt=false` не учитывается (проверено 15.09.2026 21:5x UTC)

Проверено по коду, не исправлено ни в одном доступном дереве:
* локальная ветка `stage4` = `f1a15ac` (`git status` чистый), `llama-disk-cache/llama-stage4/tools/server/server-context.cpp:2115-2133`:
  `disk_cache_try_restore()` вызывается под условием только `task.type == SERVER_TASK_TYPE_COMPLETION`;
* дерево на стенде `~/llamacpp-diskcache-stage4/llama.cpp` — тот же код (`:2131`), сборка `build-s4/bin/llama-server` от 21:24,
  признаков правки гейта нет.

Штатный reuse в master гейтится ещё и `params.cache_prompt` (`server-context.cpp:3701`), наш путь — нет.
Следствие: клиент с `"cache_prompt": false` всё равно получает восстановление состояния с диска (чтение ~1.5 ГБ).
Статус: **незакрытый пункт**; блокирует слияние, пока не добавлен гейт по `params.cache_prompt`.

Второй незакрытый пункт — одиночный запрос не сохраняет состояние (см. выше, точка сохранения только
«slot update» / idle-слот). Оба пункта переданы сессии Stage 4-5.

---

# Дополнение 15.09.2026 23:40 UTC: Stage 6–7, совпадение по общему префиксу, закрытые пункты

## Закрыто из списков выше

- `cache_prompt=false` больше не получает состояние с диска — гейт добавлен (коммит `b70754784`).
- Одиночный запрос сохраняется: запись отложена до конца обработки слота в главном цикле
  (`slot.disk_save_pending`), второй запрос не нужен (тот же коммит).
- Регрессия «сервер без `--cache-disk`»: был **SIGSEGV** (exit 139) в момент завершения prefill —
  `disk_cache->config()` читался без гварда `disk_cache_enabled()` при нулевом указателе.
  Исправлено `2de9b63e7`; после пересборки тест на сервере без диска проходит (`server alive`, выход 0).
- Вытеснение по лимиту, конкурентность restore+eviction, холодный restore — измерены, см. ниже.

## Совпадение по наибольшему общему префиксу (коммит `0e804250e`)

Было: кандидатом становилась только запись, которая **целиком** является префиксом запроса
(`find()`), поэтому повтор того же промпта после генерации промахивался — запись длиннее запроса
(9128 токенов промпта + 35 сгенерированных = 9163).

Стало: `find_best()` берёт запись с наибольшим общим префиксом (запись может быть длиннее запроса),
состояние грузится целиком, лишнее срезает штатный путь — как `server_prompt_cache::load` в RAM-кэше
(`prompt = std::move(it_best->prompt)`, далее `n_past = get_common_prefix(...)` и `mem.seq_rm`).
Гейт `--cache-disk-min-tokens` теперь считается по переиспользуемому префиксу.

### Измерения на стенде (RTX 3090, кэш на SATA HDD)

* selftest: `SELFTEST_OK`; bulk-вытеснение 300 записей — 14.7 ms; `find_best` — **1.522 ms на вызов
  при 300 записях** (линейно по размеру индекса, платится на каждый запрос).
* L-сценарий с честным референсом (сервер **без** диска): фаза промпта **10 998.5 ms → 193.7 ms**
  (×56.8), wall 1.95 s, состояние 849.27 MiB восстановлено за 0.426 s (2089.3 MB/s), вывод
  **бит-в-бит** совпал с референсом (`sha cbf2d04f972bcfa6`). 2089 MB/s — тёплый page cache;
  холодная оценка по `dd iflag=direct` (181 MB/s) — ≈4.7 s.
* Бенч 116 896 токенов: прогон 1 — `prompt_ms 163 340.9` (wall 163.87 s), состояние
  `4 543 807 912 B` записано за 3106.6 ms; после рестарта — `prompt_ms 231.8` (wall 2.7 s),
  `restored 116896 tokens in 1.896s` (2396.2 MB/s, тёплый кэш); `dd iflag=direct` по тому же файлу —
  **181 MB/s, 25.06 s** ⇒ холодный restore ≈25 s.
* Холодный restore через сервер (страницы выбиты `posix_fadvise DONTNEED`, рестарт):
  `3 632 254 028 B` за **20.326 s при 178.7 MB/s**, после восстановления досчитано 29 токенов;
  `dd iflag=direct` — 182 MB/s. Скорость restore совпадает со скоростью устройства.
* Конкурентность (K): 4 одновременных запроса ~2200 токенов, лимит 1 GiB, 2 слота → `writes=4`,
  `evictions=2`, `/health` 200, ни одной строки уровня E/ASSERT в логе, на диске осталось 2 `.bin`.

## Что осталось открытым

- **I/O под общим мьютексом (Stage 6).** Проверено: дисковый кэш дёргается только из главного цикла
  (`server-context.cpp`); grep по остальным `tools/server/*.cpp` пуст, `read_payload`/`n_inflight` вне
  кэша вызывает только selftest. Конкуренции за мьютекс нет, вынос I/O из-под него измеримого
  выигрыша не даёт.

## Stage 8: восстановление больше не держит цикл

Чтение состояния вынесено в отдельный поток: файл читается в буфер, который принимает
`llama_state_seq_set_data_ext()`, а применяет его главный цикл, когда данные готовы. Слот, который
ждёт своё состояние, в цикле пропускается — остальные слоты продолжают обслуживаться. Брошенное
задание (слот освободили до конца чтения) удаляется в цикле.

Измерено (`/tmp/s7async.out`): холодное чтение 849.27 MiB (страницы выбиты `posix_fadvise DONTNEED`)
заняло **5.461 s при 163.1 MB/s**; маленький запрос, отправленный через 1 s после начала restore,
ответил за **0.33 s** (базовый замер без restore — 0.39 s), то есть цикл не ждёт диск.
Бит-в-бит корректность сохранена (`/tmp/s7lclean2.out`): референс без диска 11 095.8 ms → restore
197.4 ms, sha ответа не изменился (`cbf2d04f972bcfa6`).

Оговорки (не измерено): само применение состояния (host → контекст) по-прежнему идёт в главном
цикле, и его отдельная длительность не замерена; состояние на время чтения лежит в RAM — это один
буфер размером с запись на каждое незавершённое восстановление.
