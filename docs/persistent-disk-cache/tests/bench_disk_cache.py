#!/usr/bin/env python3
"""Disk-cache benchmark harness for llama-server (project llama-disk-cache).

Usage:
  phase:  bench_disk_cache.py phase1 --url http://127.0.0.1:8080 --tokens 150000 --salt S1 --out /tmp/bench-p1.json
          bench_disk_cache.py phase2 --url http://127.0.0.1:8080 --tokens 150000 --salt S1 --tail 3000 --out /tmp/bench-p2.json
          bench_disk_cache.py report --p1 /tmp/bench-p1.json --p2 /tmp/bench-p2.json

Notes:
  * filler is prose-ish text; token count is NOT sized by characters (see skill ninfer-stand-run 6):
    the script measures prompt_tokens from the response and rescales once on request.
  * always put a unique salt prefix, otherwise `cache` reuse hides the real prefill.
"""
import argparse, json, os, sys, time, urllib.request, hashlib

WORDS = ("prefix cache disk persistent llama server context kv state reuse hdd sequential "
         "bandwidth token prompt resident eviction namespace fingerprint manifest index").split()


def build_prompt(tokens_target, salt, tail_tokens=0):
    # ~1.35 tokens per word for this filler
    n_words = int(tokens_target / 1.35) + 16
    parts = [salt]
    parts.append(" ".join(WORDS[i % len(WORDS)] for i in range(min(n_words, 200))))
    if tail_tokens:
        i0 = 200
        parts.append("TAIL")
        parts.append(" ".join(WORDS[(i0 + i) % len(WORDS)] for i in range(int(tail_tokens / 1.35) + 4)))
    # repeat the middle block; deterministic and cheap to build
    mid = " ".join(WORDS[i % len(WORDS)] for i in range(600))
    reps = max(1, (n_words - 220) // 600)
    parts.insert(2, " ".join([mid] * reps))
    return " ".join(parts)


def post(url, payload, timeout=7200):
    req = urllib.request.Request(url.rstrip("/") + "/completion",
                                 data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = r.read()
    return json.loads(body), time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["phase1", "phase2", "report"])
    ap.add_argument("--url", default="http://127.0.0.1:8080")
    ap.add_argument("--tokens", type=int, default=150000)
    ap.add_argument("--salt", default="DCSALT")
    ap.add_argument("--tail", type=int, default=3000)
    ap.add_argument("--n-predict", type=int, default=8)
    ap.add_argument("--out")
    ap.add_argument("--p1")
    ap.add_argument("--p2")
    a = ap.parse_args()

    if a.mode == "report":
        p1 = json.load(open(a.p1)); p2 = json.load(open(a.p2))
        full = p1["prompt_eval_s"]
        restored = p2["prompt_eval_s"]
        print(json.dumps({
            "phase1_prompt_tokens": p1["prompt_tokens"],
            "phase1_prompt_eval_s": full,
            "phase2_cache_reuse_tokens": p2.get("cache_tokens"),
            "phase2_prompt_tokens": p2["prompt_tokens"],
            "phase2_prompt_eval_s": restored,
            "speedup_x": round(full / restored, 2) if restored > 0 else None,
            "sha_phase1": p1["sha"], "sha_phase2": p2["sha"],
            "formula": "speedup = phase1_prompt_eval_s / phase2_prompt_eval_s "
                       "(phase2 = disk restore + suffix prefill)",
        }, indent=2))
        return

    tail = a.tail if a.mode == "phase2" else 0
    prompt = build_prompt(a.tokens, a.salt, tail)
    payload = {"prompt": prompt, "n_predict": a.n_predict, "temperature": 0, "seed": 42,
               "cache_prompt": True}
    resp, wall = post(a.url, payload)
    tim = resp.get("timings", {})
    out = {
        "prompt_tokens": tim.get("prompt_n", resp.get("tokens_evaluated")),
        "prompt_eval_ms": tim.get("prompt_ms"),
        "prompt_eval_s": (tim.get("prompt_ms") or 0) / 1000.0,
        "predicted_per_second": tim.get("predicted_per_second"),
        "cache_tokens": resp.get("tokens_cached", tim.get("cache_n")),
        "wall_s": wall,
        "sha": hashlib.sha256(resp.get("content", "").encode()).hexdigest()[:16],
    }
    print(json.dumps(out, indent=2))
    if a.out:
        json.dump(out, open(a.out, "w"))


if __name__ == "__main__":
    main()
