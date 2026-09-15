#!/bin/bash
# functional (not perf) check that the bench harness parses a real llama-server response
BIN=~/llamacpp-diskcache/llama.cpp/build-base/bin/llama-server
M=~/models/Qwen3.8-27B-UD-Q3_K_XL.gguf
LOG=/tmp/dc_harness_check.log
rm -f $LOG
setsid nohup $BIN -m $M -c 8192 -ngl 99 --host 127.0.0.1 --port 8099 > $LOG 2>&1 &
P=$!
UP=0
for i in $(seq 1 120); do
  if grep -qE "listening on|server is listening" $LOG 2>/dev/null; then UP=1; break; fi
  if grep -qE "out of memory|CUDA error" $LOG 2>/dev/null; then break; fi
  sleep 2
done
if [ "$UP" != "1" ]; then echo SERVER_FAIL; tail -15 $LOG; kill $P 2>/dev/null; echo HARNESS_CHECK_DONE; exit 1; fi
echo SERVER_UP
python3 ~/dcbench/bench_disk_cache.py phase1 --url http://127.0.0.1:8099 --tokens 400 --salt SMOKE 2>&1 | tail -14
echo "--- server timing line ---"
grep -E "prompt eval time|slot print_timing" $LOG | tail -2
kill $P 2>/dev/null
sleep 4
pkill -f "llama-server -m $M" 2>/dev/null
echo HARNESS_CHECK_DONE
