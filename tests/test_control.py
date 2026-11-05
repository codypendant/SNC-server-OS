#!/usr/bin/env python3
"""The control plane end to end (docs/CONTROL_PLANE.md, step 2): real
snc_server nodes and one quorum of three snc_witness processes holding the
placement map, the mint lease blocks and one HA machine per replication
group.

Two groups of a primary and a sync standby, started by NodeID from the map
(snc_ctl init). Group 1's primary is SIGKILLed; the witnesses promote its
standby. Checked:
  - group 1's standby serves as primary, term 1, with a fresh lease block
    of its own (disjoint from every other block handed out);
  - group 0's HA machine and its nodes are untouched (term 0, same version);
  - placement is unchanged (version 1, epoch 1 on every live node);
  - every node took its lease block from the control plane, all disjoint.

Usage: test_control.py BUILD_DIR
"""
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from witness_state import first_start  # noqa: E402


def free_ports(n, span=1):
    """n bases, each with `span` consecutive ports free to bind now, below
    the ephemeral range (32768 up), and not overlapping."""
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
                socks.append(s)
                s.bind(("127.0.0.1", b + k))
            bases.append(b)
            taken.update(b + k for k in range(span))
        except OSError:
            pass
        finally:
            for s in socks:
                s.close()
    return bases


def ask(port, line, timeout=3):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout) as s:
            s.settimeout(timeout)
            s.sendall((line + "\n").encode())
            return s.makefile().readline().strip()
    except OSError:
        return None


def status(port):
    r = ask(port, "STATUS", 1)
    if not r:
        return None
    return dict(kv.split("=", 1) for kv in r.split() if "=" in kv)


def wait(cond, what, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        v = cond()
        if v:
            return v
        time.sleep(0.05)
    raise AssertionError(f"timed out: {what}")


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build"
    work = tempfile.mkdtemp(prefix="snc-control-")
    procs = []
    try:
        # Three witnesses; per node an admin port, and two-port bases (two
        # shards) for node links, clients and replication.
        p = free_ports(3 + 4 * 4, span=2)
        wports, nodes = p[:3], [p[3 + 4 * i: 3 + 4 * (i + 1)] for i in range(4)]
        control = ",".join(f"127.0.0.1:{w}" for w in wports)
        admin = {i + 1: nodes[i][0] for i in range(4)}
        link = {i + 1: nodes[i][1] for i in range(4)}
        client = {i + 1: nodes[i][2] for i in range(4)}
        repl = {g: nodes[2 * g + 1][3] for g in range(2)}  # the standby's replication listener
        # Group g: node 2g+1 starts as primary, node 2g+2 as its standby.
        groups = ",".join(
            f"2:{2 * g + 1}/{link[2 * g + 1]}/{client[2 * g + 1]}+{2 * g + 2}/{link[2 * g + 2]}/{client[2 * g + 2]}"
            for g in range(2))
        node_list = ",".join(f"{i}@127.0.0.1:{admin[i]}" for i in range(1, 5))
        for i, w in enumerate(wports):
            args = [f"{build}/snc_witness", "--port", str(w), "--state", f"{work}/w{i}", "--id", str(i),
                    "--peers", control, "--failover-ms", "400", "--poll-ms", "50"]
            for g in range(2):
                args += ["--group", f"{g}=127.0.0.1:{admin[2 * g + 1]},127.0.0.1:{admin[2 * g + 2]}"]
            procs.append(subprocess.Popen(first_start(args), stdout=open(f"{work}/w{i}.log", "w"),
                                          stderr=subprocess.STDOUT))
        time.sleep(0.3)
        r = subprocess.run([f"{build}/snc_ctl", "--control", control, "init", "--nodes", node_list,
                            "--groups", groups], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr

        servers = {}

        def start(i, standby):
            g = (i - 1) // 2
            args = [f"{build}/snc_server", "--shards", "2", "--objects", "10000", "--data-dir", f"{work}/n{i}",
                    "--control", control, "--node-id", str(i), "--admin-port", str(admin[i]),
                    "--witness", control]
            if standby:
                args += ["--standby", str(repl[g])]
            else:
                args += ["--replica-of", f"127.0.0.1:{repl[g]}", "--replica-timeout-ms", "300"]
            servers[i] = subprocess.Popen(args, stdout=open(f"{work}/n{i}.log", "w"), stderr=subprocess.STDOUT)
            procs.append(servers[i])

        for i in (2, 4):
            start(i, True)
        for i in (2, 4):
            wait(lambda: status(admin[i]), f"standby {i} up")
        for i in (1, 3):
            start(i, False)
        st = {i: wait(lambda: status(admin[i]), f"node {i} up") for i in range(1, 5)}
        assert [st[i]["role"] for i in range(1, 5)] == ["p", "s", "p", "s"], st
        # The witnesses learn both primaries, each on its own machine.
        wait(lambda: all((ask(wports[0], f"READ ha/{g}") or "").split()[1:2] != ["0"] for g in range(2)),
             "both groups' primaries learned")
        # And both primaries prove their standby in sync (READY: degraded 0);
        # until then no standby may be promoted (docs/SYNC_READINESS.md).
        wait(lambda: all((ask(wports[0], f"READ ha/{g}") or "").split()[4:5] == ["0"] for g in range(2)),
             "both groups READY")
        before = {m: ask(wports[0], f"READ {m}") for m in ["ha/0", "ha/1", "placement", "leases"]}
        blocks = {i: tuple(map(int, st[i]["leases"].split("-"))) for i in (1, 3)}

        # Group 1's primary dies; its witnesses (the shared quorum) promote
        # its standby.
        servers[3].send_signal(signal.SIGKILL)
        servers[3].wait()
        st4 = wait(lambda: (lambda s: s if s and s["role"] in ("p", "n") else None)(status(admin[4])),
                   "group 1's standby promoted")
        assert st4["term"] == "1" and st4["epoch"] == "1", st4
        blocks[4] = tuple(map(int, st4["leases"].split("-")))
        time.sleep(0.5)
        after = {m: ask(wports[0], f"READ {m}") for m in before}

        # Group 0: nothing changed.
        assert after["ha/0"] == before["ha/0"], (before["ha/0"], after["ha/0"])
        for i in (1, 2):
            s = status(admin[i])
            assert s["term"] == "0" and s["epoch"] == "1", s
        # Group 1: a new term on its own machine.
        v0, v1 = int(before["ha/1"].split()[1]), int(after["ha/1"].split()[1])
        assert v1 > v0 and after["ha/1"].split()[3] == "1", (before["ha/1"], after["ha/1"])
        # Placement: same version, same map.
        assert after["placement"] == before["placement"], "placement moved"
        assert after["placement"].split()[1] == "1"
        # Leases: every block taken from the control plane, all disjoint, the
        # promoted member's fresh.
        lo = [b[0] for b in blocks.values()]
        assert all(b[0] >= 1 << 32 and b[1] > b[0] for b in blocks.values()), blocks
        spans = sorted(blocks.values())
        assert all(spans[k][1] <= spans[k + 1][0] for k in range(len(spans) - 1)), blocks
        assert blocks[4][0] > max(blocks[1][0], blocks[3][0]), blocks
        print(f"control plane: 2 groups on one witness quorum; group 1's primary killed, its standby promoted "
              f"to term 1 by ha/1 with a fresh lease block {blocks[4]}; ha/0 ({before['ha/0']}) and placement "
              f"(version 1, epoch 1 on every node) unchanged; lease blocks disjoint: {sorted(lo)}")
        print("test_control: ok")
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
            p.wait()
        if os.environ.get("SNC_KEEP"):
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
