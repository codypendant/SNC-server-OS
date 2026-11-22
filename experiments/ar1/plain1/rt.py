import hashlib
import json
import os
from pathlib import Path

import psycopg

R = Path(os.environ["AR1_RUN"])
RUN = json.loads((R / "run.json").read_text())
PORT = RUN["pg"]["primary_port"]
EX = os.environ.get("AR1_EXECUTOR", "setup")
URL = f"postgresql+psycopg://ar1@127.0.0.1:{PORT}/"

SCHEMA = """
CREATE SCHEMA plain;
CREATE TABLE plain.agent (id text PRIMARY KEY, epoch int, version int);
CREATE TABLE plain.task (id text PRIMARY KEY, agent text, status text, requester text, goal text, version int);
CREATE TABLE plain.memory (id text PRIMARY KEY, version int, agent text, content_hash text);
CREATE TABLE plain.invocation (id text PRIMARY KEY, agent text, task text, epoch int, state text, manifest jsonb,
    whole_hash text, output text);
CREATE TABLE plain.call (id text PRIMARY KEY, agent text, task text, tool text, key text, epoch int, state text,
    attempts int, result jsonb);
CREATE TABLE plain.decision (id text PRIMARY KEY, task text, call_id text, rule_version text, approval text,
    epoch int);
CREATE TABLE plain.commits (token text PRIMARY KEY, xid xid8, epoch int, kind text, name text, agent text, ref text);
"""


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


def setup_db(blob_dir):
    with connect() as c:
        c.execute(SCHEMA)
        c.execute("INSERT INTO plain.agent VALUES ('A', 0, 0)")
        c.execute("INSERT INTO plain.task VALUES ('T', 'A', 'requested', 'requester', 'the goal', 0)")
        for i in (1, 2, 3):
            h = blob(blob_dir, f"memory item m{i}".encode())
            c.execute("INSERT INTO plain.memory VALUES (%s, 1, 'A', %s)", (f"m{i}", h))


def cancel():
    from dbos import DBOSClient

    DBOSClient(system_database_url=URL + "ar1_sys_e1", application_name="ar1-cancel-client").cancel_workflow("T:e1")
    with connect() as c:
        c.execute("UPDATE plain.task SET status='cancelled', version=version+1 WHERE id='T'")
