"""Benchmark profile guard (docs/EXPERIMENTAL_BASELINE.md: T-H, sections 1-2).

A campaign run declares one profile (A LOCAL_DURABLE, B SYNC HA, C MEMORY
CEILING, D ASYNC LOCAL_DURABLE) and the deviations from it that it means
to make. Every process the harness starts is checked here first, so a run
outside its profile is refused rather than measured:

  - always refused, deviation or not: async replication with witness HA
    (an async primary with --witness, or witnesses promoting in Profile D);
    a witness polling STATUS (--health-verb status); a data directory that
    already exists when the run first starts a node on it; a restart of a
    data directory with another --shards than its first start in the run;
    standby reads (--standby-reads) in Profile B; and each profile's own
    shape (B: a sync standby and a witness gate on every primary, placement
    v2; A and C: no replication; C: no data directory; D: no witness HA);
  - refused unless declared: a fixed setting at another value than the
    baseline's (section 1: --group-commit-us 50, --pages 4k, --move-units
    on, --checkpoint-s 60, --replica-timeout-ms 2000), and any flag that is
    not part of a profile's shape (--debug-admin, --spin, ...).

A deviation is declared as "--flag" (any value) or "--flag=value". Used by
bench/lab/lab.py (the `profile` and `deviations` keys of [lab]) and the
campaign scripts; tests/test_profile_check.py checks every rule.
"""

PROFILES = {
    "A": "LOCAL_DURABLE",
    "B": "SYNC_HA",
    "C": "MEMORY_CEILING",
    "D": "ASYNC_LOCAL_DURABLE",
}

# Flags that take no value.
SWITCHES = {"--async-replica", "--debug-admin", "--spin", "--no-mint-local"}

# The baseline's fixed settings (section 1): absent means the binary's
# default, given here as the value it stands for.
FIXED = {
    "--group-commit-us": ("50", "50"),
    "--pages": ("4k", "4k"),
    "--move-units": ("on", "on"),
    "--checkpoint-s": ("60", "0"),       # (required value, the default when absent)
    "--replica-timeout-ms": ("2000", "2000"),
}

# Placement, addressing and identity: they shape a run without changing
# what a profile means.
STRUCTURAL = {
    "--shards", "--cpus", "--host", "--port", "--objects", "--data-dir", "--control", "--node-id", "--admin-port",
    "--report-ms", "--space-uuid", "--seed-port", "--seed-from", "--rejoin", "--standby-host", "--cluster",
    "--group", "--member", "--standby", "--replica-of", "--witness", "--async-replica", "--replica-retain-mib",
}

WITNESS_STRUCTURAL = {"--port", "--state", "--id", "--peers", "--failover-ms", "--poll-ms", "--group", "--primary",
                      "--standby", "--health-verb", "--new-state"}


class ProfileViolation(Exception):
    pass


# Flags that may appear more than once (each value kept, in order).
REPEATABLE = {"snc_server": set(), "snc_witness": {"--group"}}


def parse(argv, binary="snc_server"):
    """argv (binary first) -> {flag: value or True}, read as the binary
    reads it: a switch takes no value; every other flag takes the next
    token, whatever it is (so a missing, empty or "--"-prefixed value is
    refused here rather than misread). A flag given twice is refused,
    except the binary's repeatable ones (kept as a list)."""
    out, i = {}, 1
    args = [str(a) for a in argv]
    while i < len(args):
        k = args[i]
        if not k.startswith("--"):
            raise ProfileViolation(f"unexpected argument {k!r} in {' '.join(args)}")
        if k in SWITCHES:
            v, i = True, i + 1
        else:
            if i + 1 >= len(args) or args[i + 1] == "" or args[i + 1].startswith("--"):
                raise ProfileViolation(f"{k} needs a value (the binary would take the next token as it)")
            v, i = args[i + 1], i + 2
        if k in REPEATABLE.get(binary, set()):
            out.setdefault(k, []).append(v)
        elif k in out:
            raise ProfileViolation(f"{k} given twice: the binary keeps the last one; give it once")
        else:
            out[k] = v
    return out


def same_value(a, b):
    """Numeric settings compare as numbers (050 is 50)."""
    try:
        return float(a) == float(b)
    except ValueError:
        return a == b


