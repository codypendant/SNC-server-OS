import json,re,sys
D="/home/solo/SNC-server-OS/bench/results/diag-r640-20261106-cp1a/"
for n in ["r2183233-w130-nockpt","r436646-w60-nockpt","r2183233-w130-ckpt60","r436646-w60-ckpt60","r218323-w60-ckpt60"]:
    T=json.load(open(D+n+".times.json")); s=[json.loads(l) for l in open(D+n+".samples.jsonl")]
    rows=[]
    for a,c in zip(s,s[1:]):
        da,dc=a['disk'],c['disk']; dt=c['t']-a['t']
        if dt<=0: continue
        dw=dc['w_ios']-da['w_ios']; dm=dc['w_ms']-da['w_ms']; mb=(dc['w_sect']-da['w_sect'])*512/1e6
        rows.append((c['t']-T['load_done'],mb/dt,dm/dw if dw else 0,dt,mb, int(re.search(r'waiters=(\d+)',c['res'])[1]) if c.get('res') else -1, int(re.search(r'ckpt_running=(\d+)',c['res'])[1]) if c.get('res') else -1))
    rr=[r for r in rows if r[0]>0.7]
    base=sorted(r[1] for r in rr)[len(rr)//2]
    ev=[]
    for r in rr:
        if r[2]>=0.25:
            if ev and r[0]-ev[-1][1]<0.35: ev[-1][1]=r[0]; ev[-1][2]+=r[4]-base*r[3]; ev[-1][3]=max(ev[-1][3],r[2]);ev[-1][4]|=r[6]>0
            else: ev.append([r[0],r[0],r[4]-base*r[3],r[2],r[6]>0])
    print(n,"baseMB/s %.0f"%base,"events",[(round(a,1),round(b,1),round(m),round(w,1),'CK' if k else '') for a,b,m,w,k in ev])
