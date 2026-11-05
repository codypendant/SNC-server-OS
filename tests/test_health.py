#!/usr/bin/env python3
"""HA health-path decoupling (docs/HEALTH_DECOUPLING.md, section 6).

Real processes: a sync pair of snc_server nodes (2 shards each, durable,
--debug-admin) and three replicated snc_witness processes (--failover-ms
1000, --poll-ms 100), configured as bench/run_failover.py does with rearm:
each node can serve as the other's primary or standby, and node 0
restarted with its own command line rejoins (--rejoin). A background load
creates accounts on the primary all along; a batch counts as acknowledged
only if every create in it answered OK.

Stalls are injected with DEBUG STALL <shard> <ms> on the admin port: a task
that spins on that shard's thread, in the real process (--debug-admin).

  T1  stall shard 0 of the primary 3000 ms, every witness on HEALTH:
      HEALTH keeps answering, stall_ms reports the stall, STATUS is slow
      meanwhile, no promotion, HA state version unchanged, progress resumes,
      no acknowledged create lost.
  T2  the same with every witness on --health-verb status: the false
      promotion reproduces (the fix is the verb, not a timing change).
  T3  (option B) stall 6000 ms, far past failover_ms: HEALTH keeps
      answering, stall_ms keeps rising, no promotion, service resumes with
      no change of authority, no acknowledged create lost.
  T4  SIGKILL the primary: promotion; no acknowledged create lost.
  T5  SIGSTOP the primary: promotion; after SIGCONT the old primary is
      fenced (role f); no acknowledged create lost.
  T6  partition the primary (a proxy on every path to and from it stops
      forwarding): promotion; the old primary acknowledges nothing to a
      client on its side of the cut; once the cut heals it is fenced; no
      acknowledged create lost.
  T8  node 0 killed, node 1 promoted, then shard 0 of node 1 stalled
      5000 ms while node 0 restarts with --rejoin: node 0 finds node 1 as
      primary through HEALTH and starts as its standby.
  T10 mixed witnesses, two on HEALTH and one on STATUS, with T1's stall:
      the STATUS witness still causes the promotion (rollout: every witness
      of a group must poll HEALTH).
  T11 admin pressure: 300 connections that never send a line and an AUDIT
      running, then HEALTH: answered; the half-read bound evicts; heavy
      requests beyond the queue bound get "NO busy"; DEBUG STALL is refused
      without --debug-admin.

T7 (eligibility) and T10's fallback and upgrade pickup use fake nodes:
tests/test_witness.py and test_health_fake() below.

Latency bounds (HEALTH within poll_ms) are asserted only when
SNC_SANITIZED is unset; under ASan/TSan they are printed, not asserted.

Usage: test_health.py BUILD_DIR [TEST ...]   (default: all)
"""
import os
import random
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
ONLY = sys.argv[2:]
UUID = "c3" * 16
SANITIZED = bool(os.environ.get("SNC_SANITIZED"))
FAILOVER_MS, POLL_MS = 1000, 100


def log(*a):
    print(*a, flush=True)


def free_block(n, step=1):
    """A random base port such that base + i*step (i < n) are all free."""
    rnd = random.Random(os.getpid() * 7919 + time.time_ns())
    while True:
        base = rnd.randrange(20000, 31000)
        socks, ok = [], True
        for i in range(n):
            s = socket.socket()
            try:
                s.bind(("127.0.0.1", base + i * step))
            except OSError:
                ok = False
            socks.append(s)
        for s in socks:
            s.close()
        if ok:
            return base


def wait_port(port, timeout=60, proc=None):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            return True
        except OSError:
            if proc is not None and proc.p.poll() is not None:
                return False
            time.sleep(0.05)
    return False


class Proc:
    def __init__(self, args):
        self.args = args
        self.p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.lines = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(line.rstrip())

    def wait_for(self, pattern, timeout):
        t_end = time.time() + timeout
        while time.time() < t_end:
            for l in list(self.lines):
                if re.search(pattern, l):
                    return l
            time.sleep(0.01)
        return None

    def text(self):
        return "\n".join(self.lines[-60:])

    def signal(self, sig):
        if self.p.poll() is None:
            self.p.send_signal(sig)

    def stop(self):
        self.signal(signal.SIGCONT)
        self.signal(signal.SIGKILL)
        try:
            self.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            pass