class Guard:
    def __init__(self, profile, deviations=()):
        if profile not in PROFILES:
            raise ProfileViolation(f"unknown profile {profile!r} (one of {', '.join(PROFILES)})")
        self.profile = profile
        self.deviations = list(deviations)
        self.used = set()       # declared deviations a process actually made
        self.dirs = {}          # (host, data dir) -> (--shards, --node-id) at its first start in this run
        self.witnesses = {}     # (host, --id, peers) -> --state at its first start in this run

    def _declared(self, flag, value):
        for d in self.deviations:
            f, _, v = d.partition("=")
            if f == flag and (not v or v == str(value)):
                self.used.add(d)
                return True
        return False

    def _deviation(self, flag, value, why):
        if not self._declared(flag, value):
            raise ProfileViolation(f"Profile {self.profile}: {why}, not declared as a deviation "
                                   f"(add \"{flag}\" or \"{flag}={value}\" to the run's deviations)")

    def server(self, argv, dir_exists=lambda path: False, host=""):
        """Checks one snc_server command line for `host`. dir_exists(path)
        says whether a path exists on that host."""
        f = parse(argv, "snc_server")
        p = self.profile
        primary = "--replica-of" in f
        standby = "--standby" in f
        if f.get("--standby-reads", "0") != "0" and p == "B":
            raise ProfileViolation("Profile B: standby reads (--standby-reads) are excluded")
        if f.get("--async-replica") and "--witness" in f:
            raise ProfileViolation("async replication with witness HA (--async-replica with --witness) is refused")
        if p == "B":
            if not (primary or standby):
                raise ProfileViolation("Profile B: every node is a sync primary (--replica-of) or its standby (--standby)")
            if primary and f.get("--async-replica"):
                raise ProfileViolation("Profile B is synchronous only (--async-replica)")
            if primary and "--witness" not in f:
                raise ProfileViolation("Profile B: a primary needs the witness gate (--witness)")
        if p in ("A", "C") and (primary or standby or "--witness" in f):
            raise ProfileViolation(f"Profile {p}: no replication or witness gate (--replica-of/--standby/--witness)")
        if p == "C" and "--data-dir" in f:
            raise ProfileViolation("Profile C: no data directory (memory ceiling)")
        if p in ("A", "B", "D") and "--data-dir" not in f:
            raise ProfileViolation(f"Profile {p}: a data directory is required")
        if p == "D":
            if "--witness" in f:
                raise ProfileViolation("Profile D: no witness HA (manual failover only)")
            if primary and not f.get("--async-replica"):
                raise ProfileViolation("Profile D: replication is async (--async-replica)")
        if "--shards" not in f:
            raise ProfileViolation("--shards must be given (one per core, from the manifest)")
        d = f.get("--data-dir")
        if d:
            key, node = (host, d), f.get("--node-id")
            if key not in self.dirs:
                if dir_exists(d):
                    raise ProfileViolation(f"data directory {d} already exists: every run starts on fresh ones")
                self.dirs[key] = (f["--shards"], node)
            else:
                shards, first = self.dirs[key]
                if first != node:
                    raise ProfileViolation(f"data directory {d} on {host or 'this host'}: node {node} would start "
                                           f"on node {first}'s directory")
                if not same_value(shards, f["--shards"]):
                    raise ProfileViolation(f"data directory {d}: --shards {f['--shards']} on restart, {shards} at "
                                           "its start: a shard-count change within a run is refused")
        for flag, (want, default) in FIXED.items():
            if flag == "--checkpoint-s" and p == "C":
                continue  # no data directory: nothing to checkpoint
            if flag == "--replica-timeout-ms" and not primary and flag not in f:
                continue  # a standby given none: it has none to apply
            got = str(f.get(flag, default))
            if not same_value(got, want):
                self._deviation(flag, got, f"{flag} {got} instead of {want}")
        for flag, value in f.items():
            if flag not in STRUCTURAL and flag not in FIXED:
                self._deviation(flag, value if value is not True else "", f"{flag} is not part of Profile {p}")
        return f

    def witness(self, argv, state_exists=None, host=""):
        """Checks one snc_witness command line for `host`. state_exists(path)
        says whether its --state file exists on that host (None: not
        checked). Profile B witnesses (docs/PAXOS_BALLOTS.md 6, 9): three
        distinct endpoints, --id naming this witness's own --port,
        --new-state 1 exactly on a witness's first start in the run (its
        file absent), never on a restart, and a restart with the same
        state file and peers."""
        f = parse(argv, "snc_witness")
        if self.profile == "B":
            self._witness_identity(f, state_exists, host)
        if f.get("--health-verb", "health") != "health":
            raise ProfileViolation("a witness polling STATUS (--health-verb status) is refused: witnesses poll HEALTH")
        ha = "--group" in f or "--primary" in f
        if ha and self.profile != "B":
            raise ProfileViolation(f"Profile {self.profile}: witnesses may hold placement but promote nothing "
                                   "(no --group/--primary)" + (": async with witness HA is refused"
                                                                if self.profile == "D" else ""))
        for flag, value in f.items():
            if flag not in WITNESS_STRUCTURAL:
                self._deviation(flag, value if value is not True else "", f"witness {flag} is not part of the profile")
        return f

    def _witness_identity(self, f, state_exists, host):
        peers = [p for p in str(f.get("--peers", "")).split(",") if p]
        if len(peers) != 3:
            raise ProfileViolation(f"Profile B: three witnesses in --peers, got {len(peers)}")
        norm = [normalize_endpoint(p) for p in peers]
        if len(set(norm)) != 3:
            raise ProfileViolation(f"Profile B: --peers names one endpoint twice ({', '.join(norm)})")
        try:
            wid = int(f.get("--id", "-1"))
        except ValueError:
            wid = -1
        if not 0 <= wid < 3 or norm[wid].rsplit(":", 1)[1] != str(f.get("--port")):
            raise ProfileViolation(f"Profile B: --id {f.get('--id')} must name this witness's own --port "
                                   f"{f.get('--port')} in --peers")
        new = f.get("--new-state", "0") == "1"
        key = (host, wid, tuple(norm))
        state = f.get("--state")
        seen = self.witnesses.get(key)
        if seen is None:
            if not new:
                raise ProfileViolation("Profile B: a witness's first start in a run creates its state "
                                       "(--new-state 1)")
            if state_exists is not None and state_exists(state):
                raise ProfileViolation(f"Profile B: witness state {state} already exists at its first start")
            self.witnesses[key] = state
            return
        if new:
            raise ProfileViolation("Profile B: --new-state on a witness restart (a witness that lost its state "
                                   "must not rejoin as itself; stop the run)")
        if seen != state:
            raise ProfileViolation(f"Profile B: witness {wid} restarted on {state}, not {seen}")
        if state_exists is not None and not state_exists(state):
            raise ProfileViolation(f"Profile B: witness {wid}'s state {state} is gone: stop the run")

    def ctl_init(self, argv):
        """snc_ctl ... init: Profile B writes placement v2."""
        args = [str(a) for a in argv]
        before = args[:args.index("init")] if "init" in args else args
        if self.profile == "B" and "--placement-v2" not in before:
            raise ProfileViolation("Profile B: the placement is written as v2 (snc_ctl --placement-v2 init)")

    def report(self):
        """For result.json: the profile, and the declared deviations actually
        made (a declared one never made is listed apart)."""
        return {"profile": self.profile, "profile_name": PROFILES[self.profile],
                "deviations_made": sorted(self.used),
                "deviations_declared_unused": sorted(set(self.deviations) - self.used)}


