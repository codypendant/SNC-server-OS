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
k0, s0, t0 = ksoft(), softirq(), tcp(); start = time.time()
while True:
    time.sleep(0.5)
    k1, s1, t1 = ksoft(), softirq(), tcp(); nr, kw = sched()
    ks = " ".join(f"{c}:{k1[c][0]}{(k1[c][2]-k0.get(c,k1[c])[2])//1000000}" for c in sorted(k1))
    sq = " ".join(str(b - a) for a, b in zip(s0, s1))
    out.write(f"{time.time()-start:7.1f} nr={','.join(str(nr.get(c,-1)) for c in range(4))} kw={','.join(str(kw.get(c,-1)) for c in range(4))} "
              f"ksoft_wait_ms[{ks}] netrx[{sq}] opens={t1['ActiveOpens']-t0['ActiveOpens']} synrtx={t1['TCPSynRetrans']-t0['TCPSynRetrans']} "
              f"psi={open('/proc/pressure/cpu').readline().split()[1]}\n")
    k0, s0, t0 = k1, s1, t1
