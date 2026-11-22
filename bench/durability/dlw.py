#!/usr/bin/env python3
"""Durable-acknowledgement check through the block layer
(docs/DIRECTION_COMPARISON_PLAN.md 4.1 and its erratum).

A loop device (write back, fua 0) sits under dm-log-writes, which logs every
write once a flush covers it, or when a FUA write completes. The file system
is made through the logged device. While a workload runs, marks go into the
log every --interval seconds, each with its CLOCK_MONOTONIC time. For each
mark, the log is replayed from its start through the mark onto a fresh image,
which is the disk after losing every volatile cache at that instant; a check
command then decides whether every write acknowledged before the mark is
there.

  dlw.py controls        positive (O_DIRECT + fdatasync) must pass, negative
                         (O_DIRECT only) must fail; exit 0 iff both behave
  dlw.py run -- CMD...   run CMD with $DLW_MNT as its data mount, then check
                         each mark with --check (it gets $DLW_MNT, $DLW_MARK_NS)

Needs root, dmsetup, the dm-log-writes module and replay-log (xfstests
src/log-writes, built by --build-replay-log).
"""
import argparse
import json
import mmap
import os
import shutil
import struct
import subprocess
import sys
import time

DM = "dlw"
BLOCK = 4096


def sh(*cmd, check=True):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f"{' '.join(cmd)}: {r.stderr.strip() or r.stdout.strip()}")
    return r.stdout.strip()


def now_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


class Rig:
    def __init__(self, work, size_mb, replay_log):
        self.work, self.size_mb, self.replay_log = work, size_mb, replay_log
        self.mnt = os.path.join(work, "mnt")
        self.loops = []

    def loop(self, name, size_mb):
        path = os.path.join(self.work, name)
        with open(path, "wb") as f:
            f.truncate(size_mb << 20)
        dev = sh("losetup", "--find", "--show", path)
        self.loops.append(dev)
        return dev, path

    def up(self):
        os.makedirs(self.mnt, exist_ok=True)
        self.data, _ = self.loop("data.img", self.size_mb)
        self.log, _ = self.loop("log.img", self.size_mb * 4)
        q = f"/sys/block/{os.path.basename(self.data)}/queue"
        self.queue = {k: open(f"{q}/{k}").read().strip() for k in ("write_cache", "fua")}
        sectors = sh("blockdev", "--getsz", self.data)
        sh("dmsetup", "create", DM, "--table", f"0 {sectors} log-writes {self.data} {self.log}")
        sh("mkfs.ext4", "-q", "-E", "nodiscard", f"/dev/mapper/{DM}")
        self.mark("mkfs")
        sh("mount", f"/dev/mapper/{DM}", self.mnt)

    def mark(self, name):
        t = now_ns()
        sh("dmsetup", "message", DM, "0", "mark", name)
        return t

    def stop_logging(self):
        sh("umount", self.mnt, check=False)
        sh("dmsetup", "remove", DM, check=False)

    def replay(self, mark, check_env, check_cmd):
        """Replays through `mark` onto a fresh image and runs the check on it."""
        img = os.path.join(self.work, "replay.img")
        with open(img, "wb") as f:
            f.truncate(self.size_mb << 20)
        dev = sh("losetup", "--find", "--show", img)
        try:
            sh(self.replay_log, "--log", self.log, "--replay", dev, "--end-mark", mark)
            mnt = os.path.join(self.work, "replay")
            os.makedirs(mnt, exist_ok=True)
            sh("mount", dev, mnt)
            try:
                env = dict(os.environ, DLW_MNT=mnt, **check_env)
                return subprocess.run(check_cmd, env=env).returncode == 0
            finally:
                sh("umount", mnt, check=False)
        finally:
            sh("losetup", "-d", dev, check=False)
            os.remove(img)

    def down(self):
        self.stop_logging()
        for d in self.loops:
            sh("losetup", "-d", d, check=False)


def marks_while(rig, proc, n, interval):
    out = []
    time.sleep(interval)
    while len(out) < n and proc.poll() is None:
        out.append((f"m{len(out)}", rig.mark(f"m{len(out)}")))
        time.sleep(interval)
    return out


def writer(path, blocks, sync, acks):
    """Overwrites blocks of a pre-written file with O_DIRECT, logging each ack."""
    buf = mmap.mmap(-1, BLOCK)
    fd = os.open(path, os.O_WRONLY | os.O_DIRECT)
    with open(acks, "w") as log:
        k = 0
        while True:
            buf.seek(0)
            buf.write(struct.pack("<QQ", 0x444C57, k))
            os.pwrite(fd, buf, (k % blocks) * BLOCK)
            if sync:
                os.fdatasync(fd)
            log.write(f"{k} {now_ns()}\n")
            log.flush()
            k += 1


