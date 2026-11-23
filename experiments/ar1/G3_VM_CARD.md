# AR-1 G3 card: the dedicated VM

This card is for cody. It runs the G3 counted campaign that cody authorized on 2026-11-23
(`docs/handoff/AR1_G3_Owner_Decision.md`), under `docs/AR1_PREREG.md` revision 3.

- **Frozen commit:** `FROZEN_SHA`.
- **Driver:** `ar1/g3.py`, which runs every phase without stopping.
- **Expected time:** about 20 hours.
- **Restarts:** it can be restarted after a reboot, and it keeps the runs that are already finished.

## 1. Create the VM

Create a fresh VM at any cloud provider:
- Ubuntu 24.04 LTS, x86-64;
- 4 vCPU and 16 GB RAM;
- a local SSD of at least 80 GB.

It needs outbound HTTPS to github.com, conda.anaconda.org, pypi.org and files.pythonhosted.org, and nothing inbound except
SSH. Run nothing else on it while the campaign runs.

## 2. Set up and start the campaign

Log in as root over SSH, then paste this block as it is:

```bash
apt-get update -q && apt-get install -y -q git curl bzip2 tmux
id ar1run >/dev/null 2>&1 || useradd -m ar1run
mkdir -p /srv && cd /srv
[ -d snc ] || git clone -q https://github.com/codypendant/SNC-server-OS.git snc
cd /srv/snc && git -c safe.directory='*' fetch -q origin && git -c safe.directory='*' checkout -q FROZEN_SHA && git -c safe.directory='*' log --oneline -1
AR1_ENV=/opt/ar1 bash experiments/ar1/setup.sh
mkdir -p /srv/ar1-runs /srv/ar1-g3 && chown -R ar1run /srv/snc /srv/ar1-runs /srv/ar1-g3
cat > /srv/ar1-g3.sh <<'EOF'
#!/bin/bash
cd /srv/snc/experiments/ar1
exec runuser -u ar1run -- env AR1_PGBIN=/opt/ar1/bin AR1_RUNS=/srv/ar1-runs AR1_FREEZE_EXPECT=FROZEN_SHA \
  PYTHONPATH=/srv/snc/experiments/ar1 HOME=/home/ar1run /opt/ar1/bin/python -m ar1.g3 /srv/ar1-g3 >> /srv/ar1-g3/driver.log 2>&1
EOF
chmod +x /srv/ar1-g3.sh
tmux new -d -s g3 /srv/ar1-g3.sh
echo started
```

The last line should print `started`. The setup step prints `postgres (PostgreSQL) 18.7`. You can log out now; the
campaign keeps running inside tmux.

## 3. Check progress (any time)

```bash
tail -5 /srv/ar1-g3/driver.log; ls /srv/ar1-g3; cat /srv/ar1-g3/*.w*.jsonl 2>/dev/null | wc -l
```

If the VM rebooted, start the campaign again with `tmux new -d -s g3 /srv/ar1-g3.sh`. It picks up where it stopped. Each phase's cap is counted in wall time from the phase's start, so time the VM is down counts against it:
restart soon after a reboot.

## 4. When it ends

The driver is done when `tmux ls` no longer shows `g3`. The last line of `driver.log` is then the outcome. Pack the
results:

```bash
tail -1 /srv/ar1-g3/driver.log; tar czf /root/ar1-g3-results.tgz -C /srv ar1-g3 && ls -l /root/ar1-g3-results.tgz
```

Copy `/root/ar1-g3-results.tgz` to your computer, for example with
`scp root@<vm address>:/root/ar1-g3-results.tgz .`, and upload it in the thread. Delete the VM only after the
upload has been confirmed.

## What the driver does

1. It refuses to run unless the checkout is exactly the frozen commit and has no local changes (prereg 5.5).
2. It writes `manifest.txt`, which records:
   - the CPU, memory, kernel and filesystem;
   - PostgreSQL and its configuration;
   - Python and every package;
   - the frozen SHAs.
3. **Phase 0, the hard gate:**
   - OI-1a, the OI-1a control, OI-1a-v, OI-1b, OI-1c, OI-1d, C-7 and C-7b run 20 times each, and OI-1e runs 20
     times with 1,000 freezes;
   - an invalid run is repeated once with the same seed;
   - if any check fails, the campaign stops with REFINE (design);
   - if a check crashes, has too few valid runs, or OI-1e fails or is incomplete, it stops with UNRESOLVED.
4. **Checker adequacy on the frozen code (section 7.3):** all 14 mutations, 5 seeds at each observable point, every
   run flagged. A missed mutation stops the campaign with UNRESOLVED.
5. **The counted arms:** SNC, plain DBOS step 1 and plain DBOS step 2. Each runs all 128 points with 20 seeds, which
   is 2,560 runs per arm. An invalid run is repeated once with the same seed. An UNRESOLVED run, more than one
   invalid run at a point, a missing run, or a cap being hit stops the campaign.
6. **Performance:** 1,000 tasks for each of SNC and plain step 2, run alternately, with the stub at 500 ms. This
   gives the latency budget B, per consequential commit. Every task must finish, or the outcome is UNRESOLVED.
7. **Determinism divergence:** reported only; it is not a gate.
8. **Outcome:** the section 9 outcome goes in `summary.json` and on the last line of `driver.log`.
