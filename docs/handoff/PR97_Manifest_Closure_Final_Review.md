# PR #97 manifest-baseline closure: final review, and the next authorization

Filed verbatim from cody's message in the project thread, 2026-11-11 14:22 UTC.

---

PR #97 FINAL REVIEW

Decision: APPROVED.

Merge authorized.

Reviewed head:

"f9c21511577301836dd50030b48d68868f8d78d2"

PR #97 closure review

The manifest-baseline replacement is correct and stays within the authorized scope.

Verified changed files only:

- "AGENTS.md"
- "bench/manifests/r640-main-20261111.txt"
- "bench/pilot_plan.json"
- "docs/PILOT_PLAN.md"
- "docs/R640_TOPOLOGY.md"
- "docs/STATUS.md"

There are no engine, runtime, storage, networking, checkpoint, admission, or protocol changes.

Manifest e

Accepted as the new R640 hardware baseline:

- 2 × Xeon Gold 6150;
- 12 × 32 GB DDR4-2400, 2-rank;
- A1-A6 populated;
- B1-B6 populated;
- six of six memory channels per socket;
- 384 GB nominal;
- approximately 376 GiB usable;
- approximately 192 GB per NUMA node;
- 72 CPUs;
- governor "performance";
- "kernel.numa_balancing=0";
- dedicated ARFX3840S5xnFTRI SAS SSD at "/srv/snc-data";
- eno1 at 10 Gb/s;
- manifest captured against merged PR #95 main "d0d3bb7".

Manifest d remains historical evidence for the old five-channel-per-socket hardware configuration and must not be reinterpreted as manifest-e evidence.

Pilot plan pin

Verified:

"bench/pilot_plan.json"

changes only:

- "approved_manifest_sha256"
- "_approved_manifest"

No change to:

- object count;
- workload mix;
- routing;
- client threads;
- CPU allocations;
- shard count;
- checkpoint interval;
- calibration duration;
- rate grid;
- repetitions;
- sustained criteria;
- latency objective;
- knee rule;
- duration checks;
- capacity reps;
- shard sweep;
- seed;
- any experimental threshold or metric.

The post-pilot manifest change is acceptable because it is a reviewed hardware-baseline replacement forced by a physical RAM configuration change, not a response to performance results.

"test_pilot" and "test_profile_check" pass.

Final PR #97 ruling

Manifest e: APPROVED.

Manifest-baseline closure: COMPLETE.

PR #97: APPROVED AND MAY MERGE.

After merge:

1. File this approval in the handoff records.
2. Update STATUS/AGENTS to mark PR #97 approved and the manifest-baseline gate closed.
3. Merge PR #97.
4. Do not mix the next measurement into this PR.

---

NEXT AUTHORIZED TASK — R640 S2 PACING RE-DERIVATION + S6 PREREGISTRATION

Do not run the S6 regression yet.

The PR #75 S2 review contains an explicit unresolved physical-machine prerequisite:

«the pacing defaults are dev-VM hypotheses and MUST NOT be carried onto the R640 without re-derivation.»

Before any S6 data:

1. run "ckpt_walk_cost" on the R640;
2. measure the old 256-ID step cost on this hardware;
3. derive R640-appropriate pacing values;
4. preregister those values before the physical regression.

This is now the next authorized work.

A. Freeze the code identity first

After PR #97 merges:

- update the R640 checkout to merged "main";
- record the exact resulting HEAD SHA;
- do not make product-code changes during the characterization;
- use manifest e:
  "bench/manifests/r640-main-20261111.txt".

This SHA is the candidate S6 regression SHA unless this characterization finds a blocker requiring a separately reviewed change.

B. Verify the host before measurements

Reapply and record:

- governor "performance" on all 72 CPUs;
- "kernel.numa_balancing=0";
- manifest-e RAM population unchanged;
- data SSD still model "ARFX3840S5xnFTRI";
- "/srv/snc-data" still ext4 with the recorded mount options.

