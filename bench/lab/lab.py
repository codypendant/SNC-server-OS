#!/usr/bin/env python3
"""The real-machine lab (docs/REAL_MACHINES.md).

Runs SNC's replication, failover, cluster and migration behaviour across
the hosts of an inventory (bench/lab/inventory.example.toml), from one of
them, over SSH. The same scenarios run against loopback with
bench/lab/loopback.toml, which needs no SSH and gives the reference numbers.

    lab.py INVENTORY deploy            copy the binaries to every host
    lab.py INVENTORY p0                network and disk baseline
    lab.py INVENTORY p1                one node, clients on another host
    lab.py INVENTORY p2                sync replication across hosts
    lab.py INVENTORY p3 [--fault F]    failover of group 0's primary
    lab.py INVENTORY p4 [--fault F]    cross-group transfers, a link cut
    lab.py INVENTORY p5 [--crash WHO --at STEP]   a slot move under load
    lab.py INVENTORY soak [--minutes M]

Faults: kill, stop (SIGSTOP), and with root on real hosts: partition (both
directions between two hosts), oneway, link (data NIC down), delay (netem).
Each run writes bench/results/lab-DATE/SCENARIO-TIME/: logs, every node's
STATUS over time, the load's per-second lines, and result.json.

Python 3.11+ (tomllib); the hosts need bash, and for the fault and P0 tools
iproute2, iptables, iperf3 and fio where those phases use them.
"""
import argparse
import json
import os
import random
import shlex
import shutil
import socket
import subprocess
import sys
import threading
import time
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BINARIES = ["snc_server", "snc_witness", "snc_ctl", "snc_load", "net_bench"]
WITNESS_STRIDE, NODE_BASE, NODE_STRIDE = 1, 100, 40


# ---- Hosts -------------------------------------------------------------------------------

class Host:
    """A machine: commands run through SSH, or bash locally when ssh is
    "local". Every role's files live under `dir` on it."""

    def __init__(self, name, spec, lab):
        self.name = name
        self.addr = spec["addr"]
        self.ssh = spec.get("ssh", "local")
        self.iface = spec.get("iface")
        self.cpus = spec.get("cpus")
        self.local = self.ssh == "local"
        self.dir = lab["dir"]
        self.bin = lab["build"] if self.local else f"{self.dir}/bin"

    def argv(self, cmd):
        return ["bash", "-c", cmd] if self.local else ["ssh", "-o", "BatchMode=yes", self.ssh, cmd]

    def sh(self, cmd, check=True, timeout=600):
        r = subprocess.run(self.argv(cmd), capture_output=True, text=True, timeout=timeout)
        if check and r.returncode != 0:
            raise RuntimeError(f"{self.name}: {cmd}: exit {r.returncode}: {r.stderr.strip()[-500:]}")
        return r

    def spawn(self, log, cmd):
        """Starts cmd in the background on this host; returns its pid."""
        line = f"mkdir -p {shlex.quote(os.path.dirname(log))}; " \
               f"nohup setsid bash -c {shlex.quote('exec ' + cmd)} > {shlex.quote(log)} 2>&1 < /dev/null & echo $!"
        return int(self.sh(line).stdout.strip())

    def signal(self, pid, sig):
        self.sh(f"kill -{sig} {pid}", check=False)

    def alive(self, pid):
        return self.sh(f"kill -0 {pid}", check=False).returncode == 0

    def fetch(self, path, to):
        os.makedirs(os.path.dirname(to), exist_ok=True)
        if self.local:
            if os.path.exists(path):
                shutil.copy(path, to)
        else:
            subprocess.run(["scp", "-q", "-o", "BatchMode=yes", f"{self.ssh}:{path}", to], check=False)


def ask(addr, port, line, timeout=3.0):
    try:
        with socket.create_connection((addr, port), timeout) as s:
            s.settimeout(timeout)
            s.sendall((line + "\n").encode())
            return s.makefile().readline().strip()
    except OSError:
        return None


def status(addr, port):
    r = ask(addr, port, "STATUS", 1)
    return dict(kv.split("=", 1) for kv in r.split() if "=" in kv) if r else None


def wait(cond, what, timeout=30, every=0.05):
    end = time.time() + timeout
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(every)
    raise RuntimeError(f"timed out: {what}")


# ---- The lab -----------------------------------------------------------------------------

