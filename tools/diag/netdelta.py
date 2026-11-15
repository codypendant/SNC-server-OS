# DIAG ONLY: deltas of selected TCP counters between two /proc/net/{netstat,snmp} reads.
import sys
KEYS = ["ListenOverflows", "ListenDrops", "TCPReqQFullDrop", "TCPTimeouts", "TCPSynRetrans", "TCPBacklogDrop",
        "TCPTimeWaitOverflow", "EmbryonicRsts", "ActiveOpens", "PassiveOpens", "AttemptFails", "RetransSegs",
        "TCPAbortOnData", "TCPRcvQDrop"]
def read(path):
    out, lines = {}, open(path).read().splitlines()
    for h, v in zip(lines[::2], lines[1::2]):
        for k, x in zip(h.split()[1:], v.split()[1:]):
            out[k] = int(x)
    return out
a = {**read(sys.argv[1]), **read(sys.argv[3])}; b = {**read(sys.argv[2]), **read(sys.argv[4])}
print(" ".join(f"{k}={b.get(k,0)-a.get(k,0)}" for k in KEYS if k in b))
