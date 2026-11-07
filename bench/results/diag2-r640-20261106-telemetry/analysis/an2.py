import gzip,csv,json,sys,collections as C
R="/home/solo/SNC-server-OS/bench/results/diag2-r640-20261106-telemetry/raw/"
def rd(p):
    return list(csv.DictReader(gzip.open(p,'rt')))
for name in sys.argv[1:]:
    t=json.load(open(R+name+".times.json")); t0=t['run_start']; rel=lambda x:(int(x)-t0)/1e9
    b=rd(R+name+".buckets.csv.gz"); g=rd(R+name+".gauges.csv.gz"); d=rd(R+name+".disk.csv.gz")
    print("=====",name,"run_len %.1f"%((t['run_end']-t0)/1e9), "srvup->run %.2f"%((t0-t['server_up'])/1e9))
    # refusals
    rf=[(rel(x['t_ns']),int(x['exec_refused']),int(x['index_refused']),int(x['lat_over_1ms']),int(x['lat_over_100ms']),int(x['late_over_1ms'])) for x in b]
    print(" client refusal buckets (t,exec,idx,lat>1ms,lat>100ms,late>1ms):")
    for r in rf:
        if r[1] or r[2]: print("  %.2f %d %d %d %d %d"%r)
    print(" total exec",sum(r[1] for r in rf)," idx",sum(r[2] for r in rf))
    # stall buckets
    st=[r for r in rf if r[3]>2000*5]
    print(" buckets lat>1ms>10k: ", [ "%.2f"%r[0] for r in st][:60])
    # ckpt phases
    sh=C.defaultdict(list)
    for x in g: sh[int(x['shard'])].append(x)
    for s in sorted(sh):
        last=sh[s][-1]
        ov=[(rel(x['t_ns']),int(x['overloaded'])) for x in sh[s]]
        base=None; steps=[]
        prev=0
        for tt,o in ov:
            if o!=prev and tt>0: steps.append((round(tt,2),o-prev))
            prev=o
        pk=max(sh[s],key=lambda x:int(x['emits']))
        pkw=max(sh[s],key=lambda x:int(x['undurable']))
        pkv=max(int(x['waiters']) for x in sh[s])
        print(f" sh{s}: over_total {last['overloaded']} steps {steps[:8]} peak_emits {pk['emits']}@{rel(pk['t_ns']):.2f} (emit_wait {pk['emit_wait']}, undur {pk['undurable']}) peak_undur {pkw['undurable']}@{rel(pkw['t_ns']):.2f} peak_waiters {pkv} peak_overflow {max(int(x['overflow']) for x in sh[s])}")
        if int(last['ph_begin']): print(f"    ckpt begin {rel(last['ph_begin']):.3f} walked {rel(last['ph_walked']):.3f} sync {rel(last['ph_sync']):.3f} done {rel(last['ph_done']):.3f}")
        # poll stall : max gap in polls delta
        mn=1e18;mt=0;p=None
        for x in sh[s]:
            v=int(x['polls'])
            if p is not None and rel(x['t_ns'])>0:
                dp=v-p
                if dp<mn: mn=dp;mt=rel(x['t_ns'])
            p=v
        print(f"    min polls/50ms {mn}@{mt:.2f}")
    # disk 100 ms deltas: write await
    rows=[]
    for a,c in zip(d,d[1:]):
        dw=int(c['w_ios'])-int(a['w_ios']); dm=int(c['w_ms'])-int(a['w_ms']); ds=int(c['w_sect'])-int(a['w_sect']); df=int(c['flush'])-int(a['flush']); dfm=int(c['flush_ms'])-int(a['flush_ms'])
        dtt=(int(c['t_ns'])-int(a['t_ns']))/1e9
        rows.append((rel(c['t_ns']),dw,dm/dw if dw else 0,ds*512/1e6/dtt,df,dfm/df if df else 0,int(c['inflight']),(int(c['io_ms'])-int(a['io_ms']))/dtt/10))
    print(" disk worst 100ms by await (t, wios, await_ms, MB/s, flushes, flush_ms, inflight, util%):")
    for r in sorted(rows,key=lambda r:-r[2])[:10]: print("  %.2f w=%d aw=%.2f %.0fMB/s fl=%d flms=%.2f inf=%d util=%.0f"%r)
    print(" disk median await",sorted(r[2] for r in rows)[len(rows)//2],"median MB/s",sorted(r[3] for r in rows)[len(rows)//2])
