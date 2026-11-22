import os
import random
import time
from pathlib import Path

from . import jlog

R = Path(os.environ["AR1_RUN"]) if os.environ.get("AR1_RUN") else None
POINTS = set(filter(None, os.environ.get("AR1_POINTS", "").split(",")))
SEED = int(os.environ.get("AR1_SEED", "0"))
fired_here = []


def marker(event, point=None, **data):
    if R:
        jlog.append(R / "markers.jsonl", {"event": event, "point": point, "data": data})


def _fired(name):
    return (R / "ctl" / f"fired-{name}").exists()


def at(name):
    if not R or name not in POINTS or _fired(name):
        return
    (R / "ctl" / f"fired-{name}").write_text(str(os.getpid()))
    time.sleep(random.Random(f"{SEED}:{name}").uniform(0, 0.1))
    marker("reached", name)
    fired_here.append(name)
    rel = R / "ctl" / f"release-{name}"
    while not rel.exists():
        time.sleep(0.002)


def stale_armed():
    return R and "stale-decision" in POINTS and fired_here and not _fired("stale-decision")


def stale_begin(epoch, decision):
    if stale_armed():
        (R / "ctl" / "fired-stale-decision").write_text(str(os.getpid()))
        marker("stale-decision-attempt", "stale-decision", epoch=epoch, decision=decision)
        return True
    return False


def stale_end(outcome, reason=None, xid=None):
    marker(f"stale-decision-{outcome}", "stale-decision", reason=reason, xid=xid)