def ask(port, line, timeout=2.0):
    """(reply or None, seconds)."""
    t0 = time.time()
    try:
        with socket.create_connection(("127.0.0.1", port), timeout) as s:
            s.settimeout(timeout)
            s.sendall((line + "\n").encode())
            r = s.makefile().readline().strip()
            return (r if r else None), time.time() - t0
    except OSError:
        return None, time.time() - t0


def fields(reply):
    return dict(kv.split("=", 1) for kv in (reply or "").split() if "=" in kv)


def count_missing(port, ids_path):
    """Reads every account in ids_path through shard 0's port (it forwards
    to the owner); returns how many do not answer OK."""
    with open(ids_path, "rb") as f:
        raw = f.read()
    ids = [raw[i:i + 16] for i in range(0, len(raw), 16)]
    if not ids:
        return 0
    s = socket.create_connection(("127.0.0.1", port), 5)
    s.settimeout(20)
    for n, oid in enumerate(ids):
        s.sendall(struct.pack("<IBBHIIQ16sQ", 48, 1, 0, 0, 1006, 0, n + 1, oid, 0))
    buf, missing, got = b"", 0, 0
    while got < len(ids):
        chunk = s.recv(65536)
        if not chunk:
            raise RuntimeError("connection closed")
        buf += chunk
        while len(buf) >= 48:
            length, = struct.unpack_from("<I", buf, 0)
            if len(buf) < length:
                break
            status, = struct.unpack_from("<H", buf, 6)
            missing += status != 0
            got += 1
            buf = buf[length:]
    s.close()
    return missing


class Load:
    """Creates accounts in batches of 100 on `port` until stopped. A batch
    is acknowledged only if all of its creates answered OK (net_bench
    writes the ids file only then)."""

    def __init__(self, port, work, name="load"):
        self.port, self.work, self.name = port, work, name
        self.acked, self.failed, self.stop_flag = [], 0, False
        self.last_ok = 0.0
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        n = 0
        while not self.stop_flag:
            n += 1
            path = f"{self.work}/{self.name}.{n}"
            try:
                r = subprocess.run(
                    [f"{BUILD}/net_bench", "--port", str(self.port), "--threads", "1", "--objects", "100",
                     "--seconds", "0.02", "--route", "owner", "--reads", "100", "--depth", "16", "--ids", path],
                    capture_output=True, text=True, timeout=30)
                ok = r.returncode == 0 and os.path.exists(path)
            except subprocess.TimeoutExpired:
                ok = False
            if ok:
                self.acked.append(path)
                self.last_ok = time.time()
            else:
                self.failed += 1
                if os.path.exists(path):
                    os.unlink(path)
                time.sleep(0.05)

    def stop(self):
        self.stop_flag = True
        self.t.join(timeout=60)

    def missing_on(self, port):
        return sum(count_missing(port, p) for p in self.acked), len(self.acked) * 100


class Proxy:
    """Forwards listen_port -> target_port. cut() stops forwarding in both
    directions without closing anything (packets dropped, as in a network
    partition); new connections are accepted and get nothing. heal() closes
    what was frozen and forwards again."""

    def __init__(self, listen_port, target_port):
        self.target = target_port
        self.cut_flag = False
        self.frozen = []
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", listen_port))
        self.sock.listen(256)
        self.lock = threading.Lock()
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            if self.cut_flag:
                with self.lock:
                    self.frozen.append(c)
                continue
            try:
                u = socket.create_connection(("127.0.0.1", self.target), 2)
            except OSError:
                c.close()
                continue
            for a, b in ((c, u), (u, c)):
                threading.Thread(target=self._pump, args=(a, b), daemon=True).start()

    def _pump(self, a, b):
        while True:
            try:
                d = a.recv(65536)
            except OSError:
                d = b""
            if self.cut_flag:
                with self.lock:
                    self.frozen += [a, b]
                return  # dropped; the sockets stay open until heal()
            if not d:
                for s in (a, b):
                    try:
                        s.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                return
            try:
                b.sendall(d)
            except OSError:
                return

    def cut(self):
        self.cut_flag = True

    def heal(self):
        with self.lock:
            for s in self.frozen:
                try:
                    s.close()
                except OSError:
                    pass
            self.frozen = []
        self.cut_flag = False


