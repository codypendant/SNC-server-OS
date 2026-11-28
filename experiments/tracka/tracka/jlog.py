import json
import os
import time


def now():
    return time.monotonic_ns()


def canonical(v):
    return json.dumps(v, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def append(path, rec):
    rec = {"t": now(), **rec}
    fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
    try:
        os.write(fd, (json.dumps(rec, sort_keys=True) + "\n").encode())
        os.fsync(fd)
    finally:
        os.close(fd)
    return rec


def read(path):
    try:
        text = open(path).read()
    except FileNotFoundError:
        return []
    return [
        json.loads(x) for x in text[: text.rfind("\n") + 1].splitlines() if x.strip()
    ]