class Lab:
    def __init__(self, path, scenario):
        with open(path, "rb") as f:
            inv = tomllib.load(f)
        self.cfg = inv["lab"]
        self.cfg.setdefault("dir", "/tmp/snc-lab")
        self.cfg["build"] = os.path.abspath(os.path.join(REPO, self.cfg.get("build", "build")))
        self.hosts = {n: Host(n, s, self.cfg) for n, s in inv["hosts"].items()}
        lay = inv["layout"]
        self.witness_hosts = [self.hosts[h] for h in lay["witnesses"]]
        self.groups = [[self.hosts[h] for h in g] for g in lay["groups"]]
        self.client = self.hosts[lay["client"]]
        self.shards = int(self.cfg.get("shards", 2))
        self.base = int(self.cfg.get("base_port", 21000))
        stamp = time.strftime("%Y%m%d")
        self.out = os.path.join(REPO, "bench", "results", f"lab-{stamp}", f"{scenario}-{time.strftime('%H%M%S')}")
        os.makedirs(self.out, exist_ok=True)
        self.run_dir = f"{self.cfg['dir']}/run-{os.path.basename(self.out)}"
        self.procs = []  # (host, pid, name)
        self.nodes = {}  # node id -> dict(host, group, member, pid, admin, ...)
        self.faults = []  # undo commands (host, cmd)
        self.events = []
        self.t0 = time.time()

    def note(self, event, **kv):
        e = {"t": round(time.time() - self.t0, 3), "event": event, **kv}
        self.events.append(e)
        print(f"[{e['t']:8.2f}] {event} {kv if kv else ''}", flush=True)

    def bin(self, host, name):
        return f"{host.bin}/{name}"

    def spawn(self, host, name, cmd):
        pid = host.spawn(f"{self.run_dir}/{name}.log", cmd)
        self.procs.append((host, pid, name))
        return pid

    # Ports: witness w at base+w; node k (1-based) from base+100+40k: admin
    # +0, node links +10.., clients +20.., replication +30.
    def witness_port(self, w):
        return self.base + w * WITNESS_STRIDE

    def node_ports(self, k):
        b = self.base + NODE_BASE + NODE_STRIDE * k
        return {"admin": b, "link": b + 10, "client": b + 20, "repl": b + 30}

    def control(self):
        return ",".join(f"{h.addr}:{self.witness_port(i)}" for i, h in enumerate(self.witness_hosts))

    def save(self, result):
        result["events"] = self.events
        with open(os.path.join(self.out, "result.json"), "w") as f:
            json.dump(result, f, indent=2)
        print(f"results in {self.out}")

    # ---- Cluster up and down ----

    def up(self, failover_ms=1000, poll_ms=100, replica_timeout_ms=2000, objects=None):
        """Witnesses, the placement, then every group's standby and primary."""
        objects = objects or int(self.cfg.get("objects", 1000000))
        control = self.control()
        k = 0
        for g, members in enumerate(self.groups):
            for m, h in enumerate(members):
                k += 1
                self.nodes[k] = {"host": h, "group": g, "member": m, **self.node_ports(k)}
        groups_arg = ",".join(
            f"{self.shards}:" + "+".join(f"{k}/{n['link']}/{n['client']}" for k, n in self.nodes.items()
                                         if n["group"] == g)
            for g in range(len(self.groups)))
        nodes_arg = ",".join(f"{k}@{n['host'].addr}:{n['admin']}" for k, n in self.nodes.items())
        for i, h in enumerate(self.witness_hosts):
            args = [self.bin(h, "snc_witness"), "--port", self.witness_port(i), "--state",
                    f"{self.run_dir}/w{i}.state", "--id", i, "--peers", control,
                    "--failover-ms", failover_ms, "--poll-ms", poll_ms]
            for g in range(len(self.groups)):
                ms = [n for n in self.nodes.values() if n["group"] == g]
                args += ["--group", f"{g}=" + ",".join(f"{n['host'].addr}:{n['admin']}" for n in ms)]
            h.sh(f"mkdir -p {self.run_dir}")
            self.spawn(h, f"w{i}", " ".join(map(str, args)))
        time.sleep(0.5)
        drv = self.client
        r = drv.sh(f"{self.bin(drv, 'snc_ctl')} --control {control} init --nodes {nodes_arg} --groups {groups_arg}",
                   check=False)
        if r.returncode != 0:
            raise RuntimeError("snc_ctl init: " + r.stdout + r.stderr)
        for k, n in sorted(self.nodes.items(), key=lambda kv: -kv[1]["member"]):
            self.start_node(k, objects=objects, replica_timeout_ms=replica_timeout_ms)
            if n["member"] > 0:
                wait(lambda: status(n["host"].addr, n["admin"]), f"node {k} up")
        for k, n in self.nodes.items():
            st = wait(lambda: status(n["host"].addr, n["admin"]), f"node {k} up", timeout=120)
            n["role"] = st["role"]
        self.note("cluster up", groups=len(self.groups), nodes=len(self.nodes))

    def start_node(self, k, objects, replica_timeout_ms=2000, rejoin=False):
        n = self.nodes[k]
        h = n["host"]
        g = n["group"]
        members = [(kk, nn) for kk, nn in self.nodes.items() if nn["group"] == g]
        args = [self.bin(h, "snc_server"), "--shards", self.shards, "--objects", objects,
                "--data-dir", f"{self.run_dir}/n{k}", "--control", self.control(), "--node-id", k,
                "--admin-port", n["admin"], "--witness", self.control()]
        if h.cpus:
            args += ["--cpus", h.cpus]
        if n["member"] > 0:
            args += ["--standby", n["repl"]]
        elif len(members) > 1:
            sk, sn = members[1]
            args += ["--replica-of", f"{sn['host'].addr}:{sn['repl']}", "--replica-timeout-ms", replica_timeout_ms]
        n["pid"] = self.spawn(h, f"n{k}" + (f"-r{int(time.time())}" if rejoin else ""), " ".join(map(str, args)))

    def primary(self, g):
        for k, n in self.nodes.items():
            if n["group"] != g:
                continue
            st = status(n["host"].addr, n["admin"])
            if st and st["role"] in ("p", "n"):
                return k
        return None

    def connect_arg(self):
        k = self.primary(0)
        n = self.nodes[k]
        return f"{n['host'].addr}:{n['client']}"

    def down(self):
        self.heal()
        for h, pid, name in reversed(self.procs):
            h.signal(pid, "KILL")
        self.procs.clear()
        for k, n in self.nodes.items():
            n["host"].fetch(f"{self.run_dir}/n{k}.log", os.path.join(self.out, f"n{k}.log"))
        for i, h in enumerate(self.witness_hosts):
            h.fetch(f"{self.run_dir}/w{i}.log", os.path.join(self.out, f"w{i}.log"))
        if not self.cfg.get("keep"):
            for h in {id(h): h for h in self.hosts.values()}.values():
                h.sh(f"rm -rf {self.run_dir}", check=False)

    # ---- Faults ----

    def fault(self, kind, k=None, peer=None, ms=None):
        """Applies a fault to node k (or between node k's host and `peer`)."""
        n = self.nodes[k]
        h = n["host"]
        self.note("fault", kind=kind, node=k, peer=peer.name if peer else None)
        if kind == "kill":
            h.signal(n["pid"], "KILL")
        elif kind == "stop":
            h.signal(n["pid"], "STOP")
            self.faults.append((h, f"kill -CONT {n['pid']}"))
        elif kind in ("partition", "oneway"):
            self.need_root(h)
            h.sh(f"iptables -I INPUT -s {peer.addr} -j DROP")
            self.faults.append((h, f"iptables -D INPUT -s {peer.addr} -j DROP"))
            if kind == "partition":
                h.sh(f"iptables -I OUTPUT -d {peer.addr} -j DROP")
                self.faults.append((h, f"iptables -D OUTPUT -d {peer.addr} -j DROP"))
        elif kind == "link":
            self.need_root(h)
            h.sh(f"ip link set {h.iface} down")
            self.faults.append((h, f"ip link set {h.iface} up"))
        elif kind == "delay":
            self.need_root(h)
            h.sh(f"tc qdisc add dev {h.iface} root netem delay {ms or 5}ms")
            self.faults.append((h, f"tc qdisc del dev {h.iface} root"))
        else:
            raise ValueError(f"unknown fault {kind}")

    def need_root(self, h):
        if h.local:
            raise RuntimeError("network faults need real hosts (root, a data NIC); loopback supports kill and stop")

    def heal(self):
        while self.faults:
            h, cmd = self.faults.pop()
            h.sh(cmd, check=False)
            self.note("healed", host=h.name, cmd=cmd)

    # ---- Load and checks ----

    def create(self, accounts, in_slot=0):
        c = self.client
        ids = f"{self.run_dir}/ids"
        r = c.sh(f"{self.bin(c, 'snc_load')} create --connect {self.connect_arg()} --accounts {accounts} "
                 f"--in-slot {in_slot} --ids {ids}", timeout=1800)
        self.note("created", out=r.stdout.strip())
        self.ids = ids
        if in_slot:
            first = c.sh(f"head -1 {ids}.hot").stdout.strip()
            return first
        return None

    def load(self, name, seconds, threads=2, depth=16, hot_pct=0):
        c = self.client
        cmd = (f"{self.bin(c, 'snc_load')} run --connect {self.connect_arg()} --ids {self.ids} --seconds {seconds} "
               f"--threads {threads} --depth {depth} --hot-pct {hot_pct} --report-ms 500 "
               f"--out {self.run_dir}/{name}")
        pid = self.spawn(c, name, cmd)
        self.note("load started", name=name, seconds=seconds)
        return pid

    def wait_load(self, name, pid, timeout):
        wait(lambda: not self.client.alive(pid), f"load {name} finished", timeout=timeout, every=0.5)
        self.client.fetch(f"{self.run_dir}/{name}.log", os.path.join(self.out, f"{name}.log"))
        self.client.fetch(f"{self.run_dir}/{name}.summary", os.path.join(self.out, f"{name}.summary"))
        summary = {}
        try:
            for line in open(os.path.join(self.out, f"{name}.summary")):
                k, _, v = line.strip().partition(" ")
                summary[k] = v
        except OSError:
            summary["error"] = "no summary"
        self.note("load finished", name=name, **{k: summary.get(k) for k in ("ops_s", "p99_us", "error")})
        return summary

    def audit(self):
        """AUDIT on every serving primary."""
        out = {}
        for k, n in self.nodes.items():
            r = ask(n["host"].addr, n["admin"], "AUDIT", 30)
            if r and r.startswith("AUDIT"):
                a, total, pending, epoch, held, mis = map(int, r.split()[1:])
                out[k] = {"accounts": a, "total": total, "pending": pending, "epoch": epoch, "held": held,
                          "misrouted": mis}
        return out

    def settle(self, timeout=300):
        t = time.time()
        a = wait(lambda: (lambda a: a if a and all(v["pending"] == 0 for v in a.values()) else None)(self.audit()),
                 "every transfer settled", timeout=timeout, every=0.2)
        self.note("settled", seconds=round(time.time() - t, 2))
        return a

    def check(self, runs):
        """Balances from every node that serves accounts, against the runs."""
        files = []
        for k, n in self.nodes.items():
            path = f"{self.run_dir}/bal.{k}"
            r = ask(n["host"].addr, n["admin"], f"AUDIT {path}", 60)
            if r and r.startswith("AUDIT") and int(r.split()[1]) > 0:
                local = os.path.join(self.out, f"bal.{k}")
                n["host"].fetch(path, local)
                files.append((n["host"], path, local))
        c = self.client
        # The checker runs on the client host, where the run files are.
        remote = []
        for h, path, local in files:
            if c.local and h.local:
                remote.append(path)
            else:
                dst = f"{self.run_dir}/bal.check.{len(remote)}"
                if not c.local:
                    subprocess.run(["scp", "-q", local, f"{c.ssh}:{dst}"], check=True)
                else:
                    shutil.copy(local, dst)
                remote.append(dst)
        r = c.sh(f"{self.bin(c, 'snc_load')} check --ids {self.ids} --balances {','.join(remote)} "
                 f"--runs {','.join(f'{self.run_dir}/{x}' for x in runs)}", check=False)
        ok = r.returncode == 0
        self.note("check", ok=ok, out=r.stdout.strip())
        return ok, r.stdout.strip()

    def watch(self, every=0.05):
        """Polls every node's STATUS in the background; returns a stopper
        and the samples [(t, k, role, term, epoch)]."""
        samples, stop = [], threading.Event()

        def loop():
            while not stop.is_set():
                t = time.time() - self.t0
                for k, n in self.nodes.items():
                    st = status(n["host"].addr, n["admin"])
                    samples.append((round(t, 3), k, st["role"] if st else "-", st["term"] if st else "-",
                                    st["epoch"] if st else "-"))
                stop.wait(every)

        th = threading.Thread(target=loop, daemon=True)
        th.start()
        return (lambda: (stop.set(), th.join())), samples


