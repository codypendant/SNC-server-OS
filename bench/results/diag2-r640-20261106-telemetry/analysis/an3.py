import gzip,csv,json,sys,collections as C
R="/home/solo/SNC-server-OS/bench/results/diag2-r640-20261106-telemetry/raw/"
rd=lambda p:list(csv.DictReader(gzip.open(p,'rt')))
for name in sys.argv[1:]:
    t=json.load(open(R+name+".times.json")); t0=t['run_start']; rel=lambda x:(int(x)-t0)/1e9
    b=rd(R+name+".buckets.csv.gz"); g=rd(R+name+".gauges.csv.gz"); d=rd(R+name+".disk.csv.gz")
    print("=====",name)
    # sum gauges per sample time
    S=C.OrderedDict()
    for x in g:
        k=x['t_ns']; s=S.setdefault(k,dict(em=0,mx=0,ud=0,wt=0,ov=0,ofl=0,ph=[],pl=0))
        e=int(x['emits']); s['em']+=e; s['mx']=max(s['mx'],e); s['ud']+=int(x['undurable']); s['wt']+=int(x['waiters']); s['ov']+=int(x['overloaded']); s['ofl']+=int(x['overflow']); s['ph'].append(x['ckpt_phase']); s['pl']+=int(x['polls'])
    ts=[(rel(k),v) for k,v in S.items() if rel(k)>0]
    meds=sorted(v['mx'] for _,v in ts)[len(ts)//2]
    # all stall events: sample where sum undurable > 3x median
    um=sorted(v['ud'] for _,v in ts)[len(ts)//2]
    ev=[];
    for tt,v in ts:
        if v['ud']>max(3*um,1000):
            if ev and tt-ev[-1][1]<0.3: ev[-1][1]=tt; ev[-1][2]=max(ev[-1][2],v['ud']); ev[-1][3]=max(ev[-1][3],v['mx'])
            else: ev.append([tt,tt,v['ud'],v['mx']])
    print(" median sum_undur",um,"median max-shard emits",meds)
    print(" stall events start-end peak_sum_undur peak_shard_emits:",[(round(a,2),round(c,2),u,m) for a,c,u,m in ev])
    if ev:
        gaps=[round(ev[i+1][0]-ev[i][0],2) for i in range(len(ev)-1)]; print(" gaps",gaps)
    D=[]
    for a,c in zip(d,d[1:]):
        dw=int(c['w_ios'])-int(a['w_ios']); dm=int(c['w_ms'])-int(a['w_ms']); ds=int(c['w_sect'])-int(a['w_sect']); df=int(c['flush'])-int(a['flush'])
        dtt=(int(c['t_ns'])-int(a['t_ns']))/1e9
        D.append((rel(c['t_ns']),dw,dm/dw if dw else 0,ds*512/1e6/dtt,df,ds*512/1024/dw if dw else 0,int(c['inflight'])))
    # disk burst events >550MB/s
    print(" disk >550MB/s samples:",[(round(x[0],2),round(x[3])) for x in D if x[3]>550 and x[0]>0][:40])
    if len(sys.argv)>1 and name.startswith('T-w30'):
        c=ev[-1][0] if ev else 0
        big=max(ts,key=lambda z:z[1]['mx'])[0]
        print(" around peak",big)
        for x in D:
            if abs(x[0]-big)<0.45: print("  disk %.2f w=%d aw=%.2f %.0fMB/s fl=%d kb/w=%.0f inf=%d"%x)
        for tt,v in ts:
            if abs(tt-big)<0.3: print("  g %.2f sum_emits %d max %d undur %d waiters %d ovf %d ov %d dpolls"%(tt,v['em'],v['mx'],v['ud'],v['wt'],v['ofl'],v['ov']))
        for x in b:
            r=rel(x['t_ns'])
            if abs(r-big)<0.3: print("  b %.2f srv %s ex %s idx %s lat1 %s late %s"%(r,x['served'],x['exec_refused'],x['index_refused'],x['lat_over_1ms'],x['late_over_1ms']))
    if name.startswith('T-closed'):
        print(" PHASES per shard:")
        last={}
        for x in g:last[x['shard']]=x
        ph=[(rel(l['ph_begin']),rel(l['ph_walked']),rel(l['ph_sync']),rel(l['ph_done'])) for l in last.values()]
        print("  begin %.3f walked %.3f-%.3f sync %.3f-%.3f done %.3f-%.3f"%(min(p[0] for p in ph),min(p[1] for p in ph),max(p[1] for p in ph),min(p[2] for p in ph),max(p[2] for p in ph),min(p[3] for p in ph),max(p[3] for p in ph)))
        st=min(p[0] for p in ph)
        for tt,v in ts:
            if st-0.15<tt<st+3.0 and int(tt*20)%2==0: print("  g %.2f sum_em %d max %d undur %d wait %d ofl %d ov %d ph %s"%(tt,v['em'],v['mx'],v['ud'],v['wt'],v['ofl'],v['ov'],''.join(v['ph'])))
        for x in D:
            if st-0.25<x[0]<st+3.0: print("  d %.2f w=%d aw=%.2f %.0fMB/s fl=%d kb/w=%.0f inf=%d"%x)
