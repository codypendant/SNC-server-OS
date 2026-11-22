"""One app process: python -m ar1.app start|recover|takeover <epoch>. Env: AR1_RUN, AR1_EXECUTOR, AR1_POINTS, AR1_SEED."""

import sys
import time

import psycopg
from dbos import DBOS, SetWorkflowID

from . import rt


def end_previous_backends():
    """Package 2.6 d7: make the previous incarnation's transactions final before anything reads them."""
    with psycopg.connect(
        host="127.0.0.1", port=rt.PORT, user="ar1", dbname="ar1", autocommit=True, application_name="ar1-recovery"
    ) as c:
        like = f"{rt.APP}-%"
        c.execute("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name LIKE %s", (like,))
        while c.execute("SELECT count(*) FROM pg_stat_activity WHERE application_name LIKE %s", (like,)).fetchone()[0]:
            time.sleep(0.01)


def main():
    mode, epoch = sys.argv[1], int(sys.argv[2])
    if mode == "recover":
        end_previous_backends()
    port = rt.PORT
    # One system database per executor: DBOS 3.6 recovery re-enqueues a dead executor's workflows on a shared
    # internal queue, where any live process (including a stale activation's) would run them.
    DBOS(
        config={
            "name": "ar1",
            "system_database_url": f"postgresql://ar1@127.0.0.1:{port}/ar1_sys_{rt.EXECUTOR}?application_name={rt.APP}-sys",
            "application_version": "ar1-snc-1",
            "executor_id": rt.EXECUTOR,
        }
    )
    rt.init_datasource(f"postgresql+psycopg://ar1@127.0.0.1:{port}/ar1?application_name={rt.APP}-ds")
    DBOS.launch()
    wf = f"T:e{epoch}"
    if mode == "takeover":
        go = rt.R / "ctl" / "go-takeover"
        (rt.R / "ctl" / f"ready-{rt.EXECUTOR}").write_text("1")
        while not go.exists():
            time.sleep(0.002)
    if mode in ("start", "takeover"):
        with SetWorkflowID(wf):
            h = DBOS.start_workflow(rt.activation, epoch, mode == "takeover")
    else:
        (rt.R / "ctl" / f"ready-{rt.EXECUTOR}").write_text("1")
        h = DBOS.retrieve_workflow(wf)
    result = h.get_result()
    (rt.R / "ctl" / f"result-{rt.EXECUTOR}").write_text(str(result))
    DBOS.destroy()


if __name__ == "__main__":
    main()