# ---- Scenarios ---------------------------------------------------------------------------

def p0(lab, args):
    """RTT, bandwidth and fsync latency."""
    res = {"rtt": {}, "iperf": {}, "fsync": {}}
    hosts = list({h.addr: h for h in lab.hosts.values()}.values())
    for a in hosts:
        for b in hosts:
            if a is b:
                continue
            r = a.sh(f"ping -q -c 200 -i 0.01 {b.addr} 2>/dev/null || ping -q -c 50 -i 0.2 {b.addr}", check=False)
            res["rtt"][f"{a.name}->{b.name}"] = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr
    if len(hosts) > 1 and hosts[0].sh("command -v iperf3", check=False).returncode == 0:
        srv = hosts[0]
        pid = srv.spawn(f"{lab.run_dir}/iperf3.log", "iperf3 -s -1")
        time.sleep(0.5)
        r = hosts[1].sh(f"iperf3 -c {srv.addr} -t 5 -J", check=False)
        try:
            res["iperf"][f"{hosts[1].name}->{srv.name}"] = json.loads(r.stdout)["end"]["sum_received"]["bits_per_second"]
        except (ValueError, KeyError):
            res["iperf"]["error"] = r.stderr.strip()[-300:]
        srv.signal(pid, "KILL")
    for h in hosts:
        if h.sh("command -v fio", check=False).returncode != 0:
            res["fsync"][h.name] = "fio missing"
            continue
        r = h.sh(f"mkdir -p {lab.run_dir} && fio --name=fsync --directory={lab.run_dir} --rw=write --bs=4k "
                 f"--size=64m --fsync=1 --runtime=10 --time_based --output-format=json", check=False)
        try:
            j = json.loads(r.stdout)["jobs"][0]
            res["fsync"][h.name] = {"iops": j["write"]["iops"],
                                    "fsync_p50_us": j["sync"]["lat_ns"]["percentile"]["50.000000"] / 1000,
                                    "fsync_p99_us": j["sync"]["lat_ns"]["percentile"]["99.000000"] / 1000}
        except (ValueError, KeyError):
            res["fsync"][h.name] = r.stderr.strip()[-300:]
    return res


