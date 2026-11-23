"""Determinism divergence (prereg 8, reported, not gated): python -m ar1.diverge <arm> <seed> <out.json>.

A b3 crash at T-I, then recovery by a copy of the arm whose T-I and T-F steps are swapped, DBOS__APPVERSION
unchanged. Records whether DBOS reports the change, re-executes silently, or fails.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from . import harness, jlog

SWAP = {
    "snc": (
        "rt.py",
        'TOOLS = [("T-I", "TI"), ("T-F", "TF"), ("T-N", "TN")]',
        'TOOLS = [("T-F", "TF"), ("T-I", "TI"), ("T-N", "TN")]',
    ),
    "plain1": ("app.py", 'for tool in ("T-I", "T-F", "T-N"):', 'for tool in ("T-F", "T-I", "T-N"):'),
    "plain2": ("app.py", 'for tool in ("T-I", "T-F", "T-N"):', 'for tool in ("T-F", "T-I", "T-N"):'),
}
POINT = "B/b3:T-I"


class Diverged(harness.Run):
    def __init__(self, arm, seed, shadow):
        super().__init__(POINT, seed, "", arm)
        self.shadow = shadow

    def spawn(self, executor, mode, epoch, points=()):
        if mode != "recover":
            return super().spawn(executor, mode, epoch, points)
        env = self.env(executor, points)
        env["PYTHONPATH"] = f"{self.shadow}:{env['PYTHONPATH']}"
        p = subprocess.Popen(
            [harness.PY, "-m", f"{self.pkg}.app", mode, str(epoch)],
            env=env,
            stdout=open(self.R / f"app-{executor}.log", "a"),
            stderr=subprocess.STDOUT,
            cwd=self.shadow,
        )
        self.procs[executor] = p
        return p


def main():
    arm, seed, out = sys.argv[1], int(sys.argv[2]), Path(sys.argv[3])
    pkg = harness.ARMS[arm]
    f, old, new = SWAP[arm]
    shadow = Path(tempfile.mkdtemp(prefix="ar1-diverge-"))
    shutil.copytree(harness.HERE / pkg, shadow / pkg, ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copy(harness.HERE / "policy.json", shadow)
    src = (shadow / pkg / f).read_text()
    if src.count(old) != 1:
        raise SystemExit(f"swap target not found once in {pkg}/{f}")
    (shadow / pkg / f).write_text(src.replace(old, new))
    (shadow / pkg / "__init__.py").write_text(
        "import os, pathlib\npathlib.Path(os.environ['AR1_RUN'], 'ctl', 'shadow-' + os.environ['AR1_EXECUTOR']).touch()\n"
    )
    os.environ["AR1_KEEP"] = "1"
    r = Diverged(arm, seed, shadow)
    res = r.execute()
    ctl = r.R / "ctl"
    res["shadow_loaded"] = sorted(p.name for p in ctl.glob("shadow-*"))
    log = (r.R / "app-e1.log").read_text(errors="replace") if (r.R / "app-e1.log").exists() else ""
    res["dbos_reported"] = [l for l in log.splitlines() if "DBOSUnexpectedStep" in l or "nondetermin" in l.lower()][:5]
    res["log_tail"] = log[-3000:]
    res["tool_entries"] = {
        s: [{k: e.get(k) for k in ("kind", "call_id", "key", "attempt")} for e in jlog.read(r.R / "svc" / f"{s}.jsonl")]
        for s in ("T-I", "T-F", "T-N")
    }
    out.write_text(json.dumps(res, indent=1))
    shutil.rmtree(shadow, ignore_errors=True)
    shutil.rmtree(r.R, ignore_errors=True)


if __name__ == "__main__":
    main()