def check_writer(path, blocks, acks, mark_ns):
    acked = {}
    for line in open(acks):
        k, t = map(int, line.split())
        if t < mark_ns:
            acked[k % blocks] = max(acked.get(k % blocks, -1), k)
    data = open(path, "rb").read()
    lost = 0
    for b, k in acked.items():
        magic, got = struct.unpack_from("<QQ", data, b * BLOCK)
        lost += magic != 0x444C57 or got < k
    return lost


def controls(args):
    rig = Rig(args.work, args.size_mb, args.replay_log)
    result = {}
    try:
        for name, sync in (("positive", True), ("negative", False)):
            rig.up()
            path = os.path.join(rig.mnt, "w.dat")
            with open(path, "wb") as f:
                f.write(b"\0" * BLOCK * args.blocks)
                f.flush()
                os.fsync(f.fileno())
            acks = os.path.join(args.work, f"{name}.acks")
            p = subprocess.Popen([sys.executable, __file__, "_writer", path, str(args.blocks), str(int(sync)), acks])
            marks = marks_while(rig, p, args.marks, args.interval)
            p.kill()
            p.wait()
            rig.stop_logging()
            failed = 0
            for mark, t in marks:
                env = {"DLW_MARK_NS": str(t)}
                ok = rig.replay(mark, env, [sys.executable, __file__, "_check", "w.dat", str(args.blocks), acks])
                failed += not ok
            result[name] = {"marks": len(marks), "failed": failed, "queue": rig.queue}
            rig.down()
            rig.loops = []
    finally:
        rig.down()
    good = result["positive"]["failed"] == 0 and result["negative"]["failed"] > 0
    print(json.dumps(dict(result, controls="ok" if good else "INVALID")))
    return 0 if good else 1


def run(args):
    rig = Rig(args.work, args.size_mb, args.replay_log)
    try:
        rig.up()
        p = subprocess.Popen(args.cmd, env=dict(os.environ, DLW_MNT=rig.mnt))
        marks = marks_while(rig, p, args.marks, args.interval)
        p.terminate()
        p.wait()
        rig.stop_logging()
        failed = [m for m, t in marks if not rig.replay(m, {"DLW_MARK_NS": str(t)}, args.check.split())]
        print(json.dumps({"marks": len(marks), "failed": failed, "queue": rig.queue}))
        return 0 if marks and not failed else 1
    finally:
        rig.down()


def build_replay_log(dest):
    """xfstests src/log-writes at a pinned commit (docs/research/TRACKB_PREREQUISITES.md)."""
    base = "https://raw.githubusercontent.com/kdave/xfstests/d07dca72/src/log-writes/"
    src = os.path.join(os.path.dirname(dest) or ".", "log-writes-src")
    os.makedirs(src, exist_ok=True)
    for f in ("replay-log.c", "log-writes.c", "log-writes.h"):
        sh("curl", "-fsSL", "-o", os.path.join(src, f), base + f)
    sh("gcc", "-O2", "-o", dest, os.path.join(src, "replay-log.c"), os.path.join(src, "log-writes.c"))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "_writer":
        writer(sys.argv[2], int(sys.argv[3]), sys.argv[4] == "1", sys.argv[5])
        return 0
    if len(sys.argv) > 1 and sys.argv[1] == "_check":
        path = os.path.join(os.environ["DLW_MNT"], sys.argv[2])
        return 1 if check_writer(path, int(sys.argv[3]), sys.argv[4], int(os.environ["DLW_MARK_NS"])) else 0
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["controls", "run"])
    ap.add_argument("--work", default="/var/tmp/dlw")
    ap.add_argument("--size-mb", type=int, default=512)
    ap.add_argument("--marks", type=int, default=20)
    ap.add_argument("--interval", type=float, default=0.5)
    ap.add_argument("--blocks", type=int, default=256)
    ap.add_argument("--replay-log", default="replay-log")
    ap.add_argument("--build-replay-log", action="store_true")
    ap.add_argument("--check", default="")
    ap.add_argument("cmd", nargs="*")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    if args.build_replay_log:
        args.replay_log = os.path.join(args.work, "replay-log")
        build_replay_log(args.replay_log)
    rc = controls(args) if args.mode == "controls" else run(args)
    shutil.rmtree(args.work, ignore_errors=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