def net_bench_once(lab, srv_host, port, extra, seconds, depth, threads=2):
    c = lab.client
    r = c.sh(f"{lab.bin(c, 'net_bench')} --host {srv_host.addr} --port {port} --threads {threads} "
             f"--objects {lab.cfg.get('bench_objects', 100000)} --seconds {seconds} --depth {depth} {extra}",
             check=False, timeout=600)
    out = r.stdout + r.stderr
    ops = None
    for line in out.splitlines():
        if "ops/s" in line:
            ops = line.strip()
    return ops or out.strip()[-300:]


def p1_p2(lab, args, replicate):
    """One node (P1) or a sync pair (P2), net_bench from the client host."""
    prim, stby = lab.groups[0][0], (lab.groups[0][1] if len(lab.groups[0]) > 1 else None)
    if replicate and stby is None:
        raise RuntimeError("P2 needs group 0 to have a standby")
    port, rport = lab.base + 300, lab.base + 340
    res = {}
    for durable in (False, True):
        pids = []
        if replicate:
            pids.append((stby, stby.spawn(f"{lab.run_dir}/p2-standby.log",
                                          f"{lab.bin(stby, 'snc_server')} --shards {lab.shards} --objects 1000000 "
                                          f"--data-dir {lab.run_dir}/p2s --standby {rport}")))
            time.sleep(1)
        cmd = f"{lab.bin(prim, 'snc_server')} --shards {lab.shards} --objects 1000000 --host 0.0.0.0 --port {port}"
        if prim.cpus:
            cmd += f" --cpus {prim.cpus}"
        if durable or replicate:
            cmd += f" --data-dir {lab.run_dir}/p1-{int(durable)}"
        if replicate:
            cmd += f" --replica-of {stby.addr}:{rport}" + ("" if durable else " --async-replica")
        pids.append((prim, prim.spawn(f"{lab.run_dir}/p1.log", cmd)))
        time.sleep(2)
        mode = ("durable" if durable else "memory") + ("+sync" if replicate and durable else "")
        for depth in args.depths:
            res[f"{mode} d{depth}"] = net_bench_once(lab, prim, port, "--reads 50", args.seconds, depth)
            lab.note("net_bench", mode=mode, depth=depth, result=res[f"{mode} d{depth}"])
        for h, pid in pids:
            h.signal(pid, "KILL")
        time.sleep(0.5)
        for h in (prim, stby):
            if h:
                h.sh(f"rm -rf {lab.run_dir}/p1-* {lab.run_dir}/p2s", check=False)
    return res