def sanitizer(build):
    """"thread", "address" or None: the build's -fsanitize, from its CMake cache."""
    try:
        cache = open(os.path.join(build, "CMakeCache.txt")).read()
    except OSError:
        return None
    return "thread" if "fsanitize=thread" in cache else "address" if "fsanitize=address" in cache else None


class Pair:
    """Node 0 (starts as primary) and node 1 (its standby), symmetric and
    rearmed, plus three witnesses. With proxied=True every path to and
    from node 0 goes through a Proxy (T6)."""

    def __init__(self, verbs=("health",) * 3, proxied=False, objects="100000", extra=(), poll_ms=POLL_MS,
                 witness_extra=(), node_bin=None, witness_bins=None, ready=True):
        self.work = tempfile.mkdtemp(prefix="snc-health-")
        b = free_block(100)
        self.b = b
        # node 0: cli b, adm b+40, repl (its standby port) b+60, seed b+70
        # node 1: cli b+10, adm b+50, repl b+20, seed b+80; witnesses b+30..
        self.cli = [b, b + 10]
        self.adm = [b + 40, b + 50]
        self.wit = [b + 30 + i for i in range(3)]
        self.proxies = []
        # What node 0 and the outside use to reach each other.
        to0_adm, to0_cli = self.adm[0], self.cli[0]
        n0_repl_target, n0_wit = b + 20, list(self.wit)
        if proxied:
            # Proxy ports b+90.. : node 0's admin and 2 client ports (in),
            # node 0's replication to node 1 (2 shards) and its witness
            # requests (out).
            px = b + 90
            to0_adm, to0_cli = px, px + 1
            self.proxies += [Proxy(px, self.adm[0]), Proxy(px + 1, self.cli[0]), Proxy(px + 2, self.cli[0] + 1)]
            n0_repl_target = px + 3
            self.proxies += [Proxy(px + 3, b + 20), Proxy(px + 4, b + 21)]
            n0_wit = [px + 5 + i for i in range(3)]
            self.proxies += [Proxy(px + 5 + i, self.wit[i]) for i in range(3)]
        self.to0_adm, self.to0_cli = to0_adm, to0_cli
        common = ["--space-uuid", UUID, "--objects", objects, "--shards", "2", "--debug-admin",
                  "--replica-timeout-ms", "500"] + list(extra)
        self.args = [
            ["--cpus", "0,1", "--port", str(self.cli[0]), "--data-dir", f"{self.work}/n0", "--admin-port",
             str(self.adm[0]), "--replica-of", f"127.0.0.1:{n0_repl_target}", "--witness",
             ",".join(f"127.0.0.1:{p}" for p in n0_wit), "--seed-port", str(b + 70),
             "--rejoin", f"{b + 60},127.0.0.1:{self.adm[1]},127.0.0.1:{b + 80}"] + common,
            ["--cpus", "2,2", "--port", str(self.cli[1]), "--data-dir", f"{self.work}/n1", "--admin-port",
             str(self.adm[1]), "--replica-of", f"127.0.0.1:{b + 60}", "--witness",
             ",".join(f"127.0.0.1:{p}" for p in self.wit), "--seed-port", str(b + 80)] + common,
        ]
        self.nodes = [None, None]
        node_bin = node_bin or f"{BUILD}/snc_server"
        witness_bins = witness_bins or [f"{BUILD}/snc_witness"] * 3
        self.nodes[1] = Proc([node_bin, "--standby", str(b + 20)] + self.args[1])
        assert wait_port(self.adm[1], proc=self.nodes[1]), "node 1 did not start\n" + self.nodes[1].text()
        self.nodes[0] = Proc([node_bin] + self.args[0])
        assert wait_port(self.adm[0], proc=self.nodes[0]), "node 0 did not start\n" + self.nodes[0].text()
        peers = ",".join(f"127.0.0.1:{p}" for p in self.wit)
        self.w = [Proc([witness_bins[i], "--port", str(self.wit[i]), "--primary", f"127.0.0.1:{to0_adm}",
                        "--standby", f"127.0.0.1:{self.adm[1]}", "--state", f"{self.work}/w{i}.state",
                        "--failover-ms", str(FAILOVER_MS), "--poll-ms", str(poll_ms), "--id", str(i),
                        "--peers", peers, "--health-verb", verbs[i]] +
                       (list(witness_extra) if witness_bins[i] == f"{BUILD}/snc_witness" else []))
                  for i in range(3)]
        for w in self.w:
            assert w.wait_for(r"primary up|failover learned", 15), "a witness never saw the primary\n" + w.text()
        # A learned primary is NOT READY until it proves its standby in sync
        # (docs/SYNC_READINESS.md): tests start from a promotable pair.
        if ready:
            assert self.wait_ready(30), f"the pair never became READY: {self.state()}\n" + self.nodes[0].text()
        self.load = None

    def wait_ready(self, timeout, term=None):
        """The witnesses' chosen state once READY (degraded false), at
        `term` if given; None on timeout."""
        t_end = time.time() + timeout
        while time.time() < t_end:
            st = self.state()
            if st and not st[3] and (term is None or st[1] == term):
                return st
            time.sleep(0.05)
        return None

    def state(self):
        """(version, term, primary node, degraded) chosen on a majority."""
        best = None
        for p in self.wit:
            r, _ = ask(p, "READ ha/0", 1.0)
            if r and r.startswith("STATE "):
                v = r.split()
                cand = (int(v[1]), int(v[3]), int(v[5]), v[4] == "1")
                if best is None or cand[0] > best[0]:
                    best = cand
        return best

    def wait_promoted(self, timeout):
        """True once the witnesses chose node 1 as primary (term 1) and it
        answers role p."""
        t_end = time.time() + timeout
        while time.time() < t_end:
            st = self.state()
            if st and st[2] == 1:
                r, _ = ask(self.adm[1], "HEALTH", 0.5)
                if fields(r).get("role") in ("p", "n"):
                    return True
            time.sleep(0.05)
        return False

    def stop(self):
        if self.load:
            self.load.stop()
        for p in self.nodes + self.w:
            if p:
                p.stop()
        for x in self.proxies:
            x.heal()
            x.sock.close()
        shutil.rmtree(self.work, ignore_errors=True)


