#!/bin/bash
# DIAG ONLY: live state of a failing test_migrate scenario (removed before merge).
d=$1
echo "=== SNAPSHOT $(date +%T.%N) dir=$d"
base=$(cut -d' ' -f1 $d/env)
echo "--- psi"; cat /proc/pressure/cpu
echo "--- listen sockets in [$base,$((base+64))): Recv-Q=accept queue, Send-Q=backlog"
ss -ltn | awk -v b=$base 'NR>1{n=split($4,a,":"); p=a[n]; if (p>=b && p<b+64) print p-b, $2, $3}' | sort -n | tr '\n' ';'; echo
echo "--- tcp states toward the scenario's ports"
ss -tan | awk -v b=$base 'NR>1{n=split($5,a,":"); p=a[n]; m=split($4,c,":"); q=c[m]; if ((p>=b&&p<b+64)||(q>=b&&q<b+64)) print $1}' | sort | uniq -c | tr '\n' ' '; echo
echo "--- established with data queued (state recvq sendq local peer)"
ss -tan state established | awk -v b=$base 'NR>1 && ($1>0||$2>0){print $1,$2,$3,$4}' | head -20
echo "--- netstat"; nstat -az 2>/dev/null | grep -E 'ListenOverflows|ListenDrops|TCPReqQFullDrop|TCPTimeouts|TCPSynRetrans|TCPBacklogDrop|TW |TCPAbortOnData|TCPRcvQDrop|TCPOFODrop|TCPTimeWaitOverflow|EmbryonicRsts|TCPZeroWindowDrop' | awk '{print $1"="$2}' | tr '\n' ' '; echo
echo "--- threads of this test (psr stat pcpu wchan comm), by cpu"
ps -eLo pgid,psr,stat,pcpu,wchan:24,comm | awk -v g=$(ps -o pgid= -p $PPID | tr -d ' ') '$1==g' | awk '{print $2, $3, $5, $6}' | sort | uniq -c | sort -k2n -k1rn | head -40
echo "--- busiest threads system-wide"
ps -eLo psr,stat,pcpu,comm --sort=-pcpu | head -15
echo "--- runnable per cpu (sched debug)"
sudo cat /sys/kernel/debug/sched/debug 2>/dev/null | awk '/^cpu#/{c=$1} /nr_running/{print c, $0}' | head -8
sudo cat /sys/kernel/debug/sched/debug 2>/dev/null | awk '/runnable tasks:/{p=1;next} /^cpu#/{p=0} p && /^ *[RS>]/' | awk '{print $1,$2,$3}' | grep -v ' S ' | head -40
echo "--- cpu0/cpu1 run time over 2 s (ms ran, state, policy/nice, comm, pid): kernel threads and top consumers"
snap() { for t in /proc/[0-9]*/task/[0-9]*; do read -r st < $t/stat 2>/dev/null || continue; echo "${t##*/} $(cut -d' ' -f1 $t/schedstat 2>/dev/null) $st"; done; }
snap > /tmp/snc-s0.$$; sleep 2; snap > /tmp/snc-s1.$$
python3 - /tmp/snc-s0.$$ /tmp/snc-s1.$$ <<'PY'
import sys,re
def load(f):
    d={}
    for l in open(f):
        m=re.match(r'(\d+) (\d+) \d+ \((.*)\) (\S) (.*)',l)
        if not m: continue
        rest=m.group(5).split()
        d[m.group(1)]=(int(m.group(2)),m.group(3),m.group(4),int(rest[35]),rest[15],rest[37])
    return d
a,b=load(sys.argv[1]),load(sys.argv[2])
for cpu in (0,1):
    rows=[(b[t][0]-a[t][0],t,b[t]) for t in b if t in a and b[t][3]==cpu]
    rows.sort(reverse=True)
    k=[r for r in rows if re.match(r'(ksoftirqd|kworker|jbd2|migration|rcu)',r[2][1]) and (r[2][2]=='R' or r[0]>0)]
    print("cpu%d top:"%cpu, "; ".join("%s/%s %.1fms %s"%(r[2][1],r[1],r[0]/1e6,r[2][2]) for r in rows[:8]))
    print("cpu%d kthreads runnable or ran:"%cpu, "; ".join("%s/%s %.1fms %s"%(r[2][1],r[1],r[0]/1e6,r[2][2]) for r in k[:12]))
PY
rm -f /tmp/snc-s0.$$ /tmp/snc-s1.$$
if [ ! -e /tmp/snc-gdb-done ]; then touch /tmp/snc-gdb-done
for pid in $(pgrep -x test_migrate); do
  n=$(sudo ls -l /proc/$pid/fd 2>/dev/null | grep -oE "$d/n[0-9]" | head -1)
  [ -z "$n" ] && continue
  echo "--- gdb ${n##*/} pid $pid"
  sudo timeout 60 gdb -p $pid -batch -q -ex "set pagination off" -ex "thread apply all bt 14" 2>/dev/null | grep -E '^Thread|^#' | sed -E 's/ \(.*\) at / at /' | cut -c1-200
done; fi
echo "=== END SNAPSHOT"
