#!/usr/bin/env python3
"""Listener exposure and AUDIT NAME (AFR-F-02, slice AFR-S-2;
docs/LISTENER_EXPOSURE.md).

Every listener snc_server and snc_witness open binds one address the
operator names (--host), loopback by default; AUDIT writes balances only to a
plain file name inside --audit-dir. Real processes, deterministic: each case
reads the kernel's table of listening sockets (/proc/net/tcp) for the ports
it gave, or the file system after one admin request.

  L1 defaults: snc_server's client data, admin, seed and standby replication
     ports, a cluster member's node-link ports and the witness port all
     listen on 127.0.0.1 only; with a non-loopback address of this machine
     at hand, a connection to it is refused.
  L2 explicit --host 0.0.0.0 (snc_server and snc_witness) binds every
     interface, and --standby-host still overrides the replication ports:
     the deliberate opt-in works.
  L3 AUDIT with a path does not touch the file the path names (on the old
     code it truncated it, any peer of the admin port could); AUDIT NAME
     without --audit-dir is refused; with it, the balances land in the
     directory; a name with '/', a leading '.', or a symbolic link in the
     directory is refused and the link's target is untouched; a FIFO or
     a directory is refused; a hard link named NAME is replaced, and the
     file it pointed at is untouched.
  L4 refusals at start, exit 2 with the fix named: a cluster member whose
     map address is not loopback and no --host; a witness whose --peers
     entry is a non-loopback address of this machine and no --host (needs
     one, else skipped), also with an explicit --host 127.0.0.1;
     --audit-dir equal to or inside --data-dir (also through a symbolic
     link) or holding --links-generation-file; --host that is not an IPv4
     address; a map address the bind does not serve (127.0.0.2 on the
     default 127.0.0.1, or a different --host); a loopback-bound node whose
     --witness, --seed-from or --replica-of (with --seed-port) peer is on
     another machine, and a loopback-bound witness watching nodes on
     another machine. A map naming the node by a name that resolves to
     loopback starts; with --host 0.0.0.0 the member listens on every interface.

  L5 --control: a node's admin, client and link ports on loopback without
     --host; a node the placement map names at a non-loopback address is
     refused without --host.

L1, L3, L4 and L5 fail on the code before AFR-S-2 (L2 only on its new log
line) (admin, seed, node-link and
witness ports on 0.0.0.0; a standby replication port on 0.0.0.0; a cluster
member under --control on 0.0.0.0; AUDIT /path truncating the file).

Usage: test_listen_exposure.py BUILD_DIR [CASE ...]
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
SERVER = os.path.join(BUILD, "snc_server")
WITNESS = os.path.join(BUILD, "snc_witness")


def free_ports(n, span=1):
    """A run of n*span free ports below the ephemeral range."""
    want = n * span
    for base in range(20000 + (os.getpid() % 400) * 25, 32000, 41):
        try:
            for i in range(want):
                s = socket.socket()
                try:
                    s.bind(("0.0.0.0", base + i))
                finally:
                    s.close()
            return [base + i * span for i in range(n)]
        except OSError:
            continue
    raise RuntimeError("no free port run")


def listening():
    """{port: set of local IPv4 addresses} of LISTEN sockets (/proc/net/tcp)."""
    out = {}
    with open("/proc/net/tcp") as f:
        next(f)
        for line in f:
            p = line.split()
            if p[3] != "0A":  # TCP_LISTEN
                continue
            hexaddr, hexport = p[1].split(":")
            addr = socket.inet_ntoa(bytes.fromhex(hexaddr)[::-1])
            out.setdefault(int(hexport, 16), set()).add(addr)
    return out


def other_address():
    """A non-loopback IPv4 address of this machine, or None. A UDP connect
    sends nothing; it only picks the source address a route would use."""
    for dst in ("192.0.2.1", "10.255.255.255", "8.8.8.8"):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect((dst, 9))
            a = s.getsockname()[0]
            s.close()
            if not a.startswith("127.") and a != "0.0.0.0":
                return a
        except OSError:
            continue
    return None


def wait(pred, what, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


def ask(port, line, timeout=10.0, host="127.0.0.1"):
    s = socket.create_connection((host, port), timeout=timeout)
    try:
        s.sendall((line + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            b = s.recv(65536)
            if not b:
                break
            buf += b
        return buf.decode().strip()
    finally:
        s.close()


def refused(host, port):
    """True when a TCP connection to host:port is refused (nothing listens
    there for that address)."""
    try:
        s = socket.create_connection((host, port), timeout=2)
        s.close()
        return False
    except ConnectionRefusedError:
        return True


class Procs:
    def __init__(self, work):
        self.work = work
        self.ps = []

    def start(self, name, args):
        log = open(os.path.join(self.work, f"{name}.log"), "w")
        p = subprocess.Popen([str(a) for a in args], stdout=log, stderr=subprocess.STDOUT)
        self.ps.append(p)
        return p

    def log(self, name):
        with open(os.path.join(self.work, f"{name}.log")) as f:
            return f.read()

    def stop(self):
        for p in self.ps:
            if p.poll() is None:
                p.kill()
        for p in self.ps:
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        self.ps = []


def run_exit(args, timeout=20):
    """Runs a process expected to exit at once: (returncode or None, output)."""
    try:
        r = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout + r.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode() + (e.stderr or b"").decode() if isinstance(e.stdout, bytes) else ""
        return None, out


def expect_bound(ports, addr, what):
    """Each port in `ports` listens on exactly {addr}."""
    tab = wait(lambda: (lambda t: t if all(p in t for p in ports) else None)(listening()),
               f"{what} listening on {ports}")
    bad = {p: sorted(tab.get(p, set())) for p in ports if tab.get(p) != {addr}}
    assert not bad, f"{what}: expected {addr} only, got {bad}"


# ---- cases ---------------------------------------------------------------------------------


def l1_defaults(work):
    pr = Procs(work)
    other = other_address()
    try:
        data, admin, seed, sdata, repl = free_ports(5)
        pr.start("primary", [SERVER, "--shards", 1, "--objects", 1000, "--data-dir", f"{work}/p", "--port", data,
                             "--admin-port", admin, "--seed-port", seed])
        pr.start("standby", [SERVER, "--shards", 1, "--objects", 1000, "--data-dir", f"{work}/s", "--port", sdata,
                             "--standby", repl])
        expect_bound([data, admin, seed], "127.0.0.1", "primary data/admin/seed")
        expect_bound([sdata, repl], "127.0.0.1", "standby data/replication")
        # A cluster member (in memory, so links open at once): node links.
        l0, l1, c0, c1 = free_ports(4, span=2)
        cluster = f"127.0.0.1:{l0}:1:{c0},127.0.0.1:{l1}:1:{c1}"
        pr.start("member", [SERVER, "--shards", 1, "--objects", 1000, "--port", c0, "--cluster", cluster,
                            "--group", 0, "--member", 0, "--links-generation-file", f"{work}/gen"])
        expect_bound([l0, c0], "127.0.0.1", "cluster member link/data")
        (w,) = free_ports(1)
        pr.start("witness", [WITNESS, "--port", w, "--state", f"{work}/w.state", "--new-state", 1])
        expect_bound([w], "127.0.0.1", "witness")
        assert "loopback only" in pr.log("primary"), pr.log("primary")
        if other:
            for p in (data, admin, seed, repl, l0, w):
                assert refused(other, p), f"port {p} reachable at {other}"
            print(f"  L1: every port refused at {other}")
        else:
            print("  L1: no non-loopback address here; the /proc/net/tcp check stands alone")
    finally:
        pr.stop()


def l2_explicit(work):
    pr = Procs(work)
    try:
        data, admin, seed, sdata, repl = free_ports(5)
        pr.start("primary", [SERVER, "--shards", 1, "--objects", 1000, "--data-dir", f"{work}/p", "--port", data,
                             "--admin-port", admin, "--seed-port", seed, "--host", "0.0.0.0"])
        pr.start("standby", [SERVER, "--shards", 1, "--objects", 1000, "--data-dir", f"{work}/s", "--port", sdata,
                             "--standby", repl, "--host", "0.0.0.0", "--standby-host", "127.0.0.1"])
        expect_bound([data, admin, seed], "0.0.0.0", "primary with --host 0.0.0.0")
        expect_bound([sdata], "0.0.0.0", "standby data with --host 0.0.0.0")
        expect_bound([repl], "127.0.0.1", "standby replication with --standby-host 127.0.0.1")
        assert "every interface" in pr.log("primary"), pr.log("primary")
        (w,) = free_ports(1)
        pr.start("witness", [WITNESS, "--port", w, "--state", f"{work}/w.state", "--new-state", 1,
                             "--host", "0.0.0.0"])
        expect_bound([w], "0.0.0.0", "witness with --host 0.0.0.0")
        r = ask(admin, "HEALTH")
        assert r.startswith("role="), r
    finally:
        pr.stop()


def l3_audit(work):
    pr = Procs(work)
    try:
        victim = os.path.join(work, "victim")
        with open(victim, "w") as f:
            f.write("must survive\n")
        outside = os.path.join(work, "outside")
        with open(outside, "w") as f:
            f.write("link target\n")
        adir = os.path.join(work, "audit")
        os.mkdir(adir)
        os.symlink(outside, os.path.join(adir, "link"))
        hard_target = os.path.join(work, "hard_target")
        with open(hard_target, "w") as f:
            f.write("hard target\n")
        os.link(hard_target, os.path.join(adir, "hard"))
        os.mkfifo(os.path.join(adir, "fifo"))
        os.mkdir(os.path.join(adir, "sub"))
        data, admin, data2, admin2 = free_ports(4)
        # Without --audit-dir.
        pr.start("plain", [SERVER, "--shards", 1, "--objects", 1000, "--data-dir", f"{work}/p", "--port", data,
                           "--admin-port", admin])
        wait(lambda: admin in listening(), "admin port")
        r = ask(admin, f"AUDIT {victim}")
        assert r.startswith("NO "), r
        r = ask(admin, "AUDIT bal")
        assert r.startswith("NO ") and "--audit-dir" in r, r
        r = ask(admin, "AUDIT")
        assert r.startswith("AUDIT "), r
        # With --audit-dir.
        pr.start("audited", [SERVER, "--shards", 1, "--objects", 1000, "--data-dir", f"{work}/q", "--port", data2,
                             "--admin-port", admin2, "--audit-dir", adir])
        wait(lambda: admin2 in listening(), "admin port (audited)")
        for bad in (victim, "../victim", "a/b", ".hidden", "..", "x" * 129, "link", "fifo", "sub"):
            r = ask(admin2, f"AUDIT {bad}")
            assert r.startswith("NO "), (bad, r)
        r = ask(admin2, "AUDIT bal.1")
        assert r.startswith("AUDIT "), r
        assert os.path.isfile(os.path.join(adir, "bal.1")), os.listdir(adir)
        # A hard link named NAME is replaced, not written through (QA 1).
        r = ask(admin2, "AUDIT hard")
        assert r.startswith("AUDIT "), r
        with open(hard_target) as f:
            assert f.read() == "hard target\n", "AUDIT wrote through a hard link"
        assert sorted(os.listdir(adir)) == ["bal.1", "fifo", "hard", "link", "sub"], os.listdir(adir)
        with open(victim) as f:
            assert f.read() == "must survive\n", "AUDIT PATH touched the file a path names"
        with open(outside) as f:
            assert f.read() == "link target\n", "AUDIT followed a symbolic link out of --audit-dir"
    finally:
        pr.stop()


def l4_refusals(work):
    other = other_address()
    l0, l1, c0, c1 = free_ports(4, span=2)
    cluster = f"192.0.2.77:{l0}:1:{c0},127.0.0.1:{l1}:1:{c1}"
    rc, out = run_exit([SERVER, "--shards", 1, "--objects", 1000, "--port", c0, "--cluster", cluster, "--group", 0,
                        "--member", 0, "--links-generation-file", f"{work}/gen"])
    assert rc == 2 and "--host 192.0.2.77" in out, (rc, out)
    # The same map with the address given (or 0.0.0.0) is not refused for it.
    pr = Procs(work)
    try:
        pr.start("open", [SERVER, "--shards", 1, "--objects", 1000, "--port", c0, "--cluster", cluster, "--group", 0,
                          "--member", 0, "--links-generation-file", f"{work}/gen", "--host", "0.0.0.0"])
        expect_bound([c0, l0], "0.0.0.0", "member with --host 0.0.0.0")
    finally:
        pr.stop()
    # An explicit --host 127.0.0.1 is refused the same way (no listener
    # would serve the map's address).
    rc, out = run_exit([SERVER, "--shards", 1, "--objects", 1000, "--port", c0, "--cluster", cluster, "--group", 0,
                        "--member", 0, "--links-generation-file", f"{work}/gen", "--host", "127.0.0.1"])
    assert rc == 2 and "--host 192.0.2.77" in out, (rc, out)
    # The bind must serve the map address exactly: 127.0.0.2 is loopback,
    # but a socket on 127.0.0.1 refuses it; and a different --host (QA 2).
    for host_args, addr in (([], "127.0.0.2"), (["--host", "127.0.0.1"], "192.0.2.77")):
        m = f"{addr}:{l0}:1:{c0},127.0.0.1:{l1}:1:{c1}"
        rc, out = run_exit([SERVER, "--shards", 1, "--objects", 1000, "--port", c0, "--cluster", m, "--group", 0,
                            "--member", 0, "--links-generation-file", f"{work}/gen"] + host_args)
        assert rc == 2 and f"--host {addr}" in out, (addr, rc, out)
    # Peers on another machine that dial back (QA 2), each refused on loopback.
    (adm,) = free_ports(1)
    for extra, word in ((["--admin-port", adm, "--witness", "192.0.2.77:1"], "--witness"),
                        (["--data-dir", f"{work}/sb", "--standby", adm, "--seed-from", "192.0.2.77:1"], "--seed-from"),
                        (["--data-dir", f"{work}/pr", "--replica-of", "192.0.2.77:1", "--seed-port", adm],
                         "--replica-of")):
        rc, out = run_exit([SERVER, "--shards", 1, "--objects", 1000, "--port", c0] + extra)
        assert rc == 2 and word in out and "another machine" in out, (extra, rc, out)
    rc, out = run_exit([WITNESS, "--port", adm, "--state", f"{work}/wg.state", "--new-state", 1, "--group",
                        "0=192.0.2.77:1,192.0.2.78:2"])
    assert rc == 2 and "another machine" in out, (rc, out)
    rc, out = run_exit([SERVER, "--shards", 1, "--objects", 1000, "--host", "localhost"])
    assert rc == 2 and "not an IPv4 address" in out, (rc, out)
    os.mkdir(os.path.join(work, "d"))
    for adir in (f"{work}/d", f"{work}/d/sub"):
        os.makedirs(adir, exist_ok=True)
        rc, out = run_exit([SERVER, "--shards", 1, "--objects", 1000, "--data-dir", f"{work}/d", "--audit-dir", adir])
        assert rc == 2 and "data directory" in out, (adir, rc, out)
    rc, out = run_exit([SERVER, "--shards", 1, "--objects", 1000, "--audit-dir", f"{work}/missing"])
    assert rc == 2 and "No such file or directory" in out, (rc, out)
    # --audit-dir holding the links generation file (QA 1).
    os.mkdir(f"{work}/lg")
    rc, out = run_exit([SERVER, "--shards", 1, "--objects", 1000, "--links-generation-file", f"{work}/lg/gen",
                        "--audit-dir", f"{work}/lg"])
    assert rc == 2 and "--links-generation-file" in out, (rc, out)
    # --audit-dir reached through a symbolic link to the data directory.
    os.symlink(f"{work}/d", f"{work}/dlink")
    rc, out = run_exit([SERVER, "--shards", 1, "--objects", 1000, "--data-dir", f"{work}/d", "--audit-dir",
                        f"{work}/dlink/sub"])
    assert rc == 2 and "data directory" in out, (rc, out)
    # A map that names this node by a name resolving to loopback is not
    # refused (node links resolve names; QA 1 and 3).
    l2, l3, c2, c3 = free_ports(4, span=2)
    rc, out = run_exit([SERVER, "--shards", 1, "--objects", 1000, "--port", c2, "--cluster",
                        f"localhost:{l2}:1:{c2},127.0.0.1:{l3}:1:{c3}", "--group", 0, "--member", 0,
                        "--links-generation-file", f"{work}/gen2"], timeout=3)
    assert rc is None, (rc, out)
    if other:
        a, b, c = free_ports(3)
        peers = f"{other}:{a},127.0.0.1:{b},127.0.0.1:{c}"
        rc, out = run_exit([WITNESS, "--port", a, "--state", f"{work}/w.state", "--new-state", 1, "--id", 0,
                            "--peers", peers])
        assert rc == 2 and f"--host {other}" in out, (rc, out)
        assert not os.path.exists(f"{work}/w.state"), "a refused witness created its state"
        rc, out = run_exit([WITNESS, "--port", a, "--state", f"{work}/w.state", "--new-state", 1, "--id", 0,
                            "--peers", peers, "--host", "127.0.0.1"])
        assert rc == 2 and f"--host {other}" in out, (rc, out)
        print(f"  L4: witness refusal checked at {other}")
    else:
        print("  L4: no non-loopback address here; the witness refusal is not exercised")


def l5_control(work):
    """--control: before AFR-S-2 the client data port went to 0.0.0.0 when
    --host was not given (and admin and links always did). Now loopback; a
    placement map that names the node at a non-loopback address refuses
    the start without --host."""
    pr = Procs(work)
    try:
        wports = free_ports(3)
        control = ",".join(f"127.0.0.1:{w}" for w in wports)
        for i, w in enumerate(wports):
            pr.start(f"w{i}", [WITNESS, "--port", w, "--state", f"{work}/w{i}", "--id", i, "--peers", control,
                               "--new-state", 1])
        wait(lambda: all(w in listening() for w in wports), "witnesses")
        admin, link, client, admin2, link2, client2 = free_ports(6, span=2)
        groups = f"1:1/{link}/{client},1:2/{link2}/{client2}"
        nodes = f"1@127.0.0.1:{admin},2@192.0.2.77:{admin2}"
        r = subprocess.run([os.path.join(BUILD, "snc_ctl"), "--control", control, "init", "--nodes", nodes,
                            "--groups", groups], capture_output=True, text=True, timeout=30)
        assert r.returncode == 0, r.stdout + r.stderr
        pr.start("n1", [SERVER, "--objects", 1000, "--data-dir", f"{work}/n1", "--control", control, "--node-id", 1,
                        "--admin-port", admin])
        expect_bound([admin, client, link], "127.0.0.1", "--control node 1 admin/client/link")
        rc, out = run_exit([SERVER, "--objects", 1000, "--data-dir", f"{work}/n2", "--control", control,
                            "--node-id", 2, "--admin-port", admin2], timeout=30)
        assert rc == 2 and "--host 192.0.2.77" in out, (rc, out)
    finally:
        pr.stop()


CASES = {"l1_defaults": l1_defaults, "l2_explicit": l2_explicit, "l3_audit": l3_audit, "l4_refusals": l4_refusals,
         "l5_control": l5_control}


def main():
    names = sys.argv[2:] or list(CASES)
    failed = []
    for name in names:
        work = tempfile.mkdtemp(prefix=f"snc-listen-{name}-")
        try:
            CASES[name](work)
            print(f"{name}: PASS")
        except AssertionError as e:
            print(f"{name}: FAIL {e}")
            failed.append(name)
        finally:
            shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
