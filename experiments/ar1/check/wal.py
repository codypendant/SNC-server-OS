import os
import re
import subprocess
from pathlib import Path

from check.oracle import lsn_int

BLCKSZ, SHORT, LONG = 8192, 24, 40
REC = re.compile(
    r"rmgr: (\S+)\s+len \(rec/tot\):\s*(\d+)/\s*(\d+), tx:\s*(\d+), lsn: ([0-9A-F]+/[0-9A-F]+), prev \S+, desc: (\S+)"
)
SEG_NAME = re.compile(r"^[0-9A-F]{24}$")


def lsn_str(n):
    return f"{n >> 32:X}/{n & 0xFFFFFFFF:X}"


class Geometry:
    def __init__(self, seg=16 << 20):
        self.seg = seg
        self.upage = BLCKSZ - SHORT
        self.useg = seg // BLCKSZ * self.upage - (LONG - SHORT)

    def bytepos(self, ptr):
        segs, off = divmod(ptr, self.seg)
        pages, off = divmod(off, BLCKSZ)
        r = segs * self.useg
        if pages == 0:
            return r + (off - LONG if off else 0)
        return r + (BLCKSZ - LONG) + (pages - 1) * self.upage + (off - SHORT if off else 0)

    def end_ptr(self, b):
        segs, left = divmod(b, self.useg)
        if left < BLCKSZ - LONG:
            off = left + LONG if left else 0
        else:
            left -= BLCKSZ - LONG
            pages, left = divmod(left, self.upage)
            off = BLCKSZ + pages * BLCKSZ + (left + SHORT if left else 0)
        return segs * self.seg + off

    def rec_end(self, start, tot):
        # The commit's XactLastRecEnd (what SyncRep waits for): differs from the next record's start at page boundaries.
        return self.end_ptr(self.bytepos(start) + ((tot + 7) & ~7))


def segments(waldir):
    by_tli = {}
    for f in sorted(os.listdir(waldir)):
        if SEG_NAME.match(f):
            by_tli.setdefault(int(f[:8], 16), []).append(f)
    return by_tli


def history(waldir):
    sw = {}
    for f in os.listdir(waldir):
        if f.endswith(".history"):
            for line in (Path(waldir) / f).read_text().splitlines():
                p = line.split()
                if len(p) >= 2 and p[0].isdigit():
                    sw[int(p[0])] = lsn_int(p[1])
    return sw


def dump(waldir, pgbin):
    """{tli: [(start, end, next_start, rmgr, xid, desc)]} over every segment in waldir."""
    out = {}
    segs = segments(waldir)
    for tli, files in segs.items():
        size = os.path.getsize(Path(waldir) / files[0])
        g = Geometry(size)
        r = subprocess.run(
            [str(Path(pgbin) / "pg_waldump"), "-p", str(waldir), files[0], files[-1]], capture_output=True, text=True
        )
        recs = []
        for m in REC.finditer(r.stdout):
            start = lsn_int(m[5])
            recs.append([start, g.rec_end(start, int(m[3])), None, m[1], int(m[4]), m[6]])
        for a, b in zip(recs, recs[1:]):
            a[2] = b[0]
        out[tli] = [tuple(x) for x in recs]
    return out


def commits(recs_by_tli):
    """{xid32: [(tli, start, end)]} for COMMIT records; aborts separately."""
    c, a = {}, {}
    for tli, recs in recs_by_tli.items():
        for start, end, _, rmgr, xid, desc in recs:
            if rmgr == "Transaction" and desc == "COMMIT":
                c.setdefault(xid, []).append((tli, start, end))
            elif rmgr == "Transaction" and desc == "ABORT":
                a.setdefault(xid, []).append((tli, start, end))
    return c, a


def xid32(x):
    return int(x) & 0xFFFFFFFF
