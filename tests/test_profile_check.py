#!/usr/bin/env python3
"""T-H (docs/EXPERIMENTAL_BASELINE.md section 5): bench/profile_check.py
refuses each invalid profile combination, and a valid profile passes,
including the command lines bench/lab/lab.py builds for Profile B.

Usage: test_profile_check.py [BUILD_DIR]   (the build is not used)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "bench"))
from profile_check import (Guard, ProfileViolation, WitnessStateGuard,  # noqa: E402
                           WitnessStateViolation, state_marks, state_versions)
import shutil  # noqa: E402
import tempfile  # noqa: E402

SRV, WIT, CTL = "snc_server", "snc_witness", "snc_ctl"


def b_primary(d="/r/n1", shards=4, extra=()):
    return [SRV, "--shards", shards, "--objects", 1000000, "--data-dir", d, "--control", "h:1,h:2,h:3",
            "--node-id", 1, "--admin-port", 21140, "--witness", "h:1,h:2,h:3", "--replica-of", "h2:21170",
            "--replica-timeout-ms", 2000, "--checkpoint-s", 60] + list(extra)


def b_standby(d="/r/n2", shards=4, extra=()):
    return [SRV, "--shards", shards, "--objects", 1000000, "--data-dir", d, "--control", "h:1,h:2,h:3",
            "--node-id", 2, "--admin-port", 21180, "--witness", "h:1,h:2,h:3", "--standby", 21210,
            "--checkpoint-s", 60] + list(extra)


PEERS = "h1:21000,h2:21000,h3:21000"


def witness(extra=(), new_state=True, state="/r/w0.state", wid=0, peers=PEERS):
    """A Profile B witness command line; new_state: its first start."""
    return [WIT, "--port", 21000, "--state", state, "--id", wid, "--peers", peers] + (
        ["--new-state", 1] if new_state else []) + [
            "--failover-ms", 1000, "--poll-ms", 100, "--group", "0=h1:21140,h2:21180"] + list(extra)


def setf(argv, flag, value=None):
    """argv with flag set to value (replaced, or appended), or removed if
    value is None."""
    a = list(argv)
    if flag in a:
        i = a.index(flag)
        del a[i:i + 2]
    return a if value is None else a + [flag, value]


def a_node(d="/r/n1", extra=()):
    return [SRV, "--shards", 4, "--data-dir", d, "--checkpoint-s", 60] + list(extra)


CASES = []


def case(fn):
    CASES.append(fn)
    return fn


def refused(why, f, *a, **kw):
    try:
        f(*a, **kw)
    except ProfileViolation as e:
        assert why in str(e), (why, str(e))
        return str(e)
    raise AssertionError(f"accepted, expected refusal: {why}")


@case
def valid_b():
    g = Guard("B")
    g.witness(witness())
    g.ctl_init([CTL, "--control", "h:1", "--placement-v2", "init", "--nodes", "x", "--groups", "y"])
    g.server(b_standby())
    g.server(b_primary())
    # A restart of the same directory with the same shards, and a rejoin.
    g.server(b_primary(extra=["--rejoin", "21170,h2:21180,h2:21190", "--seed-port", 21190, "--space-uuid", "ab" * 16]))
    assert g.report()["deviations_made"] == [], g.report()


@case
def valid_a_c_d():
    Guard("A").server(a_node())
    Guard("A").witness([WIT, "--port", 1, "--state", "s", "--id", 0, "--peers", "h:1"])  # placement only
    Guard("C").server([SRV, "--shards", 4, "--objects", 100])
    d = Guard("D")
    d.server([SRV, "--shards", 4, "--data-dir", "/r/n1", "--checkpoint-s", 60, "--replica-of", "h:1",
              "--async-replica"])
    d.server([SRV, "--shards", 4, "--data-dir", "/r/n2", "--checkpoint-s", 60, "--standby", 1])


@case
def async_with_witness():
    refused("async replication with witness HA", Guard("B").server, b_primary(extra=["--async-replica"]))
    refused("async replication with witness HA", Guard("D").server,
            [SRV, "--shards", 4, "--data-dir", "/d", "--checkpoint-s", 60, "--replica-of", "h:1", "--async-replica",
             "--witness", "h:9"])
    refused("async with witness HA is refused", Guard("D").witness, witness())
    # Not even as a declared deviation.
    refused("async replication with witness HA", Guard("B", ["--async-replica"]).server,
            b_primary(extra=["--async-replica"]))


@case
def status_witness():
    refused("polling STATUS", Guard("B").witness, witness(["--health-verb", "status"]))
    refused("polling STATUS", Guard("B", ["--health-verb"]).witness, witness(["--health-verb", "status"]))


@case
def reused_data_dir():
    g = Guard("B")
    refused("already exists", g.server, b_primary(), dir_exists=lambda p: p == "/r/n1")
    # The run's own restart of a directory it started is not a reuse.
    g = Guard("B")
    g.server(b_primary(), dir_exists=lambda p: False)
    g.server(b_primary(), dir_exists=lambda p: True)


@case
def shard_change_on_restart():
    g = Guard("B")
    g.server(b_primary(shards=4))
    refused("shard-count change", g.server, b_primary(shards=8))
    g2 = Guard("B", ["--shards"])
    g2.server(b_primary(shards=4))
    refused("shard-count change", g2.server, b_primary(shards=8))


@case
def standby_reads_in_b():
    refused("standby reads", Guard("B").server, b_standby(extra=["--standby-reads", 500]))
    refused("standby reads", Guard("B", ["--standby-reads"]).server, b_standby(extra=["--standby-reads", 500]))


@case
def undeclared_deviations():
    refused("--checkpoint-s 5 instead of 60", Guard("B").server, setf(b_primary(), "--checkpoint-s", 5))
    # Absent: the binary's default (0) is not the profile's 60.
    refused("--checkpoint-s 0 instead of 60", Guard("B").server, setf(b_primary(), "--checkpoint-s"))
    refused("--move-units off instead of on", Guard("B").server, b_primary(extra=["--move-units", "off"]))
    refused("--debug-admin is not part of Profile B", Guard("B").server, b_primary(extra=["--debug-admin"]))
    refused("--group-commit-us 200", Guard("A").server, a_node(extra=["--group-commit-us", 200]))
    refused("--replica-timeout-ms 500", Guard("B").server, setf(b_primary(), "--replica-timeout-ms", 500))
    refused("witness --bogus", Guard("B").witness, witness(["--bogus", 1]))
    # Declared, they pass, and the report lists them.
    g = Guard("B", ["--checkpoint-s=5", "--debug-admin", "--move-units=off", "--spin"])
    g.server(setf(b_primary(extra=["--debug-admin", "--move-units", "off"]), "--checkpoint-s", 5))
    r = g.report()
    assert r["deviations_made"] == ["--checkpoint-s=5", "--debug-admin", "--move-units=off"], r
    assert r["deviations_declared_unused"] == ["--spin"], r
    # A declaration of another value does not cover this one.
    refused("--checkpoint-s 7 instead of 60", Guard("B", ["--checkpoint-s=5"]).server,
            setf(b_primary(), "--checkpoint-s", 7))


@case
def profile_shapes():
    refused("needs the witness gate", Guard("B").server, setf(b_primary(), "--witness"))
    refused("sync primary (--replica-of) or its standby", Guard("B").server, a_node())
    refused("placement is written as v2", Guard("B").ctl_init, [CTL, "--control", "h:1", "init"])
    refused("no replication", Guard("A").server, a_node(extra=["--replica-of", "h:1"]))
    refused("no data directory", Guard("C").server, a_node())
    refused("a data directory is required", Guard("A").server, [SRV, "--shards", 4, "--checkpoint-s", 60])
    refused("replication is async", Guard("D").server,
            [SRV, "--shards", 4, "--data-dir", "/d", "--checkpoint-s", 60, "--replica-of", "h:1"])
    refused("promote nothing", Guard("A").witness, witness())
    refused("--shards must be given", Guard("A").server, [SRV, "--data-dir", "/d", "--checkpoint-s", 60])
    refused("unknown profile", Guard, "E")


@case
def parsing_as_the_binary_does():
    # A flag followed by another flag takes it as its value in the binary:
    # refused, not misread as a switch (a gate or directory that looks
    # present while the binary never sees it).
    refused("--report-ms needs a value", Guard("B").server,
            setf(b_primary(), "--witness")[:1] + ["--report-ms", "--witness", "h:1"] + setf(b_primary(), "--witness")[1:])
    refused("--data-dir needs a value", Guard("B").server, setf(b_primary(), "--data-dir") + ["--data-dir"])
    refused("needs a value", Guard("B").server, setf(b_primary(), "--data-dir", ""))
    # Repeated flags: the binary keeps the last one.
    refused("--data-dir given twice", Guard("B").server, b_primary() + ["--data-dir", "/old"])
    refused("--shards given twice", Guard("B").server, b_primary() + ["--shards", 8])
    # A witness's --group repeats legitimately.
    Guard("B").witness(witness(["--group", "1=h3:1,h4:2"]))


@case
def same_directory_two_nodes():
    g = Guard("B")
    g.server(b_primary(d="/r/x"), host="h1")
    refused("would start on node 1's directory", g.server, b_standby(d="/r/x"), host="h1")
    # The same path on another host is another directory.
    g.server(b_standby(d="/r/x"), host="h2")


@case
def lenient_where_harmless():
    Guard("B", ["--standby-reads=0"]).server(b_standby(extra=["--standby-reads", 0]))  # lease off, declared
    Guard("B").server(setf(b_primary(), "--checkpoint-s", "060"))
    refused("--replica-timeout-ms 500", Guard("B").server, b_standby(extra=["--replica-timeout-ms", 500]))
    refused("placement is written as v2", Guard("B").ctl_init, [CTL, "--control", "h:1", "init", "--placement-v2"])


@case
def lab_profile_b():
    """The lab, with profile = "B", builds command lines the guard accepts."""
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), "bench", "lab"))
    import lab  # noqa: E402
    g = Guard("B")
    nodes = {1: {"group": 0, "member": 0, "admin": 21140, "repl": 21170}, 2: {"group": 0, "member": 1, "admin": 21180,
                                                                              "repl": 21210}}
    for k in (2, 1):
        args = lab.node_args(bin_path=SRV, shards=4, objects=1000000, data_dir=f"/r/n{k}", control="h:1,h:2,h:3",
                             node_id=k, admin=nodes[k]["admin"], cpus=None, standby_port=nodes[k]["repl"]
                             if nodes[k]["member"] else None, replica_of=None if nodes[k]["member"] else "h2:21210",
                             replica_timeout_ms=2000, profile_args=lab.profile_args("B", {}))
        g.server(args)
    g.witness(lab.witness_args(bin_path=WIT, port=21000, state="/r/w0.state", wid=0, control=PEERS,
                               failover_ms=1000, poll_ms=100, groups=["0=h1:21140,h2:21180"], new_state=True),
              state_exists=lambda path: False, host="h1")
    # The lab's restart of the same witness: no --new-state, same file, which exists.
    g.witness(lab.witness_args(bin_path=WIT, port=21000, state="/r/w0.state", wid=0, control=PEERS,
                               failover_ms=1000, poll_ms=100, groups=["0=h1:21140,h2:21180"]),
              state_exists=lambda path: True, host="h1")
    g.ctl_init([CTL, "--control", "h:1"] + lab.ctl_flags("B") + ["init"])


@case
def witness_identity_b():
    """H4/H5 at the campaign guard: three distinct endpoints, --id naming
    this witness's own port."""
    refused("three witnesses", Guard("B").witness, witness(peers="h1:21000,h2:21000"))
    refused("one endpoint twice", Guard("B").witness, witness(peers="h1:21000,h2:21000,h2:21000"))
    refused("one endpoint twice", Guard("B").witness, witness(peers="127.0.0.1:21000,localhost:21000,h3:21000"))
    refused("must name this witness's own --port", Guard("B").witness,
            witness(peers="h1:21001,h2:21000,h3:21000"))
    refused("must name this witness's own --port", Guard("B").witness, witness(wid=3))


