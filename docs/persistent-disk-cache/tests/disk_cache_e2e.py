#!/usr/bin/env python3
"""Stage 7 — end-to-end disk-cache scenarios (HTTP level, no module internals).

Runs on the stand. Requires a built llama-server with --cache-disk support
and a small/medium GGUF model. Defaults target the project stand layout.

  python3 disk_cache_e2e.py --bin <llama-server> --model <gguf> --cache-dir /mnt/3tb/llama-cache \
      --cases A,C,H,I,J

Each case prints "CASE <letter> OK|FAIL <detail>". Exit code 0 only if all requested cases pass.
No prompt text is printed (only counts/hashes), per the project's logging rule.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, time, urllib.request

PORT = 8097


def build_prompt(n_tokens, salt, tail=""):
    words = ("prefix cache disk persistent llama server kv state reuse hdd sequential bandwidth "
             "token prompt resident eviction namespace fingerprint manifest").split()
    n = int(n_tokens / 1.35) + 8
    body = " ".join(words[i % len(words)] for i in range(n))
    return (salt + " " + body + ((" TAIL " + tail) if tail else ""))


def http(url, path, payload=None, timeout=7200):
    if payload is None:
        with urllib.request.urlopen(url.rstrip("/") + path, timeout=timeout) as r:
            return json.loads(r.read())
    req = urllib.request.Request(url.rstrip("/") + path, data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


class Server:
    def __init__(self, a, extra=(), log="/tmp/dc_e2e_server.log"):
        self.a = a
        self.extra = list(extra)
        self.log = log
        self.proc = None

    def start(self, wait=600):
        open(self.log, "w").close()
        cmd = [self.a.bin, "-m", self.a.model, "-c", "16384", "-ngl", "99",
               "--host", "127.0.0.1", "--port", str(PORT), "--no-webui",
               "--cache-ram", "2048", "--cache-disk", self.a.cache_dir] + self.extra
        self.proc = subprocess.Popen(cmd, stdout=open(self.log, "ab"), stderr=subprocess.STDOUT,
                                     start_new_session=True)
        t0 = time.time()
        while time.time() - t0 < wait:
            if self.proc.poll() is not None:
                raise RuntimeError("server exited early (see %s)" % self.log)
            try:
                if "listening on" in open(self.log, errors="replace").read():
                    return
            except FileNotFoundError:
                pass
            time.sleep(2)
        raise RuntimeError("server did not start within %ds" % wait)

    def stop(self):
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None
        time.sleep(3)

    def complete(self, prompt, n_predict=4, extra=None):
        p = {"prompt": prompt, "n_predict": n_predict, "temperature": 0, "seed": 42,
             "cache_prompt": True}
        p.update(extra or {})
        r = http("http://127.0.0.1:%d" % PORT, "/completion", p)
        t = r.get("timings", {})
        return {
            "prompt_tokens": t.get("prompt_n"),
            "prompt_eval_s": (t.get("prompt_ms") or 0) / 1000.0,
            "cached": r.get("tokens_cached"),
            "sha": hashlib.sha256(r.get("content", "").encode()).hexdigest()[:16],
        }


def case_A(a, srv):
    """restart reuse: prefill -> restart -> same prefix + tail is served mostly from disk cache."""
    salt = "SALT-A-" + str(int(time.time()))
    p1 = build_prompt(20000, salt)
    srv.start()
    r1 = srv.complete(p1)
    srv.stop()
    srv.start()  # fresh process, empty RAM/VRAM cache
    r2 = srv.complete(build_prompt(20000, salt, "extra-tail-tokens"))
    srv.stop()
    ok = (r2["prompt_eval_s"] < r1["prompt_eval_s"] * 0.5) and (r2["cached"] or 0) > 0
    return ok, {"p1": r1, "p2": r2,
                "note": "expect p2.cached>0 and p2.prompt_eval_s << p1.prompt_eval_s"}


def case_C(a, srv):
    """divergence: cached ABCD X must not be used for ABCD Y (no reuse of a state past divergence)."""
    salt = "SALT-C-" + str(int(time.time()))
    srv.start()
    srv.complete(build_prompt(8000, salt) + " ALPHA-BRANCH")
    srv.stop()
    srv.start()
    r = srv.complete(build_prompt(8000, salt) + " BETA-BRANCH")
    srv.stop()
    ok = (r["cached"] or 0) == 0
    return ok, {"p2": r, "note": "expect cached == 0 (branch differs after shared prefix ...)"}


def case_H(a, srv):
    """corrupt state must not kill the server."""
    states = os.path.join(a.cache_dir, "**", "states")
    srv.start()
    srv.complete(build_prompt(8000, "SALT-H"))
    srv.stop()
    victim = None
    for root, _dirs, files in os.walk(a.cache_dir):
        for f in files:
            if f.endswith(".bin"):
                victim = os.path.join(root, f)
    if victim:
        with open(victim, "r+b") as fh:  # stomp the middle of the payload
            fh.seek(max(0, os.path.getsize(victim) // 2))
            fh.write(b"\x00" * 64)
    srv.start()
    r = srv.complete(build_prompt(8000, "SALT-H"))
    srv.stop()
    ok = r["prompt_tokens"] is not None
    return ok, {"victim": victim, "p": r, "note": "server must answer; entry invalidated/ignored"}


def case_I(a, srv):
    """leftover .tmp must not be treated as a valid state."""
    for root, _dirs, _files in os.walk(a.cache_dir):
        if os.path.basename(root) == "states":
            open(os.path.join(root, "deadbeef.tmp"), "wb").write(b"garbage" * 1000)
    srv.start()
    r = srv.complete(build_prompt(4000, "SALT-I"))
    srv.stop()
    leftover = [os.path.join(r_, f) for r_, _d, fs in os.walk(a.cache_dir) for f in fs
                if f.endswith(".tmp")]
    return (r["prompt_tokens"] is not None) and len(leftover) == 0, {"leftover_tmp": leftover, "p": r}


def case_J(a, srv):
    """disk limit forces eviction."""
    srv.start()
    for i in range(3):
        srv.complete(build_prompt(20000, "SALT-J-%d" % i, "x" * (i + 1)))
    srv.stop()
    total = sum(os.path.getsize(os.path.join(r, f)) for r, _d, fs in os.walk(a.cache_dir) for f in fs)
    limit = a.disk_limit
    srv.start()  # restart: index must load and be consistent
    r = srv.complete(build_prompt(4000, "SALT-J-probe"))
    srv.stop()
    return (r["prompt_tokens"] is not None and total <= limit * 1.2), {"bytes_total": total,
                                                                       "limit": limit, "p": r}


CASES = {"A": case_A, "C": case_C, "H": case_H, "I": case_I, "J": case_J}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--disk-limit-bytes", type=int, default=50 * 1024 * 1024)
    ap.add_argument("--cases", default="A,C,H,I,J")
    a = ap.parse_args()
    os.makedirs(a.cache_dir, exist_ok=True)
    a.disk_limit = a.disk_limit_bytes
    rc = 0
    for letter in a.cases.split(","):
        letter = letter.strip().upper()
        if letter not in CASES:
            print("CASE %s SKIP (unknown)" % letter)
            continue
        srv = Server(a, extra=["--cache-disk-size", str(a.disk_limit_bytes)])
        try:
            ok, detail = CASES[letter](a, srv)
            print("CASE %s %s %s" % (letter, "OK" if ok else "FAIL", json.dumps(detail)[:400]))
            if not ok:
                rc = 1
        except Exception as e:
            print("CASE %s FAIL exception: %s" % (letter, e))
            rc = 1
        finally:
            srv.stop()
    print("E2E_DONE rc=%d" % rc)
    sys.exit(rc)


if __name__ == "__main__":
    main()