def role_pairs(pr):
    """[(HEALTH role, STATUS role)] for node 0 and node 1: HEALTH's
    published role must match STATUS's, which asks every shard."""
    out = []
    for port in pr.adm:
        h, _ = ask(port, "HEALTH")
        st, _ = ask(port, "STATUS")
        out.append((fields(h).get("role"), fields(st).get("role")))
    return out


def watch_health(port, seconds, out):
    """Polls HEALTH every 50 ms for `seconds`; appends (t, reply, latency)."""
    t_end = time.time() + seconds
    while time.time() < t_end:
        r, dt = ask(port, "HEALTH", 1.0)
        out.append((time.time(), r, dt))
        time.sleep(0.05)


def stall_scenario(name, verbs, stall_ms, expect_promotion):
    pr = Pair(verbs)
    try:
        pr.load = Load(pr.cli[0], pr.work)
        time.sleep(1.0)
        before = pr.state()
        assert before and before[2] == 0 and not before[3], before
        acked_before = len(pr.load.acked)
        assert acked_before > 0, "no load acknowledged before the stall"
        roles = role_pairs(pr)
        assert roles == [("p", "p"), ("s", "s")], roles
        samples, status = [], {}
        hw = threading.Thread(target=watch_health, args=(pr.adm[0], stall_ms / 1000 + 1.5, samples))
        hw.start()
        r, _ = ask(pr.adm[0], f"DEBUG STALL 0 {stall_ms}")
        assert r == "OK", r
        t_stall = time.time()
        time.sleep(0.3)
        st = threading.Thread(target=lambda: status.update(r=ask(pr.adm[0], "STATUS", 30)))
        st.start()
        if expect_promotion:
            promoted = pr.wait_promoted(stall_ms / 1000 + 5)
        else:
            time.sleep(stall_ms / 1000 + 2.0)
            promoted = pr.state()[2] == 1
        hw.join()
        st.join()
        after = pr.state()
        # Service after the stall: a new batch is acknowledged by whoever
        # serves.
        t_wait = time.time() + 15
        while pr.load.last_ok < t_stall + stall_ms / 1000 and time.time() < t_wait:
            time.sleep(0.1)
        resumed = pr.load.last_ok > t_stall + stall_ms / 1000
        pr.load.stop()
        serving = 1 if promoted else 0
        missing, total = pr.load.missing_on(pr.cli[serving])
        answered = [s for s in samples if s[1] and s[1].startswith("role=")]
        stalls = [int(fields(s[1]).get("stall_ms", 0)) for s in answered]
        mains = [int(fields(s[1]).get("main_ms", 0)) for s in answered]
        lat = [s[2] for s in answered]
        res = dict(name=name, promoted=promoted, before=before, after=after,
                   health_asked=len(samples), health_answered=len(answered),
                   health_max_ms=round(max(lat) * 1000, 1) if lat else None,
                   max_stall_ms=max(stalls) if stalls else None,
                   last_stall_ms=stalls[-1] if stalls else None,
                   max_main_ms=max(mains) if mains else None,
                   status_s=round(status["r"][1], 2), status_reply=(status["r"][0] or "")[:20],
                   resumed=resumed, acked_batches=len(pr.load.acked), failed_batches=pr.load.failed,
                   missing=missing, total=total)
        log(f"  {res}")
        return res, pr, samples
    except BaseException:
        for p in pr.nodes + pr.w:
            if p:
                log(p.text()[-3000:])
        pr.stop()
        raise