@case
def witness_new_state_b():
    """G1 at the campaign guard: --new-state exactly on the first start, with
    the file absent; never on a restart; a restart on the same, present file."""
    refused("creates its state", Guard("B").witness, witness(new_state=False))
    refused("already exists at its first start", Guard("B").witness, witness(), state_exists=lambda p: True)
    g = Guard("B")
    g.witness(witness(), state_exists=lambda p: False, host="h1")
    refused("--new-state on a witness restart", g.witness, witness(), state_exists=lambda p: False, host="h1")
    refused("restarted on /r/other.state", g.witness, witness(new_state=False, state="/r/other.state"), host="h1")
    refused("is gone: stop the run", g.witness, witness(new_state=False), state_exists=lambda p: False, host="h1")
    g.witness(witness(new_state=False), state_exists=lambda p: True, host="h1")
    # The same id and peers on another host is another witness's first start.
    g.witness(witness(), state_exists=lambda p: False, host="h2")


@case
def witness_state_guard():
    """H10: the campaign's witness state guard sees the file deleted, its
    directory recreated, and an older copy put back; an ordinary save
    (rename over the file, versions up) passes."""
    work = tempfile.mkdtemp(prefix="snc-h10-")
    try:
        d = f"{work}/run"
        os.mkdir(d)
        path = f"{d}/w0.state"

        def write(text):
            with open(path + ".tmp", "w") as f:
                f.write(text)
            os.replace(path + ".tmp", path)

        def probe(i):
            if not os.path.exists(path):
                return {"exists": False, "dir_id": None, "text": ""}
            st = os.stat(d)
            marker = open(path + ".run").read() if os.path.exists(path + ".run") else "-"
            with open(path) as f:
                return {"exists": True, "dir_id": f"{st.st_dev}:{st.st_ino}", "marker": marker, "text": f.read()}

        v1 = "chosen ha/0 1 a\nopen ha/0 2\npromised ha/0 0\naccepted ha/0 0 0 -\n"
        v3 = "chosen ha/0 3 c\nopen ha/0 4\npromised ha/0 0\naccepted ha/0 0 0 -\n" \
             "chosen placement 2 p\nopen placement 3\npromised placement 0\naccepted placement 0 0 -\n"
        write(v1)
        with open(path + ".run", "w") as f:
            f.write("nonce1")
        g = WitnessStateGuard()
        g.record(0, "h1", 0, "h1:21000,h2:21000,h3:21000", path, probe)
        write(v3)
        g.check(0, probe, "after a save")
        old = v1.replace("chosen ha/0 1", "chosen ha/0 2")
        write(old)  # ha/0 2 < 3: an older copy
        refused_state("went back from version 3", g.check, 0, probe)
        write(v3)
        g.check(0, probe, "v3 back")
        # Same chosen versions, an older promise (C4a with the COMMIT lost).
        write(v3.replace("promised ha/0 0", "promised ha/0 900"))
        g.check(0, probe, "promise")
        write(v3)
        refused_state("went back from (chosen, open, promised, accepted)", g.check, 0, probe)
        write(v3.replace("promised ha/0 0", "promised ha/0 900"))
        os.remove(path)
        refused_state("state file gone", g.check, 0, probe)
        shutil.rmtree(d)
        os.mkdir(d)
        write(v3)
        refused_state("state directory replaced", g.check, 0, probe)
        g2 = WitnessStateGuard()
        os.remove(path)
        refused_state("missing right after its first start", g2.record, 0, "h1", 0, "h1:1,h2:1,h3:1", path, probe)
        # Legacy files count as ha/0.
        assert state_versions("chosen 7 x 0 1 0\nopen 8\npromised 0\naccepted 0 0 -\n") == {"ha/0": 7}
        assert state_versions("chosen 5 7 x\nopen 5 8\n") == {"5": 7}
        assert g.manifest()["0"]["state"] == path
        assert state_marks("chosen 7 x 0 1 0\nopen 8\npromised 3\naccepted 2 1 y\n") == {"ha/0": (7, 8, 3, 2)}
    finally:
        shutil.rmtree(work, ignore_errors=True)


