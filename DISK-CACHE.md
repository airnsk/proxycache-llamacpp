# Persistent prefix/KV disk cache for `llama-server`

This fork adds a persistent, multi-level prompt/KV cache to `llama-server`:
the existing in-RAM prompt cache stays the first level, and a new on-disk
level keeps serialized sequence states across server restarts, so a long
prompt that was prefilled once is restored from disk instead of being
recomputed.

Nothing external is required — no proxy, no FastAPI sidecar; the cache lives
inside `llama-server` and uses the streaming sequence-state API.

## Status

| stage | content | state |
| --- | --- | --- |
| 1 | code study of the current master | done |
| 2 | namespace, manifest, index, atomic writes | done |
| 3 | streamed state save/load | done |
| 4+5 | slot selection (resident vs disk), save hooks, cost model, metrics | done |
| 6 | eviction/lifecycle, crash recovery, dedup | in progress |
| 7 | tests A–L, benchmark | in progress |

## What is in the commits

- `server: persistent disk cache — namespace, manifest, index, atomic writes`
- `server: stream the sequence state to/from disk instead of buffering it in RAM`
- `server: hook the disk cache into the slot lifecycle (selection, save, cost model)`

## Usage

```
llama-server -m model.gguf -ngl 99 -fa on -c 160000 \
  --cache-ram 32768 \
  --cache-disk /mnt/cache \
  --cache-disk-size 500 \
  --cache-disk-read-mbps 200 \
  --cache-disk-min-gain-ms 1000 \
  --cache-idle-slots --metrics
```

`--cache-disk <path>` enables the disk level and points at the cache root
(the size limit applies to the whole root). Without it, behaviour is exactly
as upstream.

## Documentation

- `docs/persistent-disk-cache/ARCHITECTURE.md` — code map of master and the
  insertion points used by the cache
- `docs/persistent-disk-cache/PLAN-STAGE4-6.md` — plan for stages 4–6
- `docs/persistent-disk-cache/VERIFY-STAGE23.md` — stage 2/3 verification
- `docs/persistent-disk-cache/STAGE4-REPORT.md` — stage 4+5 report
- `docs/persistent-disk-cache/RESULTS.md` — measured results, with an
  explicit list of what is not measured yet
- `docs/persistent-disk-cache/tests/` — harness scripts used for the
  measurements above

Measured numbers in the documents come from a single RTX 3090 host (24 GB,
24 cores) with the cache on a SATA HDD; they are measurements, not estimates.
