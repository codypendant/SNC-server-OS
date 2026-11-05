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
from profile_check import Guard, ProfileViolation  # noqa: E402

SRV, WIT, CTL = "snc_server", "snc_witness", "snc_ctl"


def b_primary(d="/r/n1", shards=4, extra=()):
    return [SRV, "--shards", shards, "--objects", 1000000, "--data-dir", d, "--control", "h:1,h:2,h:3",
            "--node-id", 1, "--admin-port", 21140, "--witness", "h:1,h:2,h:3", "--replica-of", "h2:21170",
            "--replica-timeout-ms", 2000, "--checkpoint-s", 60] + list(extra)


def b_standby(d="/r/n2", shards=4, extra=()):
    return [SRV, "--shards", shards, "--objects", 1000000, "--data-dir", d, "--control", "h:1,h:2,h:3",
            "--node-id", 2, "--admin-port", 21180, "--witness", "h:1,h:2,h:3", "--standby", 21210,
            "--checkpoint-s", 60] + list(extra)


def witness(extra=()):
    return [WIT, "--port", 21000, "--state", "/r/w0.state", "--id", 0, "--peers", "h:1,h:2,h:3",
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
    g.witness(lab.witness_args(bin_path=WIT, port=21000, state="/r/w0.state", wid=0, control="h:1,h:2,h:3",
                               failover_ms=1000, poll_ms=100, groups=["0=h1:21140,h2:21180"]))
    g.ctl_init([CTL, "--control", "h:1"] + lab.ctl_flags("B") + ["init"])


def main():
    for fn in CASES:
        fn()
        print(f"  {fn.__name__}: ok")
    print(f"test_profile_check: {len(CASES)} cases ok")


if __name__ == "__main__":
    main()
