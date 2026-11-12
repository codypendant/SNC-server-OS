#!/usr/bin/env python3
"""Revision 4 (device identity) mutation suite. Each mutant is a one-place textual change of a2a.py
(or of sudoers-snc-a2a), applied in a throwaway copy of the package under a temp tree that symlinks the
rest of the repository; test_a2a.py must FAIL on it (rc != 0). The baseline (no mutation) must pass.

  python3 -B negative/run_identity_mutants.py OUTDIR      # writes OUTDIR/<name>.log and OUTDIR/summary-rev4.txt

No process is started and nothing is trimmed (test_a2a.py is hermetic)."""
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.dirname(HERE)
REPO = os.path.realpath(os.path.join(PKG, "..", "..", ".."))
NAME = os.path.basename(PKG)

ID_LOOP = 'for k in ("fs_uuid", "model", "serial", "wwn"):'
M = [  # (name, file, old, new)
    ("identity-uuid-ignored", "a2a.py", ID_LOOP, 'for k in ("model", "serial", "wwn"):'),
    ("identity-model-ignored", "a2a.py", ID_LOOP, 'for k in ("fs_uuid", "serial", "wwn"):'),
    ("identity-serial-ignored", "a2a.py", ID_LOOP, 'for k in ("fs_uuid", "model", "wwn"):'),
    ("identity-wwn-ignored", "a2a.py", ID_LOOP, 'for k in ("fs_uuid", "model", "serial"):'),
    ("identity-mount-ignored", "a2a.py", 'if f.get("mount") != [MOUNT, MOUNT_FSTYPE, MOUNT_OPTIONS]:', "if False:"),
    ("identity-byid-disk-ignored", "a2a.py", 'if f.get("byid_disk") != f"/dev/{f[\'disk\']}":', "if False:"),
    ("identity-byid-part-ignored", "a2a.py", 'if f.get("byid_part") != f["source"]:', "if False:"),
    ("identity-byuuid-ignored", "a2a.py", 'if f.get("byuuid") != f["source"]:', "if False:"),
    ("identity-rename-unchecked", "a2a.py", "    if want is not None:\n        for k in", "    if False:\n        for k in"),
    ("identity-unresolved-ok", "a2a.py", 'if not f.get("disk") or not f.get("source"):', "if False:"),
    ("check-identity-not-raised", "a2a.py", "    if bad:\n        raise Incomplete(\"device identity: \"", "    if False:\n        raise Incomplete(\"device identity: \""),
    ("guard-identity-ignored", "a2a.py", '    p += identity_problems(ident, want.get("identity"))', "    pass"),
    ("guard-identity-no-start-reference", "a2a.py", 'identity_problems(ident, want.get("identity"))', "identity_problems(ident)"),
    ("guard-data-disk-ignored", "a2a.py", '    if o.get("data_disks") != [ident.get("disk")]:', "    if False:"),
    ("recheck-after-trim-removed", "a2a.py", '    check_identity(want["identity"])            # the counters above', "    pass  # the counters above"),
    ("recheck-after-settle-removed", "a2a.py", '    check_identity(want["identity"])            # and the settle', "    pass  # and the settle"),
    ("recheck-before-end-removed", "a2a.py", '    check_identity(want["identity"])\n    rec["end"]', '    rec["end"]'),
    ("diskstats-fixed-name-sda", "a2a.py", "f[2] == dev:", 'f[2] == "sda":'),
    ("diskstats-fixed-name-sdb", "a2a.py", "f[2] == dev:", 'f[2] == "sdb":'),
    ("diskstats-first-line", "a2a.py", "f[2] == dev:", "True:"),
    ("disk-is-partition-name", "a2a.py", 'f["disk"] = f["part"] if parent == "block" else parent', 'f["disk"] = f["part"]'),
    ("disk-is-first-letter-sdb", "a2a.py", 'f["disk"] = f["part"] if parent == "block" else parent', 'f["disk"] = "sdb"'),
    ("root-smartctl-kernel-name", "a2a.py", '"/usr/sbin/smartctl", "-x", BYID_DISK]', '"/usr/sbin/smartctl", "-x", "/dev/sdb"]'),
    ("root-tune2fs-kernel-name", "a2a.py", '"/usr/sbin/tune2fs", "-l", BYID_PART]', '"/usr/sbin/tune2fs", "-l", "/dev/sdb1"]'),
    ("byid-wwn-typo", "a2a.py", 'BYID_DISK = "/dev/disk/by-id/wwn-0x5002538b097f68a0"', 'BYID_DISK = "/dev/disk/by-id/wwn-0x5002538b097f68a1"'),
    ("approved-serial-typo", "a2a.py", 'serial="S48LNA0M700325"', 'serial="S48LNA0M700326"'),
    ("sudoers-kernel-name", "sudoers-snc-a2a", "/usr/sbin/smartctl -x /dev/disk/by-id/wwn-0x5002538b097f68a0", "/usr/sbin/smartctl -x /dev/sdb"),
    ("sudoers-part-kernel-name", "sudoers-snc-a2a", "-l /dev/disk/by-id/wwn-0x5002538b097f68a0-part1", "-l /dev/sdb1"),
    ("sudoers-sgl-by-id-typo", "sudoers-snc-a2a", "/usr/bin/sg_logs -a /dev/disk/by-id/wwn-0x5002538b097f68a0,", "/usr/bin/sg_logs -a /dev/disk/by-id/wwn-0x5002538b097f68a1,"),
]