def check_no_failover(res, stall_ms):
    assert not res["promoted"], res
    assert res["after"][0] == res["before"][0], ("HA state version changed", res)
    assert not res["after"][3], ("degraded", res)
    assert res["health_answered"] == res["health_asked"], ("HEALTH went silent", res)
    if not SANITIZED:
        assert res["health_max_ms"] < POLL_MS, ("HEALTH slower than poll_ms", res)
    assert res["max_stall_ms"] >= stall_ms - 500, ("stall_ms did not report the stall", res)
    assert res["last_stall_ms"] < 100, ("progress did not resume", res)
    assert res["status_s"] >= (stall_ms - 300) / 1000 - 0.5, ("STATUS was not slow: coupling gone from STATUS?", res)
    assert res["resumed"], ("service did not resume", res)
    assert res["missing"] == 0, ("acknowledged creates lost", res)


def t1():
    res, pr, _ = stall_scenario("T1", ("health",) * 3, 3000, False)
    pr.stop()
    check_no_failover(res, 3000)


def t2():
    res, pr, _ = stall_scenario("T2", ("status",) * 3, 3000, True)
    pr.stop()
    assert res["promoted"], ("STATUS witnesses did not promote: no A/B evidence", res)
    assert res["missing"] == 0, ("acknowledged creates lost", res)


def t3():
    res, pr, samples = stall_scenario("T3", ("health",) * 3, 6000, False)
    pr.stop()
    check_no_failover(res, 6000)
    # stall_ms keeps rising through the stall, then drops to 0.
    seq = [int(fields(s[1]).get("stall_ms", 0)) for s in samples if s[1]]
    first = next(i for i, x in enumerate(seq) if x > 0)
    peak = seq.index(max(seq))
    assert all(b >= a for a, b in zip(seq[first:peak], seq[first + 1:peak + 1])), seq
    assert seq[peak] >= 5500 and seq[-1] < 100, seq


def failover_scenario(name, action):
    pr = Pair(proxied=(action == "partition"))
    try:
        pr.load = Load(pr.to0_cli, pr.work)
        time.sleep(1.0)
        assert pr.load.acked, "no load before the fault"
        t0 = time.time()
        extra = {}
        if action == "kill":
            pr.nodes[0].signal(signal.SIGKILL)
        elif action == "stop":
            pr.nodes[0].signal(signal.SIGSTOP)
        elif action == "partition":
            for x in pr.proxies:
                x.cut()
        promoted = pr.wait_promoted(FAILOVER_MS / 1000 + 8)
        t_prom = time.time() - t0
        pr.load.stop()
        if action == "partition":
            # A client on node 0's side of the cut: nothing acknowledged.
            side = Load(pr.cli[0], pr.work, "side")
            time.sleep(3.0)
            side.stop_flag = True
            extra["side_acked"] = len(side.acked)
            for x in pr.proxies:
                x.heal()
            side.t.join(timeout=60)
            extra["side_acked"] = len(side.acked)
        if action == "stop":
            pr.nodes[0].signal(signal.SIGCONT)
        if action in ("stop", "partition"):
            t_end = time.time() + 15
            role = None
            while time.time() < t_end:
                r, _ = ask(pr.adm[0], "HEALTH", 0.5)
                role = fields(r).get("role")
                if role == "f":
                    break
                time.sleep(0.1)
            extra["old_role"] = role
        missing, total = pr.load.missing_on(pr.cli[1])
        h, _ = ask(pr.adm[1], "HEALTH")
        st, _ = ask(pr.adm[1], "STATUS")
        extra["promoted_roles"] = (fields(h).get("role"), fields(st).get("role"))
        res = dict(name=name, promoted=promoted, promote_s=round(t_prom, 2), missing=missing, total=total, **extra)
        log(f"  {res}")
        pr.stop()
        return res
    except BaseException:
        for p in pr.nodes + pr.w:
            if p:
                log(p.text()[-3000:])
        pr.stop()
        raise