def p3(lab, args):
    """Group 0's primary fails under load; its standby is promoted."""
    lab.up(failover_ms=args.failover_ms)
    lab.create(args.accounts)
    stop, samples = lab.watch()
    pid = lab.load("load", args.seconds, threads=args.threads, depth=args.depth)
    time.sleep(args.seconds / 3)
    k = lab.primary(0)
    sk = next(kk for kk, n in lab.nodes.items() if n["group"] == 0 and kk != k)
    t_fault = time.time()
    lab.fault(args.fault, k, peer=lab.nodes[sk]["host"] if args.fault in ("partition", "oneway") else None)
    st = wait(lambda: (lambda s: s if s and s["role"] in ("p", "n") else None)(
        status(lab.nodes[sk]["host"].addr, lab.nodes[sk]["admin"])), "standby promoted", timeout=60, every=0.02)
    promote_s = time.time() - t_fault
    lab.note("promoted", node=sk, seconds=round(promote_s, 3), term=st["term"])
    withdraw_s = None
    if args.fault in ("stop", "partition", "oneway", "link"):
        time.sleep(1)
        lab.heal()
        t = time.time()
        try:
            wait(lambda: (lambda s: s and s["role"] == "f")(status(lab.nodes[k]["host"].addr, lab.nodes[k]["admin"])),
                 "old primary withdrew", timeout=30, every=0.02)
            withdraw_s = time.time() - t
            lab.note("old primary withdrew", seconds=round(withdraw_s, 3))
        except RuntimeError as e:
            lab.note("old primary did not withdraw", why=str(e))
    summary = lab.wait_load("load", pid, args.seconds + 120)
    stop()
    lab.settle()
    ok, out = lab.check(["load"])
    # Client-visible gap: the longest interval with no acknowledgement.
    gap = max_gap(os.path.join(lab.out, "load.log"))
    return {"ok": ok, "check": out, "promote_s": promote_s, "withdraw_s": withdraw_s, "client_gap_s": gap,
            "load": summary, "status_samples": len(samples)}


