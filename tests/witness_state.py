"""--new-state for test witnesses (docs/PAXOS_BALLOTS.md 6): snc_witness
starts only on an existing state file, or with --new-state 1 on its first
start. Test clusters start every witness fresh and restart it on the same
file, so the flag goes on when the file does not exist yet."""
import os


def first_start(args, binary_knows_it=True):
    """args with --new-state 1 added when its --state file does not exist
    (and the binary has the flag: older builds in mixed-version tests do
    not, and need no file)."""
    args = [str(a) for a in args]
    state = args[args.index("--state") + 1]
    if binary_knows_it and not os.path.exists(state):
        return args + ["--new-state", "1"]
    return args
