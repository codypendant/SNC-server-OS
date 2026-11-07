import gzip,csv,json,re,statistics as st
R="/home/solo/SNC-server-OS/bench/results/diag2-r640-20261106-telemetry/raw/"
rd=lambda p:list(csv.DictReader(gzip.open(p,'rt')))
g=rd(R+"T-closed-r0.gauges.csv.gz"); by={}
for x in g: by.setdefault(x['shard'],[]).append(x)
for s in ['0','3']:
    L=by[s]; l=L[-1]; bg,wk,sy,dn=[int(l[k]) for k in('ph_begin','ph_walked','ph_sync','ph_done')]
    def r(a,c):
        v=[int(y['polls'])-int(x['polls']) for x,y in zip(L,L[1:]) if a<=int(x['t_ns'])<c]; return (min(v),int(st.median(v))) if v else None
    print("shard",s,"polls/50ms (min,med) pre",r(bg-3_000_000_000,bg-50_000_000),"begin",r(bg-50_000_000,bg+50_000_000),"walk",r(bg+100_000_000,wk),"sync",r(sy,dn))
# client tail: B-w30 refusal vs max; lateness-before-refusal ordering
for n in ["B-w30-r0","B-w30-r1","B-w30-r2"]:
    t=open(R+n+".client.log").read().split('--- run')[1]
    print(n,re.search(r'overloaded (\d+)',t)[1],re.search(r'total latency.*',t)[0][-60:])
# pidstat: shard-thread cpu by phase in diag1 436k ckpt
D="/home/solo/SNC-server-OS/bench/results/diag-r640-20261106-cp1a/"
for n,ck in [("r436646-w60-ckpt60",None)]:
    rows={}
    for l in open(D+n+".pidstat.log"):
        p=l.split()
        if len(p)>=10 and p[-1].endswith('snc_server') and p[2]=='-' and p[3].isdigit():
            rows.setdefault(p[0],[]).append((p[3],float(p[4]),float(p[5]),float(p[7]),float(p[8])))
    ts=list(rows)
    print(n,"pidstat secs",len(ts))
    for tk in ts[44:54]:
        rr=rows[tk]; big=[r for r in rr if r[4]>20][:8]
        print(" ",tk,"threads",len(rr),"sum_usr %.0f sys %.0f wait %.0f cpu %.0f"%(sum(r[1] for r in rr),sum(r[2] for r in rr),sum(r[3] for r in rr),sum(r[4] for r in rr)))
