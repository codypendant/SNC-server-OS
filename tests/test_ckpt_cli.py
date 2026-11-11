#!/usr/bin/env python3
"""snc_server checkpoint walk flags (docs/handoff/PR98_S2_Pacing_S6_Prereg_Final_Review.md section 7).

--ckpt-walk-batch N, --ckpt-walk-min-us N, --ckpt-walk-max-us N,
--ckpt-walk-ceiling-us N and --ckpt-walk-step-us N set the existing
Engine::Config walk fields. Deterministic; no load, no data directory.

  C1 no new flag: the startup line shows the compiled defaults, unchanged
     (batch 16, 20 / 100 / 250 / 10 us, update 1000 us, target 30000 ms,
     max age 300000 ms).
  C2 the S6 set (20 / 65 / 155 / 5, batch 16, target 30000, max age 60000)
     starts and its startup line shows exactly those values.
  C3 each flag alone, at a valid value, starts and shows it.
  C4 refused with exit 2 before anything starts (no "snc_server shards="
     line, no exception or abort): zero, negative, malformed, empty, space,
     overflow, above 60 s (time) or u32 (batch), max < min, ceiling < max,
     and a single flag that breaks the order against the defaults.
  C5 the 60 s bound itself (60000000 us) and the u32 maximum batch start.

Usage: test_ckpt_cli.py BUILD_DIR
"""
import os
import re
import signal
import socket
import subprocess
import sys
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "build"
SERVER = os.path.join(BUILD, "snc_server")
LINE = re.compile(r"^ckpt_pacing: (.*)$", re.M)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def refused(*flags):
    """The server must exit 2 at once, with a message, and start nothing."""
    r = subprocess.run([SERVER, "--shards", "1", "--port", str(free_port()), *flags],
                       capture_output=True, text=True, timeout=30)
    out = r.stdout + r.stderr
    assert r.returncode == 2, f"{flags}: rc {r.returncode}, want 2\n{out}"
    assert "snc_server shards=" not in out and "ckpt_pacing:" not in out, f"{flags}: started\n{out}"
    assert r.stderr.strip(), f"{flags}: no message"
    assert "terminate called" not in out and "Aborted" not in out, f"{flags}: not a clean exit\n{out}"
    return r.stderr.strip()


def started(*flags):
    """Start, read the effective pacing line, stop. Returns it as a dict."""
    p = subprocess.Popen([SERVER, "--shards", "1", "--port", str(free_port()), *flags],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines, got = [], None
    t_end = time.time() + 30
    try:
        while time.time() < t_end:
            line = p.stdout.readline()
            if not line:
                break
            lines.append(line)
            m = LINE.match(line.rstrip("\n"))
            if m:
                got = dict(kv.split("=", 1) for kv in m.group(1).split())
                break
    finally:
        p.send_signal(signal.SIGTERM)
        try:
            rest, _ = p.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            p.kill()
            rest, _ = p.communicate()
    assert got is not None, f"{flags}: no ckpt_pacing line\n{''.join(lines)}{rest}"
    assert p.returncode == 0, f"{flags}: rc {p.returncode}\n{''.join(lines)}{rest}"
    return got


DEFAULTS = dict(pacing="on", batch="16", min_us="20", max_us="100", ceiling_us="250", step_us="10",
                update_us="1000", target_ms="30000", max_age_ms="300000")


def main():
    # C1
    assert started() == DEFAULTS, started()
    print("C1 defaults unchanged:", DEFAULTS)

    # C2
    s6 = ["--ckpt-walk-batch", "16", "--ckpt-walk-min-us", "20", "--ckpt-walk-max-us", "65",
          "--ckpt-walk-ceiling-us", "155", "--ckpt-walk-step-us", "5",
          "--ckpt-target-ms", "30000", "--ckpt-max-age-ms", "60000"]
    want = dict(DEFAULTS, max_us="65", ceiling_us="155", step_us="5", max_age_ms="60000")
    got = started(*s6)
    assert got == want, got
    print("C2 S6 set:", got)

    # C3: each flag alone (valid against the other defaults)
    for flag, value, key in (("--ckpt-walk-batch", "8", "batch"), ("--ckpt-walk-min-us", "30", "min_us"),
                             ("--ckpt-walk-max-us", "65", "max_us"), ("--ckpt-walk-ceiling-us", "155", "ceiling_us"),
                             ("--ckpt-walk-step-us", "5", "step_us")):
        got = started(flag, value)
        assert got == dict(DEFAULTS, **{key: value}), (flag, got)
    print("C3 each flag alone: ok")

    # C4
    n = 0
    for flag in ("--ckpt-walk-batch", "--ckpt-walk-min-us", "--ckpt-walk-max-us", "--ckpt-walk-ceiling-us",
                 "--ckpt-walk-step-us"):
        for bad in ("0", "-5", "abc", "12x", "", " 5", "+5", "1.5", "99999999999999999999999"):
            refused(flag, bad)
            n += 1
    refused("--ckpt-walk-batch", "4294967296"); n += 1                      # above u32
    for flag in ("--ckpt-walk-min-us", "--ckpt-walk-max-us", "--ckpt-walk-ceiling-us", "--ckpt-walk-step-us"):
        refused(flag, "60000001"); n += 1                                   # above 60 s
    msg = refused("--ckpt-walk-min-us", "70", "--ckpt-walk-max-us", "65", "--ckpt-walk-ceiling-us", "155"); n += 1
    assert "min <= max <= ceiling" in msg, msg
    msg = refused("--ckpt-walk-min-us", "20", "--ckpt-walk-max-us", "160", "--ckpt-walk-ceiling-us", "155"); n += 1
    assert "min <= max <= ceiling" in msg, msg
    refused("--ckpt-walk-max-us", "10"); n += 1                              # below the default min 20
    refused("--ckpt-walk-ceiling-us", "65"); n += 1                          # below the default max 100
    refused("--ckpt-walk-min-us", "120"); n += 1                             # above the default max 100
    refused("--ckpt-walk-max-us"); n += 1                                    # no value
    print(f"C4 refused with exit 2: {n} cases")

    # C5
    got = started("--ckpt-walk-min-us", "20", "--ckpt-walk-max-us", "65", "--ckpt-walk-ceiling-us", "60000000",
                  "--ckpt-walk-step-us", "60000000", "--ckpt-walk-batch", "4294967295")
    assert got["ceiling_us"] == "60000000" and got["step_us"] == "60000000" and got["batch"] == "4294967295", got
    print("C5 bounds accepted:", got)
    print("ok")


if __name__ == "__main__":
    main()
