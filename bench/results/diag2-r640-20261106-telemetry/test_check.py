#!/usr/bin/env python3
"""After test_fail_closed and test_sync_readiness failed under ctest -j 8 on build-diag: run both one at
a time on build-pilot and build-diag. Proceed to diag2.py only if no scenario fails on build-diag alone."""
import re, subprocess, sys
OUT = sys.argv[1]
def clear():
    subprocess.run(["pkill", "-f", r"build-(pilot|diag)/snc_(server|witness)"])
def run(build):
    r = subprocess.run(["ctest", "--test-dir", build, "-R", "^(test_fail_closed|test_sync_readiness)$", "-j", "1", "--timeout", "900"], capture_output=True, text=True)
    last = open(f"{build}/Testing/Temporary/LastTest.log", errors="replace").read()
    open(f"{OUT}/testcheck-{build}.log", "w").write(r.stdout + r.stderr + "\n===== LastTest.log\n" + last)
    fails = set(re.findall(r"^(\S+): FAILED", last, re.M)) | set(re.findall(r"^FAIL (\S+)", last, re.M))
    if r.returncode and not fails: fails = {"unidentified:" + build}
    print(build, "exit", r.returncode, "failed scenarios", sorted(fails), flush=True)
    clear()
    return fails
clear()
p = run("build-pilot"); d = run("build-diag")
diag_only = d - p
verdict = f"build-pilot fails {sorted(p)}; build-diag fails {sorted(d)}; on build-diag alone {sorted(diag_only)}"
proceed = not diag_only
open(f"{OUT}/TEST_CHECK.txt", "w").write(verdict + "\nRule, approved by the user: proceed unless a scenario fails on build-diag alone.\nProceed: " + str(proceed) + "\nThe proof then runs with diag2.py --skip-ctest. The full suite was not rerun.\n")
print(verdict, flush=True)
if not proceed:
    print("STOP: a test fails on build-diag alone; no benchmark run", flush=True); sys.exit(1)
sys.exit(subprocess.run([sys.executable, "-u", f"{OUT}/diag2.py", OUT, "--skip-ctest"]).returncode)
