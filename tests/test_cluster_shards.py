#!/usr/bin/env python3
"""A cluster member's shard count is its group's (docs/SHARD_COUNT_D3.md, D3).

In --cluster and --control mode the group's shard count is recorded in the
--cluster list or the placement map, and the engine requires the node to run
exactly that many shards. Before D3, snc_server ignored it for its default
(one shard per CPU it may run on), so a member started without --shards
aborted (std::terminate) whenever that differed, and a --shards or --cpus
that disagreed aborted the same way. A data directory with another count was
remapped silently to the group's.

  K1 --cluster, no --shards: takes the group's count, under one CPU and
     unrestricted.
  K2 --cluster with --shards that disagrees: refused, exit 2, both counts named.
  K3 --cluster with --cpus that disagrees: refused the same way.
  K4 --cluster with a --group or --member the list does not have: refused,
     exit 2.
  K5 --control, no --shards: takes the placement map's count, under one CPU;
     restarted from its data directory under three CPUs it keeps that count
     and does not remap.
  K6 --control with --shards that disagrees: refused, exit 2.
  K7 a data directory made with 3 shards, started as a member of a 2-shard
     group, as a primary and as a standby: refused (exit 1, "refusing to
     remap"), with every file's bytes, mode and mtime unchanged, and a
     standalone restart at 3 shards does not remap.

No case aborts. Needs 3 CPUs for the restart under other CPUs (K5) to differ
from the first start, else that part is skipped, saying so.

Usage: test_cluster_shards.py BUILD_DIR
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
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from witness_state import first_start  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
SERVER = os.path.join(BUILD, "snc_server")
LINE = re.compile(r"^snc_server shards=(\d+) ports", re.M)
UUID = "d3" * 16


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


def run(args, cpus=None, want_line=True, timeout=30):
    """Start snc_server on `cpus` (None: inherited). With want_line, wait for
    its startup line, stop it with SIGINT and return (shards, output);
    otherwise wait for it to exit and return (exit code, output)."""
    def pin():
        if cpus is not None:
            os.sched_setaffinity(0, cpus)

    p = subprocess.Popen([SERVER, *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         preexec_fn=pin)
    out, got = [], None
    t_end = time.time() + timeout
    # Read the pipe's bytes directly: a buffered readline() would hold lines
    # that select() no longer reports, and lose them to communicate().
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
        out.append(rest or "")
    text = "".join(out)
    assert "terminate called" not in text and p.returncode != -signal.SIGABRT, f"{args}: aborted\n{text}"
    if want_line:
        assert got is not None, f"{args}: no startup line (rc {p.returncode})\n{text}"
        return got, text
    return p.returncode, text


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


def refused(args, code, *needles, cpus=None):
    rc, text = run(args, cpus=cpus, want_line=False)
    assert rc == code, f"{args}: rc {rc}, want {code}\n{text}"
    assert not LINE.search(text), f"{args}: started\n{text}"
    for n in needles:
        assert n in text, f"{args}: no '{n}' in\n{text}"
    return text


def main():
    allowed = sorted(os.sched_getaffinity(0))
    one = {allowed[-1]}
    work = tempfile.mkdtemp(prefix="snc-cshards-")
    procs = []
    try:
        # One call, so no two bases overlap (witness ports included).
        port, link, link2, sport, admin, nlink, nclient, admin2, nlink2, nclient2, *wports = free_ports(13)
        def cluster_args(group="0", member="0"):
            return ["--objects", "1000", "--port", str(port), "--cluster", f"127.0.0.1:{link}:2", "--group", group,
                    "--member", member, "--links-generation-file", f"{work}/gen"]

        cl = cluster_args()

        # K1
        n, _ = run(cl, cpus=one)
        assert n == 2, f"K1: one CPU gave shards={n}, want the group's 2"
        n, _ = run(cl)
        assert n == 2, f"K1: unrestricted ({len(allowed)} CPUs) gave shards={n}, want 2"
        print(f"K1 --cluster without --shards: 2 shards under 1 CPU and under {len(allowed)}")

        # K2, K3, K4
        refused(cl + ["--shards", "3"], 2, "group 0 has 2 shards in --cluster", "--shards gives 3")
        refused(cl + ["--cpus", str(allowed[0])], 2, "group 0 has 2 shards in --cluster", "--cpus gives 1")
        refused(cluster_args("5"), 2, "--group 5")
        refused(cluster_args(member="9"), 2, "--member 9")
        print("K2-K4 a disagreeing --shards, --cpus, --group or --member: refused, exit 2, counts named")

        # K5, K6: the placement map from three witnesses (snc_ctl init): one
        # group of 2 shards, members 1 and 2; only node 1 is started.
        control = ",".join(f"127.0.0.1:{w}" for w in wports)
        for i, w in enumerate(wports):
            args = [f"{BUILD}/snc_witness", "--port", str(w), "--state", f"{work}/w{i}", "--id", str(i), "--peers",
                    control, "--group", f"0=127.0.0.1:{admin},127.0.0.1:{admin2}"]
            procs.append(subprocess.Popen(first_start(args), stdout=open(f"{work}/w{i}.log", "w"),
                                          stderr=subprocess.STDOUT))
        time.sleep(0.3)
        r = subprocess.run([f"{BUILD}/snc_ctl", "--control", control, "init", "--nodes",
                            f"1@127.0.0.1:{admin},2@127.0.0.1:{admin2}", "--groups",
                            f"2:1/{nlink}/{nclient}+2/{nlink2}/{nclient2}"],
                           capture_output=True, text=True, timeout=30)
        assert r.returncode == 0, r.stdout + r.stderr
        ct = ["--objects", "1000", "--data-dir", f"{work}/n1", "--control", control, "--node-id", "1",
              "--admin-port", str(admin)]
        refused(ct + ["--shards", "3"], 2, "group 0 has 2 shards in the placement map", "--shards gives 3")
        print("K6 --control with a disagreeing --shards: refused, exit 2")
        n, text = run(ct, cpus=one)
        assert n == 2, f"K5: one CPU gave shards={n}, want the map's 2\n{text}"
        # The restart runs under 3 CPUs: under 2, D2-a's default would give the
        # group's count by chance and the old code would pass too.
        if len(allowed) >= 3:
            three = set(allowed[:3])
            n, text = run(ct, cpus=three)
            assert n == 2 and "shard count changed" not in text, f"K5: restart under {sorted(three)}\n{text}"
            print("K5 --control without --shards: the map's 2 shards under 1 CPU, kept on restart under 3")
        else:
            print("K5 --control without --shards: the map's 2 shards under 1 CPU; restart part skipped, "
                  "needs 3 CPUs")

        # K7
        d = f"{work}/k7"
        n, _ = run(["--shards", "3", "--objects", "1000", "--port", str(port), "--space-uuid", UUID,
                    "--data-dir", d])
        assert n == 3
        before = listing(d)
        member = ["--objects", "1000", "--port", str(port), "--space-uuid", UUID, "--data-dir", d, "--cluster",
                  f"127.0.0.1:{link2}:2", "--group", "0", "--member", "0", "--links-generation-min", "1"]
        refused(member, 1, "holds 3 shards, but its group has 2", "refusing to remap")
        assert listing(d) == before, f"K7: the refused start changed the directory\n{before}\n{listing(d)}"
        # As a standby too: refused before its role file is written.
        refused(member + ["--standby", str(sport)], 1, "holds 3 shards, but its group has 2", "refusing to remap")
        assert listing(d) == before, f"K7: the refused standby start changed the directory\n{listing(d)}"
        n, text = run(["--shards", "3", "--objects", "1000", "--port", str(port), "--space-uuid", UUID,
                       "--data-dir", d])
        assert n == 3 and "shard count changed" not in text, f"K7: directory was changed\n{text}"
        print("K7 a 3-shard directory as a 2-shard group's member (primary or standby): refused, every file's "
              "bytes, mode and mtime unchanged")
        print("test_cluster_shards: ok")
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
