import json, glob, statistics as st, sys, os
D=os.path.join(os.path.dirname(os.path.abspath(__file__)), "raw")
def load(pat):
    return [json.load(open(f)) for f in sorted(glob.glob(f"{D}/{pat}"))]
def med(v):
    v=[x for x in v if x is not None]
    return (round(st.median(v),1), round(min(v),1), round(max(v),1)) if v else None
def g(d,*k):
    for x in k:
        if d is None: return None
        d=d.get(x) if isinstance(d,dict) else None
    return d
rows=[]
for name,pat in [("on shared","m_on_shared_*.json"),("off shared","m_off_shared_*.json"),("on unique","m_on_unique_*.json"),("off unique","m_off_unique_*.json"),
                 ("on shared T=0.25ms","sw_us250_*.json"),("on shared T=4ms","sw_us4000_*.json"),("on unique B=64KiB","sw_by65536_*.json"),("on unique B=4MiB","sw_by4194304_*.json"),("on unique W=64KiB","sw_wb65536_*.json")]:
    R=load(pat)
    if not R: continue
    f=lambda *k: med([(lambda x: float(x) if str(x).replace(".","",1).isdigit() else None)(g(r,*k)) if g(r,*k) is not None else None for r in R])
    print(f"## {name} (n={len(R)}, all DONE={all(r['move']=='DONE' for r in R)})")
    for lab,k in [("move_s",("move_s",)),("final_stage_ms",("final_stage_ms",)),("E_MIGRATING client ms",("e_migrating_client_ms",)),("E_MIGRATING trace ms",("e_migrating_trace_ms",)),
                  ("units",("chain","units")),("puts",("chain","put")),("unit p50",("chain","p50")),("unit p95",("chain","p95")),("unit p99",("chain","p99")),("unit max",("chain","max")),
                  ("shard_ms",("chain","shard_ms")),("bytes",("chain","bytes")),("window_hw",("chain","window_hw")),("window_waits",("chain","window_waits")),
                  ("long_iter final max",("dest_long_iters_ge20ms","final_stage","max")),("long_iter final n",("dest_long_iters_ge20ms","final_stage","n")),("long_iter whole max",("dest_long_iters_ge20ms","whole_run","max")),
                  ("HEALTH final p50",("HEALTH","final_stage","p50")),("HEALTH final p99",("HEALTH","final_stage","p99")),("HEALTH final max",("HEALTH","final_stage","max")),
                  ("STATUS final p50",("STATUS","final_stage","p50")),("STATUS final p95",("STATUS","final_stage","p95")),("STATUS final p99",("STATUS","final_stage","p99")),("STATUS final max",("STATUS","final_stage","max")),
                  ("fg final p50",("fg","final_stage","p50")),("fg final p95",("fg","final_stage","p95")),("fg final p99",("fg","final_stage","p99")),("fg final max",("fg","final_stage","max")),("fg final n",("fg","final_stage","n")),
                  ("fg whole max",("fg","whole_move","max")),("STATUS whole max",("STATUS","whole_move","max")),
                  ("stall final max",("health_stall_ms_max","final_stage")),("stall whole max",("health_stall_ms_max","whole_move"))]:
        print(f"  {lab}: {f(*k)}")
    print("  VmHWM:", [r.get("VmHWM") for r in R])
    print("  long iters final detail:", [r.get("dest_long_iters_final_detail") for r in R][:3])
