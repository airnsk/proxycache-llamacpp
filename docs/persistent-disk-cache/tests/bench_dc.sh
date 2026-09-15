#!/bin/bash
# Disk-cache benchmark driver (project llama-disk-cache), runs ON the stand.
# usage: bash bench_dc.sh <server-bin> <model.gguf> <cache-dir> [tokens] [tail]
# phases: start -> phase1 (full prefill) -> SIGTERM -> start -> phase2 (disk restore + suffix) -> report
set -u
BIN="${1:?server binary}"
MODEL="${2:?model gguf}"
CACHE="${3:?cache dir}"
TOKENS="${4:-150000}"
TAIL="${5:-3000}"
PORT=8088
SALT="DCSALT-$(date +%s)"
LOGP=/tmp/bench_dc_server.log
ART=/tmp/bench_dc_artifacts
mkdir -p "$ART"

start_server() {
  rm -f "$LOGP"
  local out="$1"
  setsid nohup "$BIN" -m "$MODEL" -c 160000 -ngl 99 -ctk q8_0 -ctv q8_0 -fa on \
    --host 127.0.0.1 --port $PORT --no-webui \
    --cache-ram 32768 \
    --cache-disk "$CACHE" \
    > "$LOGP" 2>&1 &
  echo $! > /tmp/bench_dc.pid
  for i in $(seq 1 300); do
    if grep -qE "listening on|server is listening" "$LOGP" 2>/dev/null; then echo "SERVER_UP"; return 0; fi
    if grep -qE "out of memory|CUDA error|error:" "$LOGP" 2>/dev/null; then echo "SERVER_FAIL"; tail -20 "$LOGP"; return 1; fi
    sleep 2
  done
  echo "SERVER_TIMEOUT"; tail -20 "$LOGP"; return 1
}

stop_server() {
  [ -f /tmp/bench_dc.pid ] && kill $(cat /tmp/bench_dc.pid) 2>/dev/null
  for i in $(seq 1 60); do
    pgrep -f "[b]ench" >/dev/null; done
  sleep 5
  pkill -f "llama-server -m $MODEL" 2>/dev/null
  sleep 3
}

echo "=== START 1 ==="
start_server || { echo BENCH_FAIL_START1; exit 1; }
python3 /home/user/dcbench/bench_disk_cache.py phase1 --url http://127.0.0.1:$PORT \
  --tokens $TOKENS --salt "$SALT" --out "$ART/p1.json" | tee "$ART/p1.out"
echo "--- startup summary / disk cache lines ---"
grep -iE "disk cache|prompt cache|namespace" "$LOGP" | head -20

echo "=== STOP ==="
stop_server

echo "=== START 2 ==="
start_server || { echo BENCH_FAIL_START2; exit 1; }
grep -iE "disk cache|loaded index|namespace" "$LOGP" | head -20

python3 /home/user/dcbench/bench_disk_cache.py phase2 --url http://127.0.0.1:$PORT \
  --tokens $TOKENS --salt "$SALT" --tail $TAIL --out "$ART/p2.json" | tee "$ART/p2.out"

echo "=== REPORT ==="
python3 /home/user/dcbench/bench_disk_cache.py report --p1 "$ART/p1.json" --p2 "$ART/p2.json" | tee "$ART/report.json"
echo "--- server slot timings (phase2) ---"
grep -E "prompt eval time|slot print_timing" "$LOGP" | tail -5
echo "BENCH_DC_DONE"
