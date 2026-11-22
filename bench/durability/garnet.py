#!/usr/bin/env python3
"""bench/garnet (Garnet with the B1 account commands) under
bench/durability/dlw.py run, with the same mark check as snc.py.

  load   Garnet on $DLW_MNT/garnet with --aof --aof-commit-freq 0
         --aof-commit-wait and --device-type DEVICE, and resp_bench with
         --ack-log ACKS and the given arguments
  check  Garnet with --recover on the replayed data, and a GET of every
         account in ACKS

Usage: garnet.py load GARNET RESP_BENCH ACKS DEVICE RESP_BENCH_ARG...
       garnet.py check GARNET ACKS DEVICE START_BALANCE
"""
import os
import random
import socket
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snc  # noqa: E402


def server(garnet, device, *extra):
    port = random.randrange(20000, 32000)
    d = os.path.join(os.environ["DLW_MNT"], "garnet")
    log = open(os.path.join(os.path.dirname(os.environ["DLW_MNT"]), "server.log"), "a")
    proc = subprocess.Popen([garnet, "--bind", "127.0.0.1", "--port", str(port), "--checkpointdir", d, "--aof",
                             "--aof-commit-freq", "0", "--aof-commit-wait", "--device-type", device, "--memory", "1g",
                             "--index", "64m", *extra], stdout=log, stderr=log)
    if not snc.wait_port(port, proc, 120):
        proc.kill()
        sys.exit("garnet did not start")
    return proc, port


def stop(proc):
    proc.terminate()
    proc.wait()


def read_all(port, ids):
    """{id: balance or None}, GETs pipelined on one connection."""
    out = {}
    with socket.create_connection(("127.0.0.1", port), 10) as s:
        s.settimeout(30)
        f = s.makefile("rb")
        for i0 in range(0, len(ids), 512):
            chunk = ids[i0:i0 + 512]
            s.sendall(b"".join(b"*2\r\n$3\r\nGET\r\n$16\r\n" + struct.pack("<QQ", int(x[:16], 16), int(x[16:], 16))
                               + b"\r\n" for x in chunk))
            for x in chunk:
                line = f.readline()
                if line[:1] != b"$":
                    raise RuntimeError(f"GET: {line!r}")
                n = int(line[1:])
                if n < 0:
                    out[x] = None
                    continue
                v = f.read(n + 2)
                out[x] = struct.unpack_from("<q", v)[0]
    return out


def load(garnet, resp_bench, acks, device, *bench):
    proc, port = server(garnet, device)
    try:
        return subprocess.run([resp_bench, "--port", str(port), "--ack-log", acks, *bench], timeout=600).returncode
    finally:
        stop(proc)


def check(garnet, acks, device, start):
    def serve():
        proc, port = server(garnet, device, "--recover")
        return lambda: stop(proc), lambda ids: read_all(port, ids)
    return snc.check_with(acks, start, serve)


if __name__ == "__main__":
    sys.exit(load(*sys.argv[2:]) if sys.argv[1] == "load" else check(*sys.argv[2:]))