def tree(mutation=None):
    t = tempfile.mkdtemp(prefix="a2a-mut-")
    os.makedirs(os.path.join(t, "bench", "results", NAME))
    for e in os.listdir(os.path.join(REPO, "bench")):
        if e != "results":
            os.symlink(os.path.join(REPO, "bench", e), os.path.join(t, "bench", e))
    for e in os.listdir(os.path.join(REPO, "bench", "results")):
        if e != NAME:
            os.symlink(os.path.join(REPO, "bench", "results", e), os.path.join(t, "bench", "results", e))
    os.symlink(os.path.join(REPO, "tests"), os.path.join(t, "tests"))
    for f in ("a2a.py", "test_a2a.py", "sudoers-snc-a2a"):
        shutil.copy(os.path.join(PKG, f), os.path.join(t, "bench", "results", NAME, f))
    if mutation:
        _, f, old, new = mutation
        path = os.path.join(t, "bench", "results", NAME, f)
        s = open(path).read()
        assert s.count(old) == 1, (mutation[0], s.count(old))
        open(path, "w").write(s.replace(old, new))
    return t


def test(t):
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    p = subprocess.run([sys.executable, "-B", os.path.join(t, "bench", "results", NAME, "test_a2a.py")],
                       capture_output=True, text=True, env=env, cwd=t)
    return p.returncode, p.stdout + p.stderr


def main(out):
    os.makedirs(out, exist_ok=True)
    t = tree()
    rc, txt = test(t)
    shutil.rmtree(t)
    assert rc == 0, "baseline must pass:\n" + txt[-2000:]
    lines = ["# revision 4 (device identity) mutations of a2a.py / sudoers-snc-a2a; rc!=0: test_a2a.py caught it; "
             "after = last check group that passed"]
    caught = 0
    for m in M:
        t = tree(m)
        rc, txt = test(t)
        shutil.rmtree(t)
        ok = [g for g in ("A1", "A2", "A3", "A4", "A5") if f"{g} ok" in txt]
        lines.append(f"{m[0]} rc={rc} after={ok[-1] if ok else 'none'}")
        open(os.path.join(out, m[0] + ".log"), "w").write("\n".join(txt.splitlines()[-8:]) + "\n")
        caught += rc != 0
    lines.append(f"# {caught} / {len(M)} caught on the first pass (baseline passes)")
    open(os.path.join(out, "summary-rev4.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if caught == len(M) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