@case
def witness_state_boundaries():
    """Why the lab checks at boundaries (PR #52 review): a rollback that the
    witness later carries past the recorded marks is invisible to a check at
    the end alone, and seen by a check made while it is still behind."""
    work = tempfile.mkdtemp(prefix="snc-h10b-")
    try:
        path = f"{work}/w0.state"

        def write(v):
            with open(path, "w") as f:
                f.write(f"chosen ha/0 {v} x\nopen ha/0 {v + 1}\npromised ha/0 0\naccepted ha/0 0 0 -\n")
        with open(path + ".run", "w") as f:
            f.write("n")

        def probe(i):
            st = os.stat(work)
            with open(path) as f:
                return {"exists": True, "dir_id": f"{st.st_dev}:{st.st_ino}", "marker": "n", "text": f.read()}
        history = [1, 5, 2, 9]  # 5 -> 2: rolled back; then on past every mark
        end_only, boundaries = WitnessStateGuard(), WitnessStateGuard()
        write(history[0])
        end_only.record(0, "h1", 0, "h1:1,h2:1,h3:1", path, probe)
        boundaries.record(0, "h1", 0, "h1:1,h2:1,h3:1", path, probe)
        seen = None
        for v in history[1:]:
            write(v)
            try:
                boundaries.check(0, probe, f"boundary v{v}")
            except WitnessStateViolation as e:
                seen = seen or str(e)
        end_only.check(0, probe, "end")  # passes: 9 >= 1
        assert seen and "boundary v2" in seen and "went back from version 5" in seen, seen
    finally:
        shutil.rmtree(work, ignore_errors=True)


def refused_state(why, f, *a):
    try:
        f(*a)
    except WitnessStateViolation as e:
        assert why in str(e), (why, str(e))
        return
    raise AssertionError(f"accepted, expected: {why}")


def main():
    for fn in CASES:
        fn()
        print(f"  {fn.__name__}: ok")
    print(f"test_profile_check: {len(CASES)} cases ok")


if __name__ == "__main__":
    main()