def max_gap(log):
    """Longest run of report intervals with zero ops (in seconds)."""
    try:
        rows = [l.split() for l in open(log) if l[0].isdigit()]
    except OSError:
        return None
    gap = best = 0.0
    prev = 0.0
    for r in rows:
        t, ops = float(r[0]), float(r[1])
        gap = gap + (t - prev) if ops == 0 else 0.0
        best = max(best, gap)
        prev = t
    return best


def p4(lab, args):
    """Cross-group transfers; a link between two groups' primaries is cut."""
    if len(lab.groups) < 2:
        raise RuntimeError("P4 needs two groups")
    lab.up()
    lab.create(args.accounts)
    pid = lab.load("load", args.seconds, threads=args.threads, depth=args.depth)
    time.sleep(args.seconds / 3)
    k0, k1 = lab.primary(0), lab.primary(1)
    lab.fault(args.fault, k0, peer=lab.nodes[k1]["host"], ms=args.delay_ms)
    backlog = []
    t = time.time()
    while time.time() - t < args.cut_s:
        a = lab.audit()
        backlog.append((round(time.time() - lab.t0, 2), sum(v["pending"] for v in a.values())))
        time.sleep(0.5)
    lab.heal()
    t_heal = time.time()
    summary = lab.wait_load("load", pid, args.seconds + 300)
    lab.settle(timeout=1800)
    settle_s = time.time() - t_heal
    ok, out = lab.check(["load"])
    return {"ok": ok, "check": out, "backlog": backlog, "settle_after_heal_s": settle_s, "load": summary}


