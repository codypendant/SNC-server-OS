import gzip,csv,json,glob,os
R="/home/solo/SNC-server-OS/bench/results/diag2-r640-20261106-telemetry/raw/"
rd=lambda p:list(csv.DictReader(gzip.open(p,'rt')))
for name in ["T-w30-r0","T-w30-r1","T-w30-r2","T-closed-r0","T-closed-r1","T-closed-r2"]:
    t=json.load(open(R+name+".times.json")); t0=t['run_start']; rel=lambda x:(int(x)-t0)/1e9
    d=rd(R+name+".disk.csv.gz"); D=[]
    for a,c in zip(d,d[1:]):
        dtt=(int(c['t_ns'])-int(a['t_ns']))/1e9; dw=int(c['w_ios'])-int(a['w_ios']); dm=int(c['w_ms'])-int(a['w_ms'])
        D.append((rel(c['t_ns']),(int(c['w_sect'])-int(a['w_sect']))*512/1e6,dtt,dm/dw if dw else 0))
    D=[x for x in D if x[0]>0.7]
    base=sorted(x[1]/x[2] for x in D)[len(D)//2]
    # events: await >=0.25ms; contiguous within .25s
    ev=[]
    for tt,mb,dtt,aw in D:
        if aw>=0.25:
            if ev and tt-ev[-1][1]<0.25: ev[-1][1]=tt; ev[-1][2]+=mb-base*dtt; ev[-1][3]=max(ev[-1][3],aw)
            else: ev.append([tt,tt,mb-base*dtt,aw])
    print(name,"base %.0f MB/s"%base,"events(start,end,excessMB,maxawait):",[(round(a,2),round(b,2),round(m),round(w,1)) for a,b,m,w in ev])