def normalize_endpoint(hostport):
    """host:port with the host as an IPv4 address where it resolves (so
    "localhost:1" and "127.0.0.1:1" are one endpoint)."""
    import socket
    host, _, port = str(hostport).strip().rpartition(":")
    host = host.strip().lower()
    try:
        host = socket.gethostbyname(host)
    except OSError:
        pass
    return f"{host}:{int(port)}"


class WitnessStateViolation(Exception):
    pass


def state_versions(text):
    """{machine: chosen version} from a witness state file's text. A legacy
    file ("open v": two fields) holds ha/0 only."""
    lines = [l.split() for l in text.splitlines() if l.strip()]
    legacy = any(t[0] == "open" and len(t) == 2 for t in lines)
    out = {}
    for t in lines:
        if t[0] != "chosen":
            continue
        if legacy and len(t) >= 2:
            out["ha/0"] = int(t[1])
        elif len(t) >= 3:
            out[t[1]] = int(t[2])
    return out


class WitnessStateGuard:
    """G2 (docs/PAXOS_BALLOTS.md 6): a campaign's witness state must never
    disappear, move to a recreated directory, or go back to older chosen
    versions while the run lasts. probe(i) returns {"exists": bool,
    "dir_id": "dev:ino" of the state file's directory, "text": the file}.
    Not a general F9 solution: a copy with versions at least as new is not
    seen."""

    def __init__(self):
        self.w = {}  # i -> {"host", "id", "peers", "state", "dir_id", "versions"}

    def record(self, i, host, wid, peers, state, probe):
        p = probe(i)
        if not p["exists"]:
            raise WitnessStateViolation(f"witness {i}: state {state} missing right after its first start")
        self.w[i] = {"host": host, "id": wid, "peers": [normalize_endpoint(x) for x in peers.split(",")],
                     "state": state, "dir_id": p["dir_id"], "versions": state_versions(p["text"])}

    def check(self, i, probe, stage=""):
        rec = self.w[i]
        p = probe(i)
        where = f"witness {i} ({rec['state']}){' at ' + stage if stage else ''}"
        if not p["exists"]:
            raise WitnessStateViolation(f"{where}: state file gone: the run is invalid")
        if p["dir_id"] != rec["dir_id"]:
            raise WitnessStateViolation(f"{where}: state directory replaced ({rec['dir_id']} -> {p['dir_id']}): "
                                        "the run is invalid")
        now = state_versions(p["text"])
        for m, v in rec["versions"].items():
            if now.get(m, -1) < v:
                raise WitnessStateViolation(f"{where}: {m} went back from version {v} to {now.get(m)}: "
                                            "state replaced or rolled back; the run is invalid")
        rec["versions"] = {**rec["versions"], **now}

    def manifest(self):
        return {str(i): {k: v for k, v in r.items()} for i, r in self.w.items()}