def t4():
    res = failover_scenario("T4", "kill")
    assert res["promoted"] and res["missing"] == 0, res
    # Promoted with --replica-of: a sender exists (its standby is gone).
    assert res["promoted_roles"] == ("p", "p"), res


def t5():
    res = failover_scenario("T5", "stop")
    assert res["promoted"] and res["missing"] == 0 and res["old_role"] == "f", res


def t6():
    res = failover_scenario("T6", "partition")
    assert res["promoted"] and res["missing"] == 0, res
    assert res["side_acked"] == 0, ("the cut-off old primary acknowledged creates", res)
    assert res["old_role"] == "f", res


def t8():
    pr = Pair()
    try:
        pr.load = Load(pr.cli[0], pr.work)
        time.sleep(1.0)
        pr.nodes[0].signal(signal.SIGKILL)
        pr.nodes[0].p.wait(timeout=30)
        assert pr.wait_promoted(10), "node 1 not promoted"
        pr.load.stop()
        r, _ = ask(pr.adm[1], "DEBUG STALL 0 5000")
        assert r == "OK", r
        time.sleep(0.3)
        status = {}
        st = threading.Thread(target=lambda: status.update(r=ask(pr.adm[1], "STATUS", 30)))
        st.start()
        pr.nodes[0] = Proc([f"{BUILD}/snc_server"] + pr.args[0])
        line = pr.nodes[0].wait_for(r"rejoin: ", 20)
        st.join()
        joined = pr.nodes[0].wait_for(r"standby, listening", 30)
        res = dict(name="T8", rejoin_line=line, standby=bool(joined), peer_status_s=round(status["r"][1], 2))
        log(f"  {res}")
        assert line and "starting as its standby" in (pr.nodes[0].wait_for(r"rejoin: starting", 5) or ""), res
        assert joined, res
        # Under the old code, rejoin asked STATUS (5 tries of 500 ms): the
        # peer's STATUS took this long, so it would have read "unreachable".
        assert res["peer_status_s"] > 3.0, res
        missing, _ = pr.load.missing_on(pr.cli[1])
        assert missing == 0, missing
        pr.stop()
    except BaseException:
        for p in pr.nodes + pr.w:
            if p:
                log(p.text()[-3000:])
        pr.stop()
        raise


def t10():
    res, pr, _ = stall_scenario("T10", ("health", "health", "status"), 3000, True)
    pr.stop()
    assert res["promoted"], ("a STATUS witness among HEALTH witnesses did not promote", res)
    assert res["missing"] == 0, res


