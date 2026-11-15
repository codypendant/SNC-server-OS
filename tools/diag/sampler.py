# DIAG ONLY: 0.5 s time series of per-CPU scheduling and softirq state (run as root).
import os, sys, time, re
out = open(sys.argv[1], "w", buffering=1)
def ksoft():
    r = {}
    for p in os.listdir("/proc"):
        if not p.isdigit(): continue
        try:
            comm = open(f"/proc/{p}/comm").read().strip()
            if comm.startswith("ksoftirqd/"):
                st = open(f"/proc/{p}/stat").read().split(") ")[1].split()[0]
                run, wait, _ = open(f"/proc/{p}/schedstat").read().split()
                r[comm[10:]] = (st, int(run), int(wait))
        except OSError: pass
    return r
def softirq():
    for l in open("/proc/softirqs"):
        if l.strip().startswith("NET_RX"): return [int(x) for x in l.split()[1:]]
def disk():
    r = {}
    for l in open("/proc/diskstats"):
        f = l.split()
        if re.match(r"(nvme\d+n\d+|sd[a-z]|vd[a-z])$", f[2]): r[f[2]] = (int(f[3]) + int(f[7]), int(f[11]), int(f[12]))  # ios done, in flight, io ms
    return r
def dstate():
    n = 0
    for p in os.listdir("/proc"):
        if p.isdigit():
            try:
                for t in os.listdir(f"/proc/{p}/task"):
                    if open(f"/proc/{p}/task/{t}/stat").read().split(") ")[1][0] == "D": n += 1
            except OSError: pass
    return n
def tcp():
    d = {}
    for path in ("/proc/net/netstat", "/proc/net/snmp"):
        ls = open(path).read().splitlines()
        for h, v in zip(ls[::2], ls[1::2]):
            for k, x in zip(h.split()[1:], v.split()[1:]): d[k] = int(x)
    return d
def sched():
    nr, kw = {}, {}
    try: txt = open("/sys/kernel/debug/sched/debug").read()
    except OSError: return nr, kw
    for blk in re.split(r"\ncpu#", txt)[1:]:
        c = int(blk.split(",")[0])
        m = re.search(r"\.nr_running\s+:\s+(\d+)", blk); nr[c] = int(m.group(1)) if m else -1
        rt = blk.split("runnable tasks:")[-1]
        kw[c] = sum(1 for l in rt.splitlines() if re.match(r"\s*>?R\s+kworker", l))
    return nr, kw
def dump(t):
    out.write(f"=== DUMP at {t:.1f}: cpu#0 runnable tasks\n")
    try: txt = open("/sys/kernel/debug/sched/debug").read()
    except OSError: return
    blk = re.split(r"\ncpu#", txt)[1]
    out.write("\n".join(l for l in blk.split("runnable tasks:")[-1].splitlines() if re.match(r"\s*>?R\s", l)) + "\n")
    for k in ("nr_running", "min_vruntime", "avg_vruntime", "zero_vruntime", "nr_queued", "h_nr"):
        for l in blk.splitlines():
            if k in l: out.write(l.strip() + "\n")
    hist = {}
    for p in os.listdir("/proc"):
        if not p.isdigit(): continue
        try:
            if open(f"/proc/{p}/comm").read().strip() not in ("test_migrate", "snc_witness"): continue
            for t in os.listdir(f"/proc/{p}/task"):
                st = open(f"/proc/{p}/task/{t}/stat").read().split(") ")[1].split()
                if st[0] != "R" or st[36] != "0": continue
                sc = open(f"/proc/{p}/task/{t}/syscall").read().split()[0]
                hist[sc] = hist.get(sc, 0) + 1
        except (OSError, IndexError): pass
    try:
        T = "/sys/kernel/tracing"
        def w(f, v): open(f"{T}/{f}", "w").write(v)
        w("tracing_on", "0"); w("trace", ""); w("tracing_cpumask", "1")
        w("events/sched/sched_switch/enable", "1"); w("events/sched/sched_wakeup/enable", "1"); w("tracing_on", "1")
        time.sleep(0.5)
        w("tracing_on", "0"); w("events/sched/sched_switch/enable", "0"); w("events/sched/sched_wakeup/enable", "0")
        tr = open(f"{T}/trace").read().splitlines()
        runs, ontime, wakes, last = {}, {}, {}, None
        for l in tr:
            m = re.search(r"\s(\d+\.\d+): sched_switch: prev_comm=(.+?) prev_pid=\d+ prev_prio=\d+ prev_state=(\S+) ==> next_comm=(.+?) next_pid=(\d+)", l)
            if m:
                ts, nxt = float(m.group(1)), m.group(4)
                if last: ontime[last[0]] = ontime.get(last[0], 0) + ts - last[1]
                runs[nxt] = runs.get(nxt, 0) + 1; last = (nxt, ts); continue
            m = re.search(r"sched_wakeup: comm=(\S+)", l)
            if m: wakes[m.group(1)] = wakes.get(m.group(1), 0) + 1
        top = sorted(ontime.items(), key=lambda x: -x[1])[:12]
        out.write(f"ftrace cpu0 0.5 s: switches={sum(runs.values())} ontime_ms={[(k, round(v*1000,1), runs.get(k,0)) for k,v in top]}\n")
        out.write(f"  ksoftirqd/0 picked {runs.get('ksoftirqd/0',0)}x, woken {wakes.get('ksoftirqd/0',0)}x; kworkers picked {sum(v for k,v in runs.items() if k.startswith('kworker/0'))}x\n")
    except OSError as e:
        out.write(f"ftrace failed: {e}\n")
    out.write(f"R threads on cpu0 by syscall (24=sched_yield 230=clock_nanosleep 7=poll 426=io_uring_enter): {hist}\n=== END DUMP\n")
dumps = 0; stuck = 0
k0, s0, t0 = ksoft(), softirq(), tcp(); d0 = disk(); start = time.time()
while True:
    time.sleep(0.5)
    k1, s1, t1 = ksoft(), softirq(), tcp(); nr, kw = sched(); d1 = disk(); nd = dstate()
    dk = ' '.join(f'{k}:{d1[k][0]-d0.get(k,d1[k])[0]}/{d1[k][1]}' for k in sorted(d1))
    ks = " ".join(f"{c}:{k1[c][0]}{(k1[c][2]-k0.get(c,k1[c])[2])//1000000}" for c in sorted(k1))
    sq = " ".join(str(b - a) for a, b in zip(s0, s1))
    out.write(f"{time.time()-start:7.1f} nr={','.join(str(nr.get(c,-1)) for c in range(4))} kw={','.join(str(kw.get(c,-1)) for c in range(4))} "
              f"ksoft_wait_ms[{ks}] netrx[{sq}] opens={t1['ActiveOpens']-t0['ActiveOpens']} synrtx={t1['TCPSynRetrans']-t0['TCPSynRetrans']} "
              f"psi={open('/proc/pressure/cpu').readline().split()[1]} io_psi={open('/proc/pressure/io').readline().split()[1]} D={nd} disk[{dk}]\n")
    stuck = stuck + 1 if k1.get("0", ("S",))[0] == "R" else 0
    if stuck >= 4 and dumps < 2 and stuck % 8 == 4:
        dumps += 1; dump(time.time() - start)
    k0, s0, t0, d0 = k1, s1, t1, d1