def p5(lab, args):
    """A slot move under load, optionally with a crash at a step."""
    if len(lab.groups) < 2:
        raise RuntimeError("P5 needs two groups")
    lab.up()
    lab.create(args.accounts, in_slot=args.slot_size)
    slot = created_slot(lab)
    home = slot_group(lab, slot)
    dest = 1 - home if home < 2 else 0
    pid = lab.load("load", args.seconds, threads=args.threads, depth=args.depth, hot_pct=50)
    probe = Probe(lab, slot)
    probe.start()
    time.sleep(args.seconds / 4)
    c = lab.client
    ctl = f"{lab.bin(c, 'snc_ctl')} --control {lab.control()} move"
    chunk = f"--chunk {args.chunk}"
    steps = {"from": home, "to": dest}
    t = time.time()
    if args.crash:
        r = c.sh(f"{ctl} --stop-after {args.at} {chunk} {slot} {dest}", check=False)
        lab.note("driver stopped", at=args.at, out=r.stdout.strip())
        if args.crash in ("source", "dest"):
            k = lab.primary(home if args.crash == "source" else dest)
            lab.fault("kill", k)
            sk = next(kk for kk, n in lab.nodes.items() if n["group"] == lab.nodes[k]["group"] and kk != k)
            wait(lambda: (lambda s: s and s["role"] in ("p", "n"))(status(lab.nodes[sk]["host"].addr, lab.nodes[sk]["admin"])),
                 "standby promoted", timeout=60)
            lab.note("promoted", node=sk)
    r = c.sh(f"{ctl} {chunk} {slot} {dest}", check=False, timeout=900)
    steps["move_s"] = round(time.time() - t, 3)
    steps["move"] = r.stdout.strip()
    steps["driver_log"] = r.stderr.strip().splitlines()[-20:]
    lab.note("moved", slot=slot, result=r.stdout.strip(), seconds=steps["move_s"])
    time.sleep(args.seconds / 4)
    t = time.time()
    r = c.sh(f"{ctl} {chunk} {slot} {home}", check=False, timeout=900)
    steps["back_s"] = round(time.time() - t, 3)
    steps["back"] = r.stdout.strip()
    steps["back_driver_log"] = r.stderr.strip().splitlines()[-20:]
    lab.note("moved back", result=r.stdout.strip(), seconds=steps["back_s"])
    summary = lab.wait_load("load", pid, args.seconds + 300)
    probe.stop()
    audit = lab.settle()
    ok, out = lab.check(["load"])
    epochs = sorted({v["epoch"] for v in audit.values()})
    ok = ok and probe.violations == 0 and steps["move"] == "DONE" and steps["back"] == "DONE" and epochs == [3]
    steps["node_epochs"] = epochs
    return {"ok": ok, "check": out, "slot": slot, "slot_size": args.slot_size, **steps,
            "probe_rounds": probe.rounds, "two_serving": probe.violations, "migrating_window_s": probe.window(),
            "load": summary}


def slot_group(lab, slot):
    """The group the committed map gives `slot` (MOVE INFO's map_group)."""
    for n in lab.nodes.values():
        r = ask(n["host"].addr, n["admin"], f"MOVE INFO {slot}", 5)
        if r and r.startswith("INFO"):
            return int(r.split()[9])
    raise RuntimeError("no node answered MOVE INFO")


def created_slot(lab):
    """The slot snc_load create filled (it prints it)."""
    for e in lab.events:
        if e["event"] == "created" and " in slot " in e.get("out", ""):
            return int(e["out"].rsplit(" in slot ", 1)[1])
    raise RuntimeError("no slot from create")


class Probe:
    """Never two serving groups for the slot: MOVE INFO on every node."""

    def __init__(self, lab, slot):
        self.lab, self.slot = lab, slot
        self.rounds = self.violations = 0
        self.unserved = []  # times with no serving group
        self._stop = threading.Event()

    def start(self):
        self.th = threading.Thread(target=self.loop, daemon=True)
        self.th.start()

    def loop(self):
        while not self._stop.is_set():
            serving = []
            for k, n in self.lab.nodes.items():
                r = ask(n["host"].addr, n["admin"], f"MOVE INFO {self.slot}", 2)
                if r and r.startswith("INFO"):
                    f = r.split()
                    if f[8] == "1":  # report: phase group move epoch count digest durable serving ...
                        serving.append(k)
            self.rounds += 1
            if len({self.lab.nodes[k]["group"] for k in serving}) > 1:
                self.violations += 1
                self.lab.note("TWO SERVING GROUPS", slot=self.slot, nodes=serving)
            if not serving:
                self.unserved.append(time.time() - self.lab.t0)
            self._stop.wait(0.02)

    def window(self):
        return round(len(self.unserved) * 0.02, 2)

    def stop(self):
        self._stop.set()
        self.th.join()


