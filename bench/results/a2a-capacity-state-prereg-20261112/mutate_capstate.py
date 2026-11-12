#!/usr/bin/env python3
"""Mutation check of test_capstate.py: each mutant edits capstate.py in place (restored afterwards), the test must FAIL.
python3 -I -B mutate_capstate.py   (prints survivors; exit 1 if any)"""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "capstate.py")
M = [
 ("boundary 15.0 -> 15.5", "BOUNDARY = 15.0 ", "BOUNDARY = 15.5 "),
 ("classify >= -> >", "else \"normal-batch\")\n", None),   # placeholder replaced below
 ("sampler cpus", 'SAMPLER_CPUS = "34,70"', 'SAMPLER_CPUS = "2,70"'),
 ("sample_s 80 -> 65", "SAMPLE_S = 80 ", "SAMPLE_S = 65 "),
 ("anchor window", "else (11, 10 + WINDOW_S)", "else (1, WINDOW_S)"),
 ("anchor t_m ignores warm-up", 'warm = float(cmd[cmd.index("--warmup") + 1]) if anchor else 0.0', "warm = 0.0"),
 ("is_measure > 0 -> >= 0", 'float(cmd[cmd.index("--seconds") + 1]) > 0)', 'float(cmd[cmd.index("--seconds") + 1]) >= 0)'),
 ("sudo -n dropped", '["sudo", "-n"] + argv', '["sudo"] + argv'),
 ("separation ignores within-range", "separates=gap > 0 and gap >= within", "separates=gap > 0"),
 ("P1a inverted", 'wsum(s, "msr/aperf/", lo, hi) / wsum(s, "msr/mperf/", lo, hi))', 'wsum(s, "msr/mperf/", lo, hi) / wsum(s, "msr/aperf/", lo, hi))'),
 ("P5a kB not bytes", "kb * 1024 / pck", "kb / pck"),
 ("instrument validity 3% -> 50%", "<= 0.03", "<= 0.5"),
 ("mid snapshot at +20", "(tm + 30, \"mid\"", "(tm + 20, \"mid\""),
 ("failed sampler tolerated", "        if rc != 0:\n            problems", "        if False:\n            problems"),
 ("ORDER patch dropped", "    a2a.ORDER, a2a.PHASE = ORDER, PHASE\n", "    pass\n"),
 ("sudoers unescape dropped", 're.sub(r"\\\\(.)", r"\\1", cmd)', "cmd"),
 ("numa node 1 -> 0", 'n1 += int(n) if node == "1" else 0', 'n1 += int(n) if node == "0" else 0'),
 ("window hi exclusive", "if lo <= i <= hi)\n\n\ndef wdt", "if lo <= i < hi)\n\n\ndef wdt"),
 ("abort swallowed as complete", 'res["complete"] = "aborted" not in res and', 'res["complete"] = True or'),
 ("anchors classified", 'r["slot"].startswith("C")]', 'r["slot"][0] in "CS"]'),
 ("shim intercepts load step", "        if is_measure(cmd):\n            name", "        if True:\n            name"),
]
M[1] = ("classify >= -> >", '("large-batch" if frames_recv >= BOUNDARY', '("large-batch" if frames_recv > BOUNDARY')
orig = open(SRC).read()
surv = []
try:
    for name, old, new in M:
        if orig.count(old) != 1:
            print(f"MUTANT NOT APPLICABLE ({orig.count(old)} matches): {name}")
            surv.append(name)
            continue
        open(SRC, "w").write(orig.replace(old, new))
        r = subprocess.run([sys.executable, "-I", "-B", os.path.join(HERE, "test_capstate.py")], capture_output=True, text=True, timeout=300)
        caught = r.returncode != 0
        print(("caught   " if caught else "SURVIVED ") + name)
        if not caught:
            surv.append(name)
finally:
    open(SRC, "w").write(orig)
print(f"{len(M) - len(surv)}/{len(M)} mutants caught")
sys.exit(1 if surv else 0)
