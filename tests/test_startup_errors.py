#!/usr/bin/env python3
"""Startup refusals exit with their reason, never abort (docs/STARTUP_ERRORS_D4.md, D4).

Before D4, a configuration that snc_server's helpers or the Engine
constructor refused with an exception escaped main: std::terminate, SIGABRT,
"terminate called after throwing ...". Now a configuration refusal exits 2
with the reason on stderr, and a plain node's data directory is left as
it was (--control and --rejoin write before the engine is built, as before).

  E1 a malformed --cluster (bad entry; members of one group with different
     shard counts): exit 2, "snc_server: --cluster refused: ...".
  E2 an environment override the Engine refuses (SNC_CKPT_IO,
     SNC_CKPT_SYNC_TOKENS, SNC_FOLLOWUP_INTAKE, SNC_XNODE_ACCOUNTING):
     exit 2, "snc_server: refused: <the engine's reason>".
  E3 node-link timers the Engine refuses (--xnode-hb-ms above T_dead in
     cluster mode): exit 2, "refused: node-link timers".
  E4 a --space-uuid that is 32 characters but not hex ("zz..." threw from
     std::stoul; "0x0x...", " 1 1..." and "+1+1..." were read as bytes):
     exit 2, the usage line.
  E5 an existing data directory, started with a refused override: exit 2 and
     every file's bytes, mode and mtime unchanged; the next good start
     recovers it with no remap.
  E6 a shard count above 256 (--shards 300; --shards 65536, which wrapped
     through u16 to 0, "not given"; a --cpus list of 257 CPUs): NativeRuntime
     aborted, or the value was silently replaced; now exit 2.

No case aborts.

Usage: test_startup_errors.py BUILD_DIR
"""
import hashlib
import os
import random
import re
import selectors
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
SERVER = os.path.join(BUILD, "snc_server")
LINE = re.compile(r"^snc_server shards=(\d+) ports", re.M)
UUID = "d4" * 16


def free_ports(n, span=4):
    """n bases with `span` consecutive free ports each, below the ephemeral range."""
    bases, taken = [], set()
    rnd = random.Random(os.getpid() * 7919 + time.time_ns())
    while len(bases) < n:
        b = rnd.randrange(20000, 32000 - span)
        if any(b + k in taken for k in range(span)):
            continue
        socks = []
        try:
            for k in range(span):
                s = socket.socket()
                s.bind(("127.0.0.1", b + k))
                socks.append(s)
        except OSError:
            continue
        finally:
            for s in socks:
                s.close()
        bases.append(b)
        taken.update(b + k for k in range(span))
    return bases


