"""The crash-point matrix of prereg section 5 (revision 3: 128 points)."""

import hashlib

COMMITS = [
    "C-acc",
    "I-prep",
    "I-disp",
    "I-acc",
    "I-out",
    "P-dec",
    "TI-prep",
    "TI-disp",
    "TI-out",
    "TF-prep",
    "TF-disp",
    "TF-out",
    "TN-prep",
    "TN-disp",
    "TN-out",
    "M-done",
]
SENDS = ["model", "T-I", "T-F", "T-N"]
DISP = ["I-disp", "TI-disp", "TF-disp", "TN-disp"]
PREP = ["I-prep", "TI-prep", "TF-prep", "TN-prep"]
OUT = ["I-out", "TI-out", "TF-out", "TN-out"]


def points():
    p = []
    for c in COMMITS:
        for a in ("a1", "a2", "a3", "a4"):
            p.append({"id": f"A/{a}:{c}", "kind": "A", "at": f"{a}:{c}", "scenario": "S-M"})
    for s in SENDS:
        for b in ("b1", "b2", "b3"):
            p.append({"id": f"B/{b}:{s}", "kind": "B", "at": f"{b}:{s}", "scenario": "S-M"})
    for c in DISP:
        p.append({"id": f"C-a1/{c}", "kind": "C-a1", "at": f"a1:{c}", "scenario": "S-M"})
    for s in SENDS:
        p.append({"id": f"C-b2/{s}", "kind": "C-b2", "at": f"b2:{s}", "scenario": "S-M"})
    for c in PREP:
        p.append({"id": f"C-p/{c}", "kind": "C-p", "at": f"a4:{c}", "scenario": "S-M"})
    for c in OUT:
        p.append({"id": f"C-s/{c}", "kind": "C-s", "at": f"a4:{c}", "scenario": "S-M"})
    for c in PREP:
        p.append({"id": f"D/a4:{c}", "kind": "D", "at": f"a4:{c}", "scenario": "S-M"})
    for s in SENDS[1:]:
        for b in ("b1", "b2", "b3"):
            p.append({"id": f"D/{b}:{s}", "kind": "D", "at": f"{b}:{s}", "scenario": "S-M"})
    for c in DISP:
        p.append({"id": f"E/immediate:{c}", "kind": "E", "at": f"a3:{c}", "mode": "immediate", "scenario": "S-M"})
    p.append({"id": "E/fast:TI-disp", "kind": "E", "at": "a3:TI-disp", "mode": "fast", "scenario": "S-M"})
    p.append({"id": "R/takeover-commit", "kind": "R-takeover", "at": "a4:C-acc", "scenario": "S-M"})
    p.append({"id": "R/I-unknown", "kind": "R-two", "at": "b2:model", "second": "a1:I-unknown", "scenario": "S-M"})
    p.append({"id": "R/TN-indoubt", "kind": "R-two", "at": "b2:T-N", "second": "a1:TN-indoubt", "scenario": "S-M"})
    p.append({"id": "R/takeover-outstanding", "kind": "R-takeover", "at": "b2:T-N", "scenario": "S-M"})
    for c in ("AP-req", "AP-dec"):
        for a in ("a1", "a2", "a3", "a4"):
            p.append({"id": f"AP/{a}:{c}", "kind": "A", "at": f"{a}:{c}", "scenario": "S-AP"})
    p.append({"id": "AP/cancel", "kind": "D", "at": "a4:AP-req", "scenario": "S-AP"})
    for c in DISP:
        p.append({"id": f"Ac/{c}", "kind": "Ac", "at": f"a3:{c}", "scenario": "S-M"})
    p.append({"id": "P/pre-policy", "kind": "P", "at": "pre-policy", "scenario": "S-M"})
    return p


def release(point_id, seed):
    if not point_id.startswith("C-b2/"):
        return None
    if (seed >> 1) & 1:
        return "before"
    return "after-outcome" if point_id in ("C-b2/T-F", "C-b2/T-N") and (seed >> 2) & 1 else "after"


def seed(point_id, i, prefix="bringup"):
    return int(hashlib.sha256(f"{prefix}:{point_id}:{i}".encode()).hexdigest()[:8], 16)


assert len(points()) == 128
