import subprocess, socket, time, os, signal, shutil, sys
B="build"
W="expB"
shutil.rmtree(W, ignore_errors=True); os.makedirs(W)
U="0123456789abcdef0123456789abcdef"
base=int(sys.argv[1]) if len(sys.argv)>1 else 43000
def ask(port, line, t=0.5):
    try:
        s=socket.create_connection(("127.0.0.1",port),timeout=t); s.settimeout(t)
        s.sendall((line+"\n").encode()); d=b""
        while not d.endswith(b"\n"):
            c=s.recv(65536)
            if not c: break
            d+=c
        s.close(); return d.decode().strip()
    except Exception as e: return None
procs=[]
def P(args,name):
    f=open(f"{W}/{name}.log","w"); p=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT); procs.append(p); return p
wp=base; a0=base+2; a1=base+3; c0=base+10; c1=base+20; sb=base+40
try:
    n1=P([f"{B}/snc_server","--shards","1","--port",str(c1),"--data-dir",f"{W}/d1","--admin-port",str(a1),"--standby",str(sb),"--space-uuid",U,"--witness",f"127.0.0.1:{wp}"],"n1")
    time.sleep(1)
    n0=P([f"{B}/snc_server","--shards","1","--port",str(c0),"--data-dir",f"{W}/d0","--admin-port",str(a0),"--replica-of",f"127.0.0.1:{sb}","--space-uuid",U,"--witness",f"127.0.0.1:{wp}"],"n0")
    time.sleep(1)
    w=P([f"{B}/snc_witness","--port",str(wp),"--primary",f"127.0.0.1:{a0}","--standby",f"127.0.0.1:{a1}","--state",f"{W}/w.state","--failover-ms","1000","--poll-ms","100","--new-state","1"],"w")
    t=time.time()
    while time.time()-t<20:
        r=ask(wp,"READ ha/0")
        if r and r.split()[4]=="0": break
        time.sleep(0.1)
    print("ready state:",ask(wp,"READ ha/0"))
    print("n0 HEALTH before:",(ask(a0,"HEALTH") or "")[:60])
    os.kill(n0.pid,signal.SIGSTOP); t0=time.time()
    while time.time()-t0<15:
        h=ask(a1,"HEALTH") or ""
        if h.startswith("role=p") or h.startswith("role=n"): break
        time.sleep(0.05)
    print("promoted after %.2fs: n1 %s"%(time.time()-t0,h[:40]), "state", ask(wp,"READ ha/0"))
    os.kill(n0.pid,signal.SIGCONT); t1=time.time()
    seen=[]
    while time.time()-t1<3:
        h=(ask(a0,"HEALTH") or "none")[:24]
        if not seen or seen[-1][1]!=h: seen.append((round(time.time()-t1,3),h))
        time.sleep(0.02)
    print("n0 after SIGCONT:",seen)
    print("n0 STATUS:", (ask(a0,"STATUS",2) or "")[:80])
finally:
    for p in procs:
        try: os.kill(p.pid,signal.SIGCONT); p.kill()
        except Exception: pass
    for p in procs: p.wait()
for n in ("n0","n1","w"):
    print("----",n); print(open(f"{W}/{n}.log").read()[-1500:])