def run(args, env=None, want_line=True, timeout=30):
    """Start snc_server. With want_line, wait for its startup line, stop it
    with SIGINT and return (shards, output); otherwise wait for it to exit and
    return (exit code, output)."""
    full_env = dict(os.environ)
    full_env.update(env or {})
    p = subprocess.Popen([SERVER, *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=full_env)
    out, got = [], None
    t_end = time.time() + timeout
    # Read the pipe's bytes directly (see test_cluster_shards.py).
    fd = p.stdout.fileno()
    sel = selectors.DefaultSelector()
    sel.register(fd, selectors.EVENT_READ)
    pending = b""
    try:
        while got is None and time.time() < t_end:
            if not sel.select(timeout=max(0.0, t_end - time.time())):
                break
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            pending += chunk
            while b"\n" in pending and got is None:
                line, pending = pending.split(b"\n", 1)
                line = line.decode(errors="replace") + "\n"
                out.append(line)
                m = LINE.match(line)
                if m and want_line:
                    got = int(m.group(1))
        out.append(pending.decode(errors="replace"))
    finally:
        sel.close()
        if p.poll() is None:
            p.send_signal(signal.SIGINT)
        try:
            rest, _ = p.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            p.kill()
            rest, _ = p.communicate()
        out.append((rest or b"").decode(errors="replace"))
    text = "".join(out)
    assert "terminate called" not in text and p.returncode != -signal.SIGABRT, f"{args} {env}: aborted\n{text}"
    if want_line:
        assert got is not None, f"{args} {env}: no startup line (rc {p.returncode})\n{text}"
        return got, text
    return p.returncode, text


def refused(args, *needles, env=None, code=2):
    rc, text = run(args, env=env, want_line=False)
    assert rc == code, f"{args} {env}: rc {rc}, want {code}\n{text}"
    assert not LINE.search(text), f"{args} {env}: started\n{text}"
    for n in needles:
        assert n in text, f"{args} {env}: no '{n}' in\n{text}"
    return text


def listing(d):
    """Every file under d: path, size, mode, mtime and SHA-256."""
    out = []
    for root, dirs, files in os.walk(d):
        dirs.sort()
        for f in sorted(files):
            path = os.path.join(root, f)
            st = os.stat(path)
            with open(path, "rb") as fh:
                digest = hashlib.sha256(fh.read()).hexdigest()
            out.append((os.path.relpath(path, d), st.st_size, st.st_mode, st.st_mtime_ns, digest))
    return out


def main():
    work = tempfile.mkdtemp(prefix="snc-starterr-")
    try:
        port, link, link2 = free_ports(3)
        base = ["--shards", "2", "--objects", "1000", "--port", str(port)]

        # E1
        refused(base + ["--cluster", "garbage"], "snc_server: --cluster refused: ", "got 'garbage'")
        refused(base + ["--cluster", f"127.0.0.1:{link}:2|127.0.0.1:{link2}:3"], "snc_server: --cluster refused: ",
                "different shard counts")
        print("E1 a malformed --cluster: exit 2 with the parser's reason")

        # E2
        for var, val, why in [("SNC_CKPT_IO", "bad", "SNC_CKPT_IO must be legacy or dsync"),
                              ("SNC_CKPT_SYNC_TOKENS", "999", "SNC_CKPT_SYNC_TOKENS must be 0..256"),
                              ("SNC_FOLLOWUP_INTAKE", "maybe", "SNC_FOLLOWUP_INTAKE must be on or off"),
                              ("SNC_XNODE_ACCOUNTING", "x", "SNC_XNODE_ACCOUNTING must be on or off")]:
            refused(base, "snc_server: refused: " + why, env={var: val})
        print("E2 four refused environment overrides: exit 2, 'snc_server: refused: <reason>'")

        # E3
        refused(base + ["--cluster", f"127.0.0.1:{link}:2", "--group", "0", "--member", "0",
                        "--links-generation-file", f"{work}/gen", "--xnode-hb-ms", "100000"],
                "snc_server: refused: node-link timers")
        print("E3 node-link timers the engine refuses: exit 2 with the timers named")

        # E4
        for bad in ["z" * 32, "0x" * 16, " 1" * 16, "+1" * 16]:
            refused(base + ["--space-uuid", bad], "--space-uuid takes 32 hex digits")
        print("E4 a 32-character --space-uuid that is not hex: exit 2, the usage line")

        # E5
        d = f"{work}/data"
        dir_args = base + ["--space-uuid", UUID, "--data-dir", d]
        n, _ = run(dir_args)
        assert n == 2
        before = listing(d)
        assert before, "E5: the first start wrote nothing"
        refused(dir_args, "snc_server: refused: SNC_CKPT_IO", env={"SNC_CKPT_IO": "bad"})
        assert listing(d) == before, f"E5: the refused start changed the directory\n{before}\n{listing(d)}"
        n, text = run(dir_args)
        assert n == 2 and "shard count changed" not in text, f"E5: restart\n{text}"
        print("E5 a refused start on an existing directory: exit 2, every file unchanged; the next start recovers")
        # E6
        p6 = ["--objects", "1000", "--port", str(port)]
        refused(p6 + ["--shards", "300"], "--shards takes 1 to 256")
        refused(p6 + ["--shards", "65536"], "--shards takes 1 to 256")
        refused(p6 + ["--cpus", ",".join(["0"] * 257)], "257 CPUs would give 257 shards; at most 256")
        print("E6 more than 256 shards, by --shards or --cpus: exit 2")
        print("test_startup_errors: ok")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
