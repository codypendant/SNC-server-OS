"""Performance runs only (prereg 8): client-side timing of the task and of every stub and tool request."""

import http.client
import json
import os
import time
import urllib.request

import psycopg
from dbos import DBOS

OUT = os.path.join(os.environ["AR1_RUN"], "perf-client.jsonl")
PORT = json.load(open(os.path.join(os.environ["AR1_RUN"], "run.json")))["pg"]["primary_port"]


def lsn():
    with psycopg.connect(host="127.0.0.1", port=PORT, user="ar1", dbname="ar1", autocommit=True) as c:
        return c.execute("SELECT pg_current_wal_insert_lsn()::text").fetchone()[0]


def log(**e):
    with open(OUT, "a") as f:
        f.write(json.dumps(e) + "\n")


_urlopen = urllib.request.urlopen


def urlopen(req, *a, **kw):
    t0 = time.monotonic_ns()
    try:
        r = _urlopen(req, *a, **kw)
    except Exception:
        log(kind="http", url=getattr(req, "full_url", str(req)), ns=time.monotonic_ns() - t0)
        raise
    r._ar1 = (t0, getattr(req, "full_url", str(req)))
    return r


def ended(self):
    t = getattr(self, "_ar1", None)
    if t:
        self._ar1 = None
        log(kind="http", url=t[1], ns=time.monotonic_ns() - t[0])


for name in ("close", "_close_conn"):
    orig = getattr(http.client.HTTPResponse, name)

    def wrap(self, _orig=orig):
        ended(self)
        return _orig(self)

    setattr(http.client.HTTPResponse, name, wrap)

urllib.request.urlopen = urlopen
_start, _destroy = DBOS.start_workflow, DBOS.destroy


def start_workflow(*a, **kw):
    l0 = lsn()
    log(kind="task-start", t=time.monotonic_ns(), lsn=l0)
    return _start(*a, **kw)


def destroy(*a, **kw):
    t = time.monotonic_ns()
    log(kind="task-end", t=t, lsn=lsn())
    return _destroy(*a, **kw)


DBOS.start_workflow, DBOS.destroy = start_workflow, destroy