def soak(lab, args):
    """Mixed load in rounds; each round kills or stops one group's primary
    (while the group still has a standby) or moves the slot. A failed
    primary is not restarted (rejoin inside a cluster is not automated), so
    each group fails over at most once per member."""
    lab.up()
    lab.create(args.accounts, in_slot=1000)
    slot = created_slot(lab)
    end = time.time() + args.minutes * 60
    runs, n, where = [], 0, 0
    rnd = random.Random(args.seed)
    ctl = f"{lab.bin(lab.client, 'snc_ctl')} --control {lab.control()} move"
    dead = set()
    while time.time() < end:
        name = f"load{n}"
        pid = lab.load(name, 40, threads=args.threads, depth=args.depth, hot_pct=30)
        runs.append(name)
        time.sleep(10)
        can_fail = [g for g in range(len(lab.groups))
                    if len([k for k, nn in lab.nodes.items() if nn["group"] == g and k not in dead]) > 1]
        acts = (["move"] if len(lab.groups) > 1 else []) + (["kill", "stop"] if can_fail else [])
        act = rnd.choice(acts) if acts else None
        if act == "move":
            where = 1 - where
            r = lab.client.sh(f"{ctl} {slot} {where}", check=False, timeout=900)
            lab.note("move", to=where, result=r.stdout.strip())
        elif act:
            g = rnd.choice(can_fail)
            k = lab.primary(g)
            sk = next(kk for kk, nn in lab.nodes.items() if nn["group"] == g and kk != k and kk not in dead)
            lab.fault(act, k)
            wait(lambda: (lambda s: s and s["role"] in ("p", "n"))(status(lab.nodes[sk]["host"].addr, lab.nodes[sk]["admin"])),
                 "promoted", timeout=60)
            lab.heal()
            dead.add(k)
        lab.wait_load(name, pid, 400)
        n += 1
    lab.settle()
    ok, out = lab.check(runs)
    return {"ok": ok, "check": out, "rounds": n}


# ---- Main --------------------------------------------------------------------------------

def deploy(lab):
    for h in {id(h): h for h in lab.hosts.values()}.values():
        if h.local:
            continue
        h.sh(f"mkdir -p {h.bin}")
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes"] + [os.path.join(lab.cfg["build"], b) for b in BINARIES]
                       + [f"{h.ssh}:{h.bin}/"], check=True)
        print(f"deployed to {h.name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inventory")
    ap.add_argument("scenario", choices=["deploy", "p0", "p1", "p2", "p3", "p4", "p5", "soak"])
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--accounts", type=int, default=100000)
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--depth", type=int, default=16)
    ap.add_argument("--depths", type=lambda s: [int(x) for x in s.split(",")], default=[1, 16, 128])
    ap.add_argument("--fault", default="kill")
    ap.add_argument("--failover-ms", type=int, default=1000)
    ap.add_argument("--delay-ms", type=int, default=5)
    ap.add_argument("--cut-s", type=float, default=10)
    ap.add_argument("--slot-size", type=int, default=1000)
    ap.add_argument("--chunk", type=int, default=8, help="objects per copy request ahead of the seal")
    ap.add_argument("--crash", choices=["driver", "source", "dest"])
    ap.add_argument("--at", default="sealed")
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    lab = Lab(args.inventory, args.scenario)
    if args.scenario == "deploy":
        deploy(lab)
        return 0
    for h in {id(h): h for h in lab.hosts.values()}.values():
        h.sh(f"mkdir -p {lab.run_dir}")
    result = {"scenario": args.scenario, "args": vars(args), "hosts": {n: h.addr for n, h in lab.hosts.items()}}
    try:
        fn = {"p0": p0, "p1": lambda l, a: p1_p2(l, a, False), "p2": lambda l, a: p1_p2(l, a, True), "p3": p3,
              "p4": p4, "p5": p5, "soak": soak}[args.scenario]
        result.update(fn(lab, args))
    except Exception as e:  # keep what was measured
        result["ok"] = False
        result["error"] = f"{type(e).__name__}: {e}"
        lab.note("error", what=result["error"])
    finally:
        lab.down()
        lab.save(result)
    print(json.dumps({k: v for k, v in result.items() if k not in ("events", "args", "backlog")}, indent=1))
    return 0 if result.get("ok", True) else 1


if __name__ == "__main__":
    sys.exit(main())
