import json,re,sys,glob,os
OUT=sys.argv[1]
names=[os.path.basename(p)[:-11] for p in sorted(glob.glob(OUT+"/*.client.log"),key=os.path.getmtime)]
for n in names:
    b=f"{OUT}/{n}"; T=json.load(open(b+".times.json")); c=open(b+".client.log").read(); run=c.split('--- run')[-1]
    g=lambda p:(re.search(p,run) or [None,None])[1]
    t=re.search(r'total latency ns\s+n (\d+)\s+p50 (\d+)\s+p95 (\d+)\s+p99 (\d+)\s+p99.9 (\d+)\s+max (\d+)',run)
    l=re.search(r'issue lateness ns\s+p50 (\d+)\s+p99 (\d+)\s+p99.9 (\d+)\s+max (\d+)\s+late_over_1ms (\d+)',run)
    srv=open(b+".server.log").read()
    print(f"\n##### {n}: overl {g(r'overloaded (\d+)')} idxref {g(r'refused (\d+)')} total/s {g(r'total ops/s (\d+)')} p99us {int(t[4])//1000} p99.9us {int(t[5])//1000} maxms {int(t[6])//10**6}"+(f" lateMaxms {int(l[4])//10**6} late>1ms {l[5]}" if l else ""))
    print("  ckpt lines:",re.findall(r'checkpoint: .*',srv))
    S=[json.loads(x) for x in open(b+".samples.jsonl")]
    t0=T['run_start']; rows=[]
    for i,s in enumerate(S):
        if 'res' not in s: continue
        d=dict(kv.split('=') for kv in s['res'].split())
        rows.append(dict(t=s['t']-t0,em=int(d['emits_in_flight']),wt=int(d['waiters']),ov=int(d['overflow']),und=int(d['wal_undurable']),ck=int(d['ckpt_running']),ckn=int(d['ckpts']),disk=s['disk'],gap=(s['t']-S[i-1]['t']) if i else 0))
    rows=[r for r in rows if r['t']>=-1]
    # ckpt intervals
    iv=[];cur=None
    for r in rows:
        if r['ck'] and cur is None: cur=r['t']
        if not r['ck'] and cur is not None: iv.append((round(cur,1),round(r['t'],1)));cur=None
    print("  ckpt_running intervals (s after run_start):",iv,"max sampler gap ms",round(max(r['gap'] for r in rows)*1000))
    def seg(name,a,b):
        rs=[r for r in rows if a<=r['t']<b]
        if len(rs)<2: return
        d0,d1=rs[0]['disk'],rs[-1]['disk'];dt=rs[-1]['t']-rs[0]['t']
        ws=(d1['w_ios']-d0['w_ios']);wms=d1['w_ms']-d0['w_ms'];fl=d1['flush']-d0['flush'];flms=d1['flush_ms']-d0['flush_ms']
        print(f"  {name:12s} em max {max(r['em'] for r in rs):7d} wait max {max(r['wt'] for r in rs):6d} undur max {max(r['und'] for r in rs):7d} ovf max {max(r['ov'] for r in rs):6d} | MB/s {(d1['w_sect']-d0['w_sect'])*512/1e6/dt:7.0f} w_await ms {wms/max(ws,1):6.2f} flush/s {fl/dt:6.0f} fl_await {flms/max(fl,1):6.2f} inflt max {max(r['disk']['inflight'] for r in rs)}")
    end=T['run_end']-t0
    if iv:
        seg("pre",0,iv[0][0])
        for a,bb in iv:
            seg(f"ck {a}-{bb}",a,bb); seg(f"post {bb}",bb,bb+5)
    else: seg("all",0,end)
    # top emit peaks timeline
    rr=sorted(rows,key=lambda r:-r['em'])[:1]
    if rr and rr[0]['em']>0:
        pk=rr[0]['t']; print("  around peak em (t em undur wait ck inflt):",[ (round(r['t'],1),r['em'],r['und'],r['wt'],r['ck'],r['disk']['inflight']) for r in rows if abs(r['t']-pk)<0.5][:11])
    print("  em>4096 spans:",end=' ')
    cur=None;sp=[]
    for r in rows:
        if r['em']>4096 and cur is None: cur=r['t']
        if r['em']<=4096 and cur is not None: sp.append((round(cur,1),round(r['t'],1)));cur=None
    print(sp[:12],f"(total {len(sp)})")
