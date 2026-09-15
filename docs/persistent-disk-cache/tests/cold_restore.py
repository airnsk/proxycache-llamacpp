#!/usr/bin/env python3
"""Cold-restore test for the disk cache (project llama-disk-cache), runs ON the stand.

Cycle: full prefill (state saved) -> stop -> evict the state file from the page cache
(posix_fadvise DONTNEED, no root needed) -> restart -> same prefix + tail -> measure the
restore as the server reports it. Also reports the O_DIRECT read rate of the same file as
a device-level reference.

usage: cold_restore.py --bin <llama-server> --model <gguf> --cache-dir <dir> [--tokens 40000] [--tail 15000]
"""
import argparse, glob, json, os, re, subprocess, sys, time, urllib.request

PORT = 8096
WORDS = ("prefix cache disk persistent llama server kv state reuse hdd sequential bandwidth token "
         "prompt resident eviction namespace fingerprint manifest index checkpoint").split()


def build_prompt(n_tokens, salt, tail=""):
    n = int(n_tokens / 1.35) + 8
    body = " ".join(WORDS[i % len(WORDS)] for i in range(n))
    return salt + " " + body + ((" TAIL " + tail) if tail else "")


def post(path, payload, timeout=3600):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (PORT, path),
                                 data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def start_server(a, log):
    cmd = [a.bin, "-m", a.model, "-c", "160000", "-ngl", "99", "-ctk", "q8_0", "-ctv", "q8_0",
           "-fa", "on", "--host", "127.0.0.1", "--port", str(PORT), "--no-webui",
           "--cache-ram", "32768", "--cache-disk", a.cache_dir, "--cache-idle-slots"]
    p = subprocess.Popen(cmd, stdout=open(log, "ab"), stderr=subprocess.STDOUT, start_new_session=True)
    t0 = time.time()
    while time.time() - t0 < 900:
        if p.poll() is not None:
            raise RuntimeError("server exited early, see %s" % log)
        if "listening on" in open(log, errors="replace").read():
            return p
        time.sleep(2)
    raise RuntimeError("server did not start")


def drop_pages(path):
    """evict a file from the page cache without root"""
    subprocess.run(["sync"], check=False)
    fd = os.open(path, os.O_RDONLY)
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    finally:
        os.close(fd)


def direct_read_bps(path):
    r = subprocess.run(["dd", "if=" + path, "of=/dev/null", "bs=8M", "iflag=direct"],
                       capture_output=True, text=True)
    m = re.search(r"([0-9.]+) ([kMG])B/s", r.stderr)
    if not m:
        return None, r.stderr.strip()
    v = float(m.group(1)) * {"k": 1e3, "M": 1e6, "G": 1e9}[m.group(2)]
    return v, r.stderr.strip().splitlines()[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--tokens", type=int, default=40000)
    ap.add_argument("--tail", type=int, default=15000)
    a = ap.parse_args()

    out = {"mode": "cold_restore", "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}

    # phase 1: full prefill, state saved
    log1 = "/tmp/cold_restore_srv1.log"
    open(log1, "w").close()
    p = start_server(a, log1)
    salt = "COLDSALT-%d" % int(time.time())
    r1 = post("/completion", {"prompt": build_prompt(a.tokens, salt), "n_predict": 1,
                              "temperature": 0, "seed": 42, "cache_prompt": True})
    t1 = r1.get("timings", {})
    out["phase1"] = {"prompt_n": t1.get("prompt_n"), "prompt_s": (t1.get("prompt_ms") or 0) / 1000.0,
                     "sha": __import__("hashlib").sha256(r1.get("content", "").encode()).hexdigest()[:16]}

    # NOTE: a state is persisted when the slot is updated on the NEXT task (or when an idle slot is
    # saved), not right after a single request - so send a second, unrelated task to trigger the save
    post("/completion", {"prompt": "trigger-save " + salt, "n_predict": 1,
                         "temperature": 0, "seed": 7, "cache_prompt": True})
    time.sleep(3)
    out["phase1_saved_lines"] = [l.strip() for l in open(log1, errors="replace").read().splitlines()
                                if ("stored state" in l or "saved state" in l)]
    if not out["phase1_saved_lines"]:
        out["error"] = "no state was persisted in phase 1 (no 'stored state' line)"
        print(json.dumps(out, indent=2))
        p.terminate(); p.wait(timeout=60)
        return
    p.terminate(); p.wait(timeout=60); time.sleep(3)

    files = sorted(glob.glob(os.path.join(a.cache_dir, "*", "states", "*.bin")),
                   key=os.path.getmtime)
    if not files:
        out["error"] = "no state file was written"
        print(json.dumps(out, indent=2)); return
    state = files[-1]
    out["state_file"] = state
    out["state_bytes"] = os.path.getsize(state)

    # evict from the page cache, then measure the device-level rate of the same file
    drop_pages(state)
    bps, ddline = direct_read_bps(state)
    out["direct_read_bps"] = bps
    out["direct_read_line"] = ddline
    drop_pages(state)  # the dd above pulled it back into the cache for the buffered case

    # phase 2: restart, same prefix + tail -> disk restore
    log2 = "/tmp/cold_restore_srv2.log"
    open(log2, "w").close()
    p = start_server(a, log2)
    r2 = post("/completion", {"prompt": build_prompt(a.tokens, salt, "x" * 200), "n_predict": 4,
                              "temperature": 0, "seed": 42, "cache_prompt": True})
    t2 = r2.get("timings", {})
    out["phase2"] = {"prompt_n": t2.get("prompt_n"), "prompt_s": (t2.get("prompt_ms") or 0) / 1000.0,
                     "tokens_cached": r2.get("tokens_cached")}
    time.sleep(1)
    p.terminate(); p.wait(timeout=60)

    l2 = open(log2, errors="replace").read()
    out["phase2_restore_lines"] = [l.strip() for l in l2.splitlines() if "restored" in l or "preferred" in l]
    out["phase2_index_line"] = [l.strip() for l in l2.splitlines() if "loaded index" in l]
    if out["phase2_restore_lines"]:
        pass  # keep the raw lines; the caller quotes them verbatim
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()