def t11():
    work = tempfile.mkdtemp(prefix="snc-health-")
    cli, adm, adm2 = free_block(2), free_block(1), free_block(1)
    srv = Proc([f"{BUILD}/snc_server", "--shards", "2", "--port", str(cli), "--data-dir", f"{work}/d",
                "--admin-port", str(adm), "--objects", "400000", "--debug-admin"])
    srv2 = None
    try:
        assert wait_port(adm, proc=srv), srv.text()
        # Enough accounts that AUDIT takes a while.
        r = subprocess.run([f"{BUILD}/net_bench", "--port", str(cli), "--threads", "2", "--objects", "200000",
                            "--seconds", "0.1", "--reads", "100"], capture_output=True, text=True, timeout=300)
        assert r.returncode == 0, r.stdout + r.stderr
        # 300 connections that never send a line.
        idle = []
        for _ in range(300):
            s = socket.socket()
            s.connect(("127.0.0.1", adm))
            idle.append(s)
        # Shard 0 stalled: STATUS and AUDIT block in the worker.
        assert ask(adm, "DEBUG STALL 0 3000")[0] == "OK"
        heavy = []
        lock = threading.Lock()

        def one(verb):
            r = ask(adm, verb, 10)
            with lock:
                heavy.append(r)
        ths = [threading.Thread(target=one, args=("AUDIT" if i == 0 else "STATUS",)) for i in range(24)]
        for t in ths:
            t.start()
            time.sleep(0.005)
        time.sleep(0.3)
        h = [ask(adm, "HEALTH", 1.0) for _ in range(5)]
        for t in ths:
            t.join()
        f = fields(h[-1][0])
        busy = sum(1 for r in heavy if r[0] == "NO busy")
        status_ok = sum(1 for r in heavy if (r[0] or "").startswith("role="))
        audit_ok = sum(1 for r in heavy if (r[0] or "").startswith("AUDIT "))
        res = dict(name="T11", health=[(r[0] or "")[:7] for r in h], health_max_ms=round(max(x[1] for x in h) * 1000, 1),
                   busy=busy, status_ok=status_ok, audit_ok=audit_ok, adm=f)
        log(f"  {res}")
        assert all((r[0] or "").startswith("role=") for r in h), res
        if not SANITIZED:
            assert res["health_max_ms"] < POLL_MS, res
        # 300 idle connections against a bound of 256: the excess was
        # evicted, or (on a slow host) timed out first.
        assert int(f["adm_evicted"]) + int(f["adm_timeouts"]) >= 300 - 256, res
        assert busy >= 24 - 16 - 1 and int(f["adm_busy"]) == busy, res
        assert int(f["adm_q_hw"]) == 16, res
        assert status_ok + audit_ok + busy == 24 and audit_ok == 1, res
        for s in idle:
            s.close()
        # Without --debug-admin, DEBUG STALL is an unknown verb.
        srv2 = Proc([f"{BUILD}/snc_server", "--shards", "1", "--port", str(free_block(1)), "--admin-port", str(adm2)])
        assert wait_port(adm2, proc=srv2), srv2.text()
        r, _ = ask(adm2, "DEBUG STALL 0 1000")
        assert r == "NO unknown request", r
        assert int(fields(ask(adm2, "HEALTH")[0]).get("stall_ms", 1)) < 100
    finally:
        srv.stop()
        if srv2:
            srv2.stop()
        shutil.rmtree(work, ignore_errors=True)


class FakeNode:
    """An admin port. knows_health: answers HEALTH (else "NO unknown
    request", as a node built before HEALTH). health_ok / status_ok: False
    makes that verb silent (a stuck shard makes only STATUS silent)."""

    def __init__(self, role, uuid=UUID, term=0, knows_health=True):
        self.role, self.uuid, self.term = role, uuid, term
        self.knows_health, self.health_ok, self.status_ok = knows_health, True, True
        self.promotes = 0
        self.verbs = []
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(64)
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._one, args=(c,), daemon=True).start()

    def _one(self, c):
        with c:
            c.settimeout(3)
            try:
                line = c.makefile().readline().strip()
            except OSError:
                return
            self.verbs.append((time.time(), line))
            base = f"role={self.role} term={self.term} uuid={self.uuid}"
            if line == "HEALTH" and self.knows_health:
                if not self.health_ok:
                    time.sleep(2)
                    return
                reply = base + " epoch=0 leases=0-0 shards=2 stall_ms=0 stall_shard=0 main_ms=0"
            elif line == "STATUS":
                if not self.status_ok:
                    time.sleep(2)
                    return
                reply = base + " shards=2 live=2 degraded=0 blocked=0"
            elif line == "PROMOTE":
                self.promotes += 1
                if self.role == "s":
                    self.role, self.term = "p", self.term + 1
                reply = "OK"
            else:
                reply = "NO unknown request"
            try:
                c.sendall((reply + "\n").encode())
            except OSError:
                pass


def fake_witness(work, prim, sb, verb="health"):
    port = free_block(1)
    w = Proc([f"{BUILD}/snc_witness", "--port", str(port), "--primary", f"127.0.0.1:{prim.port}",
              "--standby", f"127.0.0.1:{sb.port}", "--state", f"{work}/w.{port}.state",
              "--failover-ms", "300", "--poll-ms", "30", "--health-verb", verb])
    assert w.wait_for(r"primary up", 10), w.text()
    # A learned primary is NOT READY; a real one says SYNCED once its
    # standby is proven (docs/SYNC_READINESS.md). The fakes start READY.
    r = ask(port, f"NOTREADY {UUID} 0", 3)[0]
    assert r and r.startswith("OK v="), r
    assert ask(port, f"READY {UUID} 0 after={r[5:]}", 3)[0].startswith("OK v="), r
    return w, port


