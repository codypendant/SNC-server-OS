import hashlib
import json
import os
import time
from pathlib import Path

import psycopg
import sqlalchemy as sa

R = Path(os.environ["AR1_RUN"])
RUN = json.loads((R / "run.json").read_text())
PORT = RUN["pg"]["primary_port"]
EX = os.environ.get("AR1_EXECUTOR", "setup")
URL = f"postgresql+psycopg://ar1@127.0.0.1:{PORT}/"

SCHEMA = """
CREATE SCHEMA plain;
CREATE TABLE plain.agent (id text PRIMARY KEY, epoch int, version int);
CREATE TABLE plain.task (id text PRIMARY KEY, agent text, status text, requester text, goal text, version int);
CREATE TABLE plain.memory (id text PRIMARY KEY, version int, agent text, content_hash text, token text);
CREATE TABLE plain.invocation (id text PRIMARY KEY, n int, epoch int, state text, attempts int, body jsonb,
    whole_hash text, request_id text, output text, late bool, retry_of text, stale bool);
CREATE TABLE plain.call (id text PRIMARY KEY, agent text, task text, tool text, key text, epoch int, state text,
    attempts int, result jsonb, policy text, stale bool);
CREATE TABLE plain.decision (id text PRIMARY KEY, task text, inv text, rule_version text, inputs text, result text,
    approval text, epoch int);
CREATE TABLE plain.commits (token text PRIMARY KEY, xid xid8, epoch int, kind text, name text, agent text, ref text,
    state text, data jsonb);
"""
COMMIT = (
    "INSERT INTO plain.commits VALUES (gen_random_uuid(), pg_current_xact_id(), :e, :k, :n, 'A', :r, :s,"
    " CAST(:d AS jsonb))"
)
SUPERSEDE = """
WITH r AS (UPDATE plain.call SET state='rejected' WHERE state='prepared' AND epoch < :e RETURNING id, 'rejected' s),
a AS (UPDATE plain.invocation SET state='abandoned' WHERE state='prepared' AND epoch < :e RETURNING id, 'abandoned' s),
o AS (SELECT id, 'outstanding' s FROM plain.call WHERE state IN ('dispatched', 'in_doubt')
    UNION ALL SELECT id, 'outstanding' FROM plain.invocation WHERE state IN ('dispatched', 'accepted'))
SELECT coalesce(jsonb_object_agg(id, s), '{}') FROM (TABLE r UNION ALL TABLE a UNION ALL TABLE o) x
"""
BOUND = (
    "SELECT CASE WHEN mod(l, 16777216) = 40 THEN l - 40 WHEN mod(l, 8192) = 24 THEN l - 24 ELSE l END"
    " FROM (SELECT pg_current_wal_insert_lsn() - '0/0' l) x"
)
FLUSHED = (
    "SELECT coalesce(bool_or(flush_lsn - '0/0' >= %s AND sync_state = 'sync'), false)"
    " FROM pg_stat_replication WHERE application_name = 's1'"
)


def blob(blob_dir, content):
    h = hashlib.sha256(content).hexdigest()
    Path(blob_dir).mkdir(parents=True, exist_ok=True)
    (Path(blob_dir) / h).write_bytes(content)
    return h


def connect():
    return psycopg.connect(
        host="127.0.0.1",
        port=PORT,
        user="ar1",
        dbname="ar1",
        autocommit=True,
        application_name=f"ar1-{EX}-rt",
    )


def wait():
    with connect() as c:
        b = c.execute(BOUND).fetchone()[0]
        while not c.execute(FLUSHED, (b,)).fetchone()[0]:
            time.sleep(0.005)


def setup_db(blob_dir):
    with connect() as c:
        c.execute(SCHEMA)
        c.execute("INSERT INTO plain.agent VALUES ('A', 0, 0)")
        c.execute("INSERT INTO plain.task VALUES ('T', 'A', 'requested', 'requester', 'the goal', 0)")
        for i in (1, 2, 3):
            h = blob(blob_dir, f"memory item m{i}".encode())
            c.execute("INSERT INTO plain.memory VALUES (%s, 1, 'A', %s)", (f"m{i}", h))


def cancel():
    with sa.create_engine(URL + "ar1").begin() as c:
        x = lambda sql, **p: c.execute(sa.text(sql), p)
        e = x("SELECT epoch FROM plain.agent WHERE id='A' FOR UPDATE").scalar()
        x("UPDATE plain.task SET status='cancelled', version=version+1 WHERE id='T'")
        x(COMMIT, e=e, k="cancel", n="CANCEL", r="T", s="cancelled", d=json.dumps(x(SUPERSEDE, e=e + 1).scalar()))
    wait()