There is one small ambiguity in manifest e that should be resolved now, before load:

manifest d showed about 2.2 GB used on "/srv/snc-data"; manifest e shows only 44 KB.

Do not assume why.

Record current filesystem identity and creation metadata, e.g.:

```
lsblk -f /dev/sdb
sudo blkid /dev/sdb1
sudo tune2fs -l /dev/sdb1 | grep -E \
  'Filesystem UUID|Filesystem created|Block size|Inode size|Filesystem features|Reserved block count'
```

If the filesystem was recently recreated, verify there is no ext4 lazy-initialization/background initialization activity that could contaminate storage measurements.

This is an evidence check only. Do not change filesystem configuration.

C. R640 "ckpt_walk_cost"

Run the repository's "bench/ckpt_walk_cost" characterization on the R640.

Keep raw output.

At minimum capture:

- old legacy 256-ID step time;
- per-ID walk cost;
- batch-16 cost;
- paced behavior at several candidate budgets;
- run-to-run variation;
- CPU placement;
- NUMA binding;
- exact binary SHA/build identity.

The purpose is not to optimize for maximum throughput.

The purpose is to establish a hardware-safe pacing envelope.

D. Derive the R640 pacing values

The R640 values must preserve the S2 intent:

- pacing must not allow materially more checkpoint monopolization per loop than the legacy 256-ID step;
- "walk_min" must provide non-zero progress;
- "walk_max" must protect foreground execution;
- "walk_ceiling" may permit catch-up but must remain a foreground-protection ceiling;
- batch size controls overshoot and must be accounted for;
- no value may be selected because it happens to produce a favorable S6 result.

Record the derivation numerically.

Do not silently inherit:

- 20 µs min;
- 100 µs normal max;
- 250 µs catch-up ceiling

unless the R640 measurement independently supports them.

E. Finish S6 preregistration before S6 data

Create a short S6 preregistration record that freezes:

- merged post-PR97 SHA;
- manifest e SHA-256;
- R640 pacing values derived above;
- build type;
- CPU/NUMA allocation;
- scratch/data/output isolation;
- Profile A;
- "--checkpoint-s 60";
- exact S6 cells;
- exact pass/fail rules.

Carry forward the DESIGN-R section 9 primary cell:

"436,646 ops/s", open loop, Profile A, 60-second window, 3 reps.

Same-machine/same-SHA comparators remain:

- 436,646 without checkpoint, 3 reps;
- 2,183,233 with checkpoint, 60-second window, 3 reps;
- closed-loop checkpoint capacity.

Before running anything, resolve the one remaining wording choice in section 9:

«total p99 ≤ 1.5× same-SHA no-checkpoint median or paired reference»

Choose same-SHA no-checkpoint median as the preregistered reference.

Reason: it is already an explicit comparator, avoids pair-order/timing ambiguity, and gives one deterministic reference for all three primary reps.

Do not change that choice after seeing S6 data.

Also record the active checkpoint target/max-age configuration explicitly. Do not leave “preregistered hard maximum age” implicit.

F. Stop after preregistration

Return:

- R640 walk-cost evidence;
- derived pacing values and derivation;
- filesystem identity check;
- exact post-merge SHA;
- S6 preregistration document.

Do not run S6 yet.

Return that preregistration for independent review and explicit GO.

---

Still not authorized

- S6 measurements before preregistration review;
- CP-1A rerun;
- comparator systems;
- CRITERIA freeze;
- S5/ioprio unless S6 evidence later justifies it;
- B2 incremental begin-snapshot;
- Agent-SNC implementation;
- Rust rewrite;
- DPDK/VPP.

The sequence is now:

merge PR #97 → R640 pacing re-derivation → freeze S6 preregistration → review/GO → S6 regression → review → only then CP-1A from the beginning.

Standing rules:

Always verify, never assume.

Fix confirmed defects on site.

Never choose a physical-machine tuning value after seeing the regression it is supposed to judge.

Keep historical manifest-d evidence separate from manifest-e evidence.
