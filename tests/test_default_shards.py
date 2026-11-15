#!/usr/bin/env python3
"""snc_server's default shard count (docs/CPU_PLACEMENT_R1.md D2, slice D2-a).

With neither --shards nor --cpus, snc_server runs one shard per CPU the
process may run on (its affinity set: taskset, a cpuset, a container), not
one per CPU the host has online. Deterministic: each case starts the server
in memory under a given CPU set, reads its startup line and stops it.

  S1 under one CPU: shards=1.
  S2 under two CPUs that are not neighbours (needs 3 CPUs): shards=2.
  S3 unrestricted: shards = the size of the test's own set.
  S4 an explicit --shards or --cpus still wins under one CPU.

S1 and S2 fail on the code before D2-a whenever the host has more CPUs than
the set given. Needs 2 CPUs (S1, S4) and 3 (S2); a case that cannot
discriminate on this host is skipped, saying so.

Usage: test_default_shards.py BUILD_DIR
"""
import os
import re
import signal
import socket
import subprocess
import sys
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
SERVER = os.path.join(BUILD, "snc_server")
LINE = re.compile(r"^snc_server shards=(\d+) ports", re.M)


def free_ports(n):
    """A run of n free ports below the ephemeral range."""
    for base in range(20000 + (os.getpid() % 500) * 20, 32000, 37):
        socks = []
        try:
            for i in range(n):
                s = socket.socket()
                s.bind(("127.0.0.1", base + i))
                socks.append(s)
            return base
        except OSError:
            continue
        finally:
            for s in socks:
                s.close()
    raise RuntimeError("no free port run")


def shards_under(cpus, *flags):
    """Start the server on `cpus` (None: the test's own set); its shard count."""
    allowed = sorted(os.sched_getaffinity(0))
    port = free_ports(len(allowed) + 1)

    def pin():
        if cpus is not None:
            os.sched_setaffinity(0, cpus)

    p = subprocess.Popen([SERVER, "--port", str(port), *flags], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, preexec_fn=pin)
    out, got = [], None
    t_end = time.time() + 30
    try:
        while time.time() < t_end:
            line = p.stdout.readline()
            if not line:
                break
            out.append(line)
            m = LINE.match(line)
            if m:
                got = int(m.group(1))
                break
    finally:
        p.send_signal(signal.SIGINT)
        try:
            p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    assert got is not None, f"cpus={cpus} {flags}: no startup line\n{''.join(out)}"
    return got


def main():
    allowed = sorted(os.sched_getaffinity(0))
    online = os.cpu_count()
    print(f"test's CPU set: {len(allowed)} CPUs, {online} online")
    ran = 0
    if len(allowed) >= 2:
        n = shards_under({allowed[-1]})
        assert n == 1, f"S1: one CPU ({allowed[-1]}) gave shards={n}, want 1"
        print(f"S1 one CPU: shards={n}")
        n = shards_under({allowed[-1]}, "--shards", "3")
        assert n == 3, f"S4: --shards 3 under one CPU gave shards={n}"
        n = shards_under({allowed[-1]}, "--cpus", f"{allowed[0]},{allowed[1]}")
        # --cpus outside the set the server was started on is the operator's
        # choice; its count still sets the shard count.
        assert n == 2, f"S4: --cpus of 2 under one CPU gave shards={n}"
        print("S4 explicit --shards and --cpus win")
        ran += 2
    else:
        print("S1, S4: skipped, need 2 CPUs")
    if len(allowed) >= 3:
        two = {allowed[0], allowed[2]}
        n = shards_under(two)
        assert n == 2, f"S2: CPUs {sorted(two)} gave shards={n}, want 2"
        print(f"S2 two CPUs {sorted(two)}: shards={n}")
        ran += 1
    else:
        print("S2: skipped, needs 3 CPUs")
    n = shards_under(None)
    assert n == len(allowed), f"S3: unrestricted gave shards={n}, want {len(allowed)}"
    print(f"S3 unrestricted: shards={n}")
    ran += 1
    print(f"test_default_shards: ok ({ran} cases)")


if __name__ == "__main__":
    main()
