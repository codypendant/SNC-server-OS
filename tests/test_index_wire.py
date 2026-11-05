#!/usr/bin/env python3
"""X1 (docs/EXPERIMENTAL_BASELINE.md section 4): the INDEX wire kind keeps
index_lookup's semantics, checked on real snc_server processes.

  durable  A sync pair whose standby is SIGSTOPped (--replica-timeout-ms 0:
           the primary never continues alone). An account created then is
           applied but not durable: INDEX on the primary does not show it,
           while a durable account with the same email is shown. After
           SIGCONT the create is acknowledged and INDEX shows both. INDEX
           on the standby answers E_STANDBY.
  serving  Two replication groups (placement v2), accounts with one shared
           email, many of them in one slot S. Each group's primary answers
           for the slots it serves; together they list every account once.
           After S moves to the other group, the source no longer lists S's
           accounts and the destination does; still every account once.

  compat   (with SNC_OLD_BUILD=DIR, a build from before this kind) An
           old server answers INDEX with E_BAD_ARGS and keeps the
           connection: the next request on it, a TOPOLOGY, is answered.
           The new server answers the existing kinds as before (TOPOLOGY
           and CREATE here; test_net and the suites cover the rest).

Usage: test_index_wire.py BUILD_DIR [durable|serving|compat ...]
"""
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import shutil
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_async_failover as af  # noqa: E402
import test_fail_closed as fc  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
ONLY = sys.argv[2:]
INDEX, CREATE, TOPOLOGY, ACCOUNT, EMAIL_INDEX = 6, 2, 4, 256, 1
E_STANDBY, E_BAD_ARGS = 11, 4  # include/snc/types.h Status
OLD = os.environ.get("SNC_OLD_BUILD")


def index(port, key, max_results=4096, timeout=5):
    """(status, [ObjectID bytes])."""
    st, _, _, body = fc.request(port, INDEX, EMAIL_INDEX, b"\0" * 16, struct.pack("<II", max_results, 0) + key,
                                timeout)
    return st, sorted(body[i:i + 16] for i in range(0, len(body), 16))


def unacked(adm):
    """RESOURCES wal_unacked: records the standby has not acknowledged."""
    res = dict(x.split("=", 1) for x in (af.ask(adm, "RESOURCES", 3) or "").split() if "=" in x)
    return int(res["wal_unacked"])


def account_args(email):
    return struct.pack("<qQBB6s64s96s", 1000, 1, len(email), 1, b"", email, b"n")


def scenario_durable():
    work = tempfile.mkdtemp(prefix="snc-x1-")
    p_cli, s_cli, repl, p_adm, s_adm = af.free_ports(5)
    common = ["--space-uuid", "a1" * 16, "--objects", "10000", "--shards", "2"]
    sb = af.Proc([f"{BUILD}/snc_server", "--port", str(s_cli), "--data-dir", f"{work}/s", "--admin-port", str(s_adm),
                  "--standby", str(repl)] + common)
    procs = [sb]
    try:
        assert af.wait_port(s_adm), sb.text()
        prim = af.Proc([f"{BUILD}/snc_server", "--port", str(p_cli), "--data-dir", f"{work}/p", "--admin-port",
                        str(p_adm), "--replica-of", f"127.0.0.1:{repl}", "--replica-timeout-ms", "0"] + common)
        procs.append(prim)
        assert af.wait_port(p_adm) and af.wait_port(p_cli), prim.text()
        st, _, first, _ = fc.request(p_cli, CREATE, ACCOUNT, b"\0" * 16, account_args(b"z@x"))
        assert st == fc.OK, st
        st, ids = index(p_cli, b"z@x")
        assert st == fc.OK and ids == [first], (st, ids)
        # Applied but not durable (no standby ack): hidden.
        sb.signal(signal.SIGSTOP)
        time.sleep(0.2)
        before = unacked(p_adm)
        assert before == 0, before
        s = socket.create_connection(("127.0.0.1", p_cli), 5)
        args = account_args(b"z@x")
        s.sendall(struct.pack("<IBBHIIQ16sQ", 48 + len(args), CREATE, 0, 0, ACCOUNT, 0, 9, b"\0" * 16, 0) + args)
        time.sleep(0.5)
        s.settimeout(0.2)
        try:
            early = s.recv(48)
        except socket.timeout:
            early = b""
        assert early == b"", "a create was acknowledged without its standby"
        # The create was applied and logged: its record is in the primary's
        # WAL (0 unacknowledged before it, 1 now), waiting for the standby.
        # (Its ID is not known until it is answered, so it cannot be read
        # here; after SIGCONT the answer's ID is the one INDEX then lists.)
        during = unacked(p_adm)
        assert during > 0, during
        st, ids = index(p_cli, b"z@x")
        assert st == fc.OK and ids == [first], ("an undurable object was listed", st, ids)
        sb.signal(signal.SIGCONT)
        s.settimeout(10)
        resp = b""
        while len(resp) < 48:
            resp += s.recv(48 - len(resp))
        s.close()
        status, = struct.unpack_from("<H", resp, 6)
        second = resp[24:40]
        assert status == fc.OK, status
        st, ids = index(p_cli, b"z@x")
        assert st == fc.OK and ids == sorted([first, second]), (st, ids)
        st, _ = index(s_cli, b"z@x")
        assert st == E_STANDBY, st
        return {"undurable_hidden": True, "unacked_while_hidden": during, "after_ack": len(ids), "standby": "E_STANDBY"}
    finally:
        for p in procs:
            p.stop()
        shutil.rmtree(work, ignore_errors=True)