def t7():
    """Eligibility is not widened by HEALTH (fake nodes, one witness)."""
    work = tempfile.mkdtemp(prefix="snc-health-")
    procs = []
    try:
        res = {}
        # a. The standby answers HEALTH but its STATUS is silent (a stuck
        #    shard): not promoted; once STATUS answers, promoted.
        prim, sb = FakeNode("p"), FakeNode("s")
        w, _ = fake_witness(work, prim, sb)
        procs.append(w)
        sb.status_ok = False
        prim.health_ok = prim.status_ok = False
        time.sleep(2.0)
        res["stuck_standby_promotes"] = sb.promotes
        sb.status_ok = True
        t_end = time.time() + 5
        while sb.promotes == 0 and time.time() < t_end:
            time.sleep(0.02)
        res["after_status_answers"] = sb.promotes
        # b. Wrong term, c. wrong space: never promoted.
        for case, kw in (("wrong_term", dict(term=5)), ("wrong_space", dict(uuid="d4" * 16))):
            prim, sb = FakeNode("p"), FakeNode("s", **kw)
            w, _ = fake_witness(work, prim, sb)
            procs.append(w)
            prim.health_ok = prim.status_ok = False
            time.sleep(1.5)
            res[case] = sb.promotes
        # d. Degraded on record (a grant to run alone): never promoted.
        prim, sb = FakeNode("p"), FakeNode("s")
        w, wport = fake_witness(work, prim, sb)
        procs.append(w)
        res["degrade"] = ask(wport, f"DEGRADE {UUID} 0", 3)[0]
        prim.health_ok = prim.status_ok = False
        time.sleep(1.5)
        res["degraded_promotes"] = sb.promotes
        log(f"  {res}")
        assert res["stuck_standby_promotes"] == 0 and res["after_status_answers"] >= 1, res
        assert res["wrong_term"] == 0 and res["wrong_space"] == 0, res
        assert res["degrade"] == "OK" and res["degraded_promotes"] == 0, res
    finally:
        for p in procs:
            p.stop()
        shutil.rmtree(work, ignore_errors=True)


def t10b():
    """Fallback: a node without HEALTH is polled with STATUS (and its
    silence still promotes); an upgrade is picked up within 10 s."""
    work = tempfile.mkdtemp(prefix="snc-health-")
    procs = []
    try:
        res = {}
        prim, sb = FakeNode("p", knows_health=False), FakeNode("s", knows_health=False)
        w, _ = fake_witness(work, prim, sb)
        procs.append(w)
        res["fallback_logged"] = bool(w.wait_for(r"node 0 does not know HEALTH", 5))
        time.sleep(0.5)
        prim.knows_health = True
        t_up = time.time()
        res["upgrade_logged"] = bool(w.wait_for(r"node 0 answers HEALTH", 12))
        res["upgrade_pickup_s"] = round(time.time() - t_up, 1)
        time.sleep(0.5)
        recent = [v for t, v in prim.verbs if t > time.time() - 0.4]
        res["recent_verbs"] = sorted(set(recent))
        # An old primary that goes silent is still failed over.
        prim2, sb2 = FakeNode("p", knows_health=False), FakeNode("s", knows_health=False)
        w2, _ = fake_witness(work, prim2, sb2)
        procs.append(w2)
        prim2.status_ok = False
        t_end = time.time() + 5
        while sb2.promotes == 0 and time.time() < t_end:
            time.sleep(0.02)
        res["old_silent_promotes"] = sb2.promotes
        log(f"  {res}")
        assert res["fallback_logged"] and res["upgrade_logged"] and res["upgrade_pickup_s"] <= 11, res
        assert res["recent_verbs"] == ["HEALTH"], res
        assert res["old_silent_promotes"] >= 1, res
    finally:
        for p in procs:
            p.stop()
        shutil.rmtree(work, ignore_errors=True)


TESTS = dict(t1=t1, t2=t2, t3=t3, t4=t4, t5=t5, t6=t6, t7=t7, t8=t8, t10=t10, t10b=t10b, t11=t11)


def main():
    names = ONLY or list(TESTS)
    for n in names:
        t0 = time.time()
        log(f"{n.upper()} ...")
        TESTS[n]()
        log(f"{n.upper()} ok ({time.time() - t0:.1f}s)")
    print(f"test_health: {len(names)} scenarios ok", flush=True)


if __name__ == "__main__":
    main()
