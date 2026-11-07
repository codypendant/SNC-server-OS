import gzip,csv,json,statistics as st
R="/home/solo/SNC-server-OS/bench/results/diag2-r640-20261106-telemetry/raw/"
rd=lambda p:list(csv.DictReader(gzip.open(p,'rt')))
for name in ["T-closed-r0","T-closed-r1","T-closed-r2"]:
    t=json.load(open(R+name+".times.json")); t0=t['run_start']; rel=lambda x:(int(x)-t0)/1e9
    b=rd(R+name+".buckets.csv.gz"); g=rd(R+name+".gauges.csv.gz"); d=rd(R+name+".disk.csv.gz")
    last={}
    for x in g:last[x['shard']]=x
    bg=min(int(l['ph_begin']) for l in last.values()); wk=max(int(l['ph_walked']) for l in last.values()); sy0=min(int(l['ph_sync']) for l in last.values()); sy1=max(int(l['ph_sync']) for l in last.values()); dn=max(int(l['ph_done']) for l in last.values())
    per=lambda x:(int(x['served'])*20)
    def rng(a,c): return [per(x) for x in b if a<=int(x['t_ns'])<c]
    pre=rng(bg-5_000_000_000,bg); wlk=rng(bg+100_000_000,wk); snc=rng(sy1,dn); post=rng(dn+200_000_000,dn+3_000_000_000)
    first=rng(bg-50_000_000,bg+200_000_000)
    def disk(a,c):
        s=[x for x in d if a<=int(x['t_ns'])<c]
        if len(s)<2:return None
        f,l=s[0],s[-1];dw=int(l['w_ios'])-int(f['w_ios']);dt=(int(l['t_ns'])-int(f['t_ns']))/1e9
        return dict(MBs=round((int(l['w_sect'])-int(f['w_sect']))*512/1e6/dt),await_ms=round((int(l['w_ms'])-int(f['w_ms']))/dw,2),flush_s=round((int(l['flush'])-int(f['flush']))/dt),util=round((int(l['io_ms'])-int(f['io_ms']))/dt/10))
    print(name,"ckpt total %.2fs walk %.2fs sync(max) %.2fs"%((dn-bg)/1e9,(wk-bg)/1e9,(dn-sy1)/1e9))
    print("  ops/s med pre %d walk %d sync %d post %d; first-buckets after begin %s"%(st.median(pre),st.median(wlk),st.median(snc),st.median(post),first))
    print("  disk pre",disk(bg-5_000_000_000,bg),"\n  walk",disk(bg+100_000_000,wk),"\n  sync",disk(sy1,dn))
    print("  per-shard sync s",[round((int(l['ph_done'])-int(l['ph_sync']))/1e9,2) for k,l in sorted(last.items(),key=lambda z:int(z[0]))])
