import subprocess, re, statistics, json, sys, shutil, os
S=os.path.dirname(os.path.abspath(__file__))
bins={"before":f"{S}/before/snc_bench","after":f"{S}/snc_bench.after"}
cfgs={"mem_local":["--remote","0"],"mem_remote50":["--remote","50"],"durable":["--remote","0","--data-dir",f"{S}/t9d"]}
pairs={"mem_local":10,"mem_remote50":10,"durable":6}
res={c:{"before":[],"after":[]} for c in cfgs}
for c,args in cfgs.items():
    for i in range(pairs[c]):
        order=["before","after"] if i%2==0 else ["after","before"]
        for v in order:
            shutil.rmtree(f"{S}/t9d",ignore_errors=True)
            out=subprocess.run([bins[v],"--shards","2","--seconds","3","--objects","1000000"]+args,capture_output=True,text=True).stdout
            m=re.search(r"ops/s (\d+)",out)
            res[c][v].append(int(m[1]))
        print(c,i,res[c]["before"][-1],res[c]["after"][-1],flush=True)
summ={}
for c in cfgs:
    b,a=res[c]["before"],res[c]["after"]
    summ[c]=dict(before_median=statistics.median(b),after_median=statistics.median(a),
      diff_pct=round(100*(statistics.median(a)-statistics.median(b))/statistics.median(b),2),
      before_min=min(b),before_max=max(b),after_min=min(a),after_max=max(a),
      before_spread_pct=round(100*(max(b)-min(b))/statistics.median(b),1),after_spread_pct=round(100*(max(a)-min(a))/statistics.median(a),1),
      before=b,after=a)
json.dump(summ,open(f"{S}/t9.json","w"),indent=1)
for c,v in summ.items(): print(c,{k:v[k] for k in v if k not in("before","after")})
