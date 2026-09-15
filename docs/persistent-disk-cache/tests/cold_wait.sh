#!/bin/bash
# Wait until the RTX 3090 is free (no other llama-server), then run the cold-restore test
# on a PRIVATE cache dir so nothing else can touch those state files.
LOG=/tmp/cold_wait.log
OUT=/tmp/cold_restore.out
: > "$LOG"
BIN=/home/user/llamacpp-diskcache-stage4/llama.cpp/build-s4/bin/llama-server
MODEL=/home/user/models/Qwen3.8-27B-UD-Q3_K_XL.gguf
CACHE=/mnt/3tb/llama-cache-cold

mkdir -p "$CACHE"
for i in $(seq 1 240); do
  USED=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits)
  SRV=$(pgrep -fc "[l]lama-server")
  echo "$(date -u +%H:%M:%S) used=${USED}MiB servers=${SRV}" >> "$LOG"
  if [ "${USED:-99999}" -lt 1500 ] && [ "${SRV:-1}" -eq 0 ]; then
    sleep 60
    USED2=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits)
    if [ "${USED2:-99999}" -lt 1500 ]; then
      echo "GPU_FREE_START $(date -u +%H:%M:%S)" >> "$LOG"
      rm -f /tmp/cold_restore_srv1.log /tmp/cold_restore_srv2.log
      python3 /home/user/dcbench/cold_restore.py --bin "$BIN" --model "$MODEL" \
        --cache-dir "$CACHE" --tokens 40000 --tail 15000 > "$OUT" 2>&1
      echo "COLD_TEST_DONE rc=$? $(date -u +%H:%M:%S)" >> "$LOG"
      exit 0
    fi
  fi
  sleep 30
done
echo "COLD_WAIT_TIMEOUT $(date -u +%H:%M:%S)" >> "$LOG"