def scenario_serving():
    work = tempfile.mkdtemp(prefix="snc-x1-")
    c = fc.Cluster(work, groups=2, async_replica=False, v2=True)
    try:
        ids_path = f"{work}/ids"
        r = subprocess.run([f"{BUILD}/snc_load", "create", "--connect", f"127.0.0.1:{c.client[1]}", "--accounts", "60",
                            "--in-slot", "40", "--ids", ids_path], capture_output=True, text=True, timeout=120)
        assert r.returncode == 0, r.stdout + r.stderr
        slot = int(r.stdout.rsplit(" in slot ", 1)[1])
        every = sorted(fc.oid_bytes(x) for x in open(ids_path).read().split())
        hot = sorted(fc.oid_bytes(x) for x in open(ids_path + ".hot").read().split())

        def per_group():
            out = {}
            for k in (1, 3):
                st, v = index(c.client[k], b"e")
                assert st == fc.OK, (k, st)
                out[k] = v
            return out

        before = per_group()
        assert sorted(before[1] + before[3]) == every, "each account listed once by the group serving it"
        src = 2 * c.info(1, slot)["map_group"] + 1
        dst = 4 - src
        assert set(hot) <= set(before[src]) and not set(hot) & set(before[dst])
        out, err = c.ctl("move", slot, c.group_of(dst))
        assert out.startswith("DONE"), out + err
        after = per_group()
        assert sorted(after[1] + after[3]) == every, "after the move: each account listed once"
        assert not set(hot) & set(after[src]) and set(hot) <= set(after[dst])
        return {"accounts": len(every), "moved": len(hot), "source": src, "dest": dst}
    finally:
        c.stop()
        shutil.rmtree(work, ignore_errors=True)


def frame(kind, op, args=b"", req_id=1):
    return struct.pack("<IBBHIIQ16sQ", 48 + len(args), kind, 0, 0, op, 0, req_id, b"\0" * 16, 0) + args


def read_resp(s):
    buf = b""
    while len(buf) < 48 or len(buf) < struct.unpack_from("<I", buf, 0)[0]:
        chunk = s.recv(65536)
        if not chunk:
            raise RuntimeError("connection closed")
        buf += chunk
    n, = struct.unpack_from("<I", buf, 0)
    status, = struct.unpack_from("<H", buf, 6)
    return status, buf[48:n], buf[n:]


def index_then_topology(build):
    """One connection to a fresh server from `build`: INDEX, then TOPOLOGY."""
    work = tempfile.mkdtemp(prefix="snc-x1-")
    cli, adm = af.free_ports(2)
    p = af.Proc([f"{build}/snc_server", "--port", str(cli), "--data-dir", f"{work}/n", "--admin-port", str(adm),
                 "--space-uuid", "a2" * 16, "--objects", "1000", "--shards", "2"])
    try:
        assert af.wait_port(adm) and af.wait_port(cli), p.text()
        with socket.create_connection(("127.0.0.1", cli), 5) as s:
            s.settimeout(5)
            s.sendall(frame(INDEX, EMAIL_INDEX, struct.pack("<II", 10, 0) + b"e", 1))
            st_index, body, rest = read_resp(s)
            assert rest == b""
            s.sendall(frame(TOPOLOGY, 0, b"", 2))
            st_topo, topo, _ = read_resp(s)
        st_create, _, _, _ = fc.request(cli, CREATE, ACCOUNT, b"\0" * 16, account_args(b"e"))
        return st_index, len(body), st_topo, len(topo), st_create
    finally:
        p.stop()
        shutil.rmtree(work, ignore_errors=True)


def scenario_compat():
    assert OLD, "compat needs SNC_OLD_BUILD (a build from before the INDEX kind)"
    old = index_then_topology(OLD)
    assert old[0] == E_BAD_ARGS and old[1] == 0, ("old server", old)
    assert old[2] == fc.OK and old[3] > 0, ("old server dropped the connection or TOPOLOGY", old)
    new = index_then_topology(BUILD)
    assert new[0] == fc.OK and new[1] == 0, ("new server, empty index", new)
    assert new[2] == fc.OK and new[3] == old[3] and new[4] == fc.OK == old[4], (new, old)
    return {"old_index": "E_BAD_ARGS, connection kept", "topology_bytes": new[3]}


SCEN = {"durable": scenario_durable, "serving": scenario_serving, "compat": scenario_compat}


def main():
    # compat runs only by name (it needs an older build).
    for name in ONLY or [n for n in SCEN if n != "compat"]:
        t = time.time()
        r = SCEN[name]()
        print(f"{name}: ok {r} ({time.time() - t:.1f}s)", flush=True)
    print("test_index_wire: OK")


if __name__ == "__main__":
    main()
