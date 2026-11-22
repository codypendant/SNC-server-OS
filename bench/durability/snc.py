#!/usr/bin/env python3
"""snc_server under bench/durability/dlw.py run.

  load   (the dlw.py command) snc_server on $DLW_MNT/data, checkpointing every
         second, and net_bench with --ack-log ACKS and the given arguments
  check  (the dlw.py --check) snc_server on the replayed $DLW_MNT/data, and a
         read of every account in ACKS

The mark lies between $DLW_MARK_NS and $DLW_MARK_END_NS (the times taken
around writing it). An account whose create or any applied op was
acknowledged before the start must exist, with its start balance plus every
change acknowledged by then, moved by at most one cent per op sent before the
end and not acknowledged before the start (in doubt). Ops sent after the end
cannot be there. The sum of all balances is bounded the same way by deposits
alone, since a transfer moves no money. Cross-shard credits are re-delivered
after recovery, so the accounts are read until two reads in a row agree and
hold, or nothing has moved for 5 s. Exit 0 iff every account and the sum hold.

$DLW_IMAGE_END_NS (negative control): the image is from an earlier mark that
ended then. Acknowledged ops sent after it cannot be on the image ("removed");
if there are none, exit 3 (the control proves nothing).

Usage: snc.py load BUILD ACKS SHARDS OBJECTS NET_BENCH_ARG...
       snc.py check BUILD ACKS SHARDS OBJECTS START_BALANCE
"""
import json
import os
import random
import socket
import struct
import subprocess
import sys
import time

GET_BALANCE, EXEC, OK, NOT_FOUND, OVERLOADED = 1006, 1, 0, 1, 9


def bounds(acks, start_ns, end_ns, start, image_end_ns):
    """{id: [must exist, base, down, up]}, the sum's [base, up], and how many
    ops acknowledged before the mark were sent after the image's end."""
    acc, total, removed = {}, [0, 0], 0

    def at(x):
        return acc.setdefault(x, [False, start, 0, 0])

    for line in open(acks):
        kind, a, b, sent, acked = line.split()
        sent, acked = int(sent), int(acked)
        if kind == "c":
            at(a)[0] |= acked < start_ns
            continue
        if kind == "n" or sent >= end_ns:
            continue
        sure = kind == "a" and acked < start_ns
        removed += sure and sent >= image_end_ns
        src = at(a)
        if int(b, 16) == 0:
            total[not sure] += 1
            if sure:
                src[0], src[1] = True, src[1] + 1
            else:
                src[3] += 1
            continue
        dst = at(b)
        if sure:
            src[0] = dst[0] = True
            src[1] -= 1
            dst[1] += 1
        else:
            src[2] += 1
            dst[3] += 1
    return acc, total, removed


def read_all(port, ids):
    """{id: balance or None}, pipelined on one connection; overloaded reads are resent."""
    out = {}
    with socket.create_connection(("127.0.0.1", port), 10) as s:
        s.settimeout(30)
        todo, rounds = list(ids), 0
        while todo:
            rounds += 1
            if rounds > 1000:
                raise RuntimeError("reads stay overloaded")
            chunk, todo = todo[:256], todo[256:]
            s.sendall(b"".join(struct.pack("<IBBHIIQQQQ", 48, EXEC, 0, 0, GET_BALANCE, 0, i, int(x[:16], 16),
                                           int(x[16:], 16), 0) for i, x in enumerate(chunk)))
            buf, got = b"", 0
            while got < len(chunk):
                while len(buf) < 48 or len(buf) < struct.unpack_from("<I", buf)[0]:
                    data = s.recv(1 << 16)
                    if not data:
                        raise RuntimeError("connection closed")
                    buf += data
                n, = struct.unpack_from("<I", buf)
                st, = struct.unpack_from("<H", buf, 6)
                x = chunk[struct.unpack_from("<Q", buf, 16)[0]]
                if st == OK:
                    out[x] = struct.unpack_from("<q", buf, 48)[0]
                elif st == NOT_FOUND:
                    out[x] = None
                elif st == OVERLOADED:
                    todo.append(x)
                else:
                    raise RuntimeError(f"status {st}")
                buf, got = buf[n:], got + 1
    return out


def wait_port(port, proc, timeout=60):
    end = time.time() + timeout
    while time.time() < end and proc.poll() is None:
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def server(build, shards, objects, *extra):
    port = random.randrange(20000, 32000, 16)
    log = open(os.path.join(os.path.dirname(os.environ["DLW_MNT"]), "server.log"), "a")
    proc = subprocess.Popen([f"{build}/snc_server", "--shards", shards, "--port", str(port), "--objects", objects,
                             "--data-dir", os.path.join(os.environ["DLW_MNT"], "data"), *extra], stdout=log,
                            stderr=log)
    if not all(wait_port(port + i, proc) for i in range(int(shards))):
        proc.kill()
        sys.exit("snc_server did not start")
    return proc, port


def load(build, acks, shards, objects, *bench):
    proc, port = server(build, shards, objects, "--checkpoint-s", "1")
    try:
        return subprocess.run([f"{build}/net_bench", "--port", str(port), "--ack-log", acks, *bench],
                              timeout=600).returncode
    finally:
        proc.terminate()
        proc.wait()


def judge(acc, total, got, start):
    bad = []
    for x, (must, base, down, up) in acc.items():
        b = got[x]
        if b is None:
            if must:
                bad.append((x, "missing"))
        elif not base - down <= b <= base + up:
            bad.append((x, b, base - down, base + up))
    base = start * sum(v is not None for v in got.values()) + total[0]
    s = sum(v for v in got.values() if v is not None)
    if not base <= s <= base + total[1]:
        bad.append(("sum", s, base, base + total[1]))
    return bad


def check(build, acks, shards, objects, start):
    start_ns, end_ns, start = int(os.environ["DLW_MARK_NS"]), int(os.environ["DLW_MARK_END_NS"]), int(start)
    image_end_ns = int(os.environ.get("DLW_IMAGE_END_NS", end_ns))
    acc, total, removed = bounds(acks, start_ns, end_ns, start, image_end_ns)
    proc, port = server(build, shards, objects)
    try:
        ids, prev, reads = sorted(acc), None, 0
        t0 = moved = time.time()
        while True:
            got = read_all(port, ids)
            reads += 1
            bad = judge(acc, total, got, start)
            if got != prev:
                moved = time.time()
            elif not bad or time.time() - moved > 5 or time.time() - t0 > 60:
                break
            prev = got
            time.sleep(0.1)
    finally:
        proc.terminate()
        proc.wait()
    print(json.dumps({"mark_ns": start_ns, "mark_window_us": (end_ns - start_ns) // 1000, "accounts": len(acc),
                      "exist": sum(v is not None for v in got.values()), "must": sum(v[0] for v in acc.values()),
                      "in_doubt": sum(v[2] + v[3] > 0 for v in acc.values()), "reads": reads, "removed": removed,
                      "bad": len(bad), "first": bad[:3]}))
    if image_end_ns != end_ns and not removed:
        return 3
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(load(*sys.argv[2:]) if sys.argv[1] == "load" else check(*sys.argv[2:]))
