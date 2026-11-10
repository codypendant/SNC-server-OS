# SNC Server OS — PR #89 D.1 Closure Review Handoff

**PR:** #89 — `D.1 closure: witness "primary up" fix, D.1 GAUGES, P1 matrix`  
**Reviewed head:** `dad1477fe8f9234795c1fb3095a174ec29bcfef5`  
**Merged as:** `417d67253b426988362f5a9ef0c5fbec50486cb2`

## Decision

**PR #89: APPROVED.**  
**FOLLOW-UP INTAKE D.1: COMPLETE.**

The three closure conditions from the PR #87 review are satisfied:

1. the confirmed witness `primary up` logging race was fixed on site with deterministic regression coverage;
2. the preregistered P1 matrix was run and reported without using the dev VM to tune production defaults;
3. D.1 backpressure state is now visible through GAUGES/admin rather than remaining internal-only.

No further D.1 redesign is required before advancing.

---

## 1. Witness logging race — accepted

The previous behavior printed `primary up` only when the witness's own proposal path learned the space. If another witness's proposal was chosen first, this witness could learn the chosen state from its peers and never print the line, causing a harness waiting for that operational signal to time out.

The fix adds one `up_reported` state and prints `primary up` exactly once from whichever path first observes the chosen space actually being served:

- this witness's initial proposal path; or
- the later serving-primary polling path after learning the state from a peer.

The fix does not change failover eligibility, Paxos state, term selection, readiness semantics, or promotion decisions. It corrects the observable signal only.

### Regression evidence

`test_health` T12 deliberately makes witness 2 unable to reach either node while it still participates with its witness peers. It verifies:

1. witness 2 learns `ha/0` from peer consensus first;
2. it has not printed `primary up` while blind;
3. its node paths are restored;
4. it then prints `primary up` exactly once;
5. the other witnesses also print it exactly once.

Evidence states:

- old witness: failed T12 3/3;
- fixed witness: passed T12 3/3;
- T12 also passed inside the full validation suites.

**Accepted.**

---

## 2. D.1 observability — accepted

PR #89 adds 33 `intake_*` gauges per shard and exposes them through admin `GAUGES`.

The published view includes the effective configuration:

- H (`intake_wal_cap_bytes`);
- R (`intake_need_bytes`);
- W (`intake_window`);
- X (`intake_exchange_budget`);
- L (`intake_link_hold_max`);
- WAL gate on/off;
- exchange gate on/off;
- owner gate on/off.

It also publishes runtime state/counters including:

- gate open state and gated applies;
- exchange refusals;
- owner refusals;
- exchanges live / peak / started / ended;
- staged now / peak;
- window outstanding / waits;
- link held now / peak / paused / pause count;
- resume active / owed;
- resume passes / triggers / coalesced / restarts / objects.

The implementation deliberately computes the observable exchange-live value without mutating or relying on the cache used by `exchange_full()`, so the observability path does not become part of the admission decision.

### Tests

- `test_intake` F12 checks intake gauges against shard counters while X is saturated and again after drain.
- F8 checks link-hold gauge behavior.
- G2 confirms rollback exposes every relevant gate as disabled.
- `test_gauges_admin` T8d verifies the admin representation with W=16 and X=256 environment overrides.

**Accepted.**

---

## 3. P1 performance regression matrix — accepted

The matrix was preregistered before execution.

### Matrix

- W = {16, 64, 256}
- X = {256, 1024, 4096}
- no-window: W=0, X=1024
- rollback: every D.1 gate disabled while contract checks remain active

Two workloads:

- uniform 100% transfers;
- all-cross-shard transfers, where every transfer creates one exchange.

Five interleaved repetitions per cell produced **110/110 valid runs**.

### Interpretation

The dev VM is explicitly treated as a regression check, not target-hardware characterization.

The default W=64 / X=1024 cell remained within rollback's run-to-run range.

Observed effects:

- X=256 is roughly 5% below rollback on uniform transfers and rejects approximately 0.03-0.04% there;
- on the all-cross workload, X=256 rejects roughly 3.9-5.0%;
- default X=1024 / W=64 rejected approximately 0.15% of cross-shard attempts in this dev-VM run;
- X=4096 produced no exchange-budget refusals in these runs;
- p99 medians were approximately 819-983 us across cells;
- no production H/W/X/L default was changed from this evidence.

This is exactly the correct interpretation. The data demonstrates that the hot-path mechanism has no obvious dev-host regression at the current default while also showing that X materially changes overload behavior when made small. Final tuning remains a target-hardware question for the preregistered R640 campaign.

**Accepted.**

---

## 4. Full validation — accepted

Final-code evidence at `d3495c9`:

- Release: **45/45**;
- ASan + UBSan: **45/45**, zero sanitizer reports;
- TSan: **45/45**, zero ThreadSanitizer warnings;
- P1: **110/110 valid runs**.

The final PR head is one commit beyond `d3495c9`, containing only evidence/AGENTS/STATUS material; no engine or runtime code changed after the final validation head.

The earlier TSan attempt is retained in evidence rather than hidden. Its failure was a test-only seqlock-read helper that sampled the published gauge once and could fail to obtain a consistent snapshot under TSan. The helper was changed to retry until it obtains a whole snapshot, then the full TSan suite passed.

This evidence handling is acceptable.

---

## 5. D.1 status

D.1 now has:

- destination H WAL apply bound;
- W per-pair intra-node intake window;
- dynamic per-invocation exchange reservation;
- X exchange budget;
- replay-before-X semantics;
- same-shard transfer reservation of zero;
- reply-only follow-up contract;
- enforced `max_emits` / follow-up registration rules;
- owner-side follow-up backlog gate;
- bounded node-link receive holding L;
- coalesced/paged resume under X;
- resume-LSN durability protection;
- negative tests;
- crash/recovery tests;
- full sanitizer suites;
- production-visible observability;
- dev-host performance regression evidence.

**D.1 is complete.**

---

## 6. Next authorized work: D.2

**D.2 is now the next authorized SNC slice.**

D.2 closes the remaining cross-node sender-bound gap. D.1 deliberately ends a local exchange when an item leaves the node, so it does not structurally bound the remote exchange across nodes. D.2 must provide that accounting/recovery semantics.

The existing design direction is the cross-node exchange-end protocol described in `FOLLOWUP_INTAKE_DESIGN.md`: cross-node completion/end information, generation handling across reconnects, and wire-version handling.

Because D.2 changes the node-link wire protocol and distributed accounting, keep it as its own reviewed slice.

D.2 acceptance must prove at minimum:

- structural cross-node B2 bound;
- no double-end accounting across reconnect/generation changes;
- no leaked exchange authority/accounting on link loss;
- no cyclic deadlock introduced by remote completion accounting;
- correct recovery/resume behavior;
- compatibility/version behavior for the node-link wire change;
- bounded memory under a stalled remote destination;
- live response/control progress remains unaffected;
- negative tests discriminate broken end accounting and generation handling;
- Release, ASan+UBSan, and TSan clean.

Do not treat D.1's dev-VM P1 numbers as production tuning for D.2 or the eventual R640 campaign.

---

## 7. Scope still forbidden

Until D.2 is implemented, reviewed, and approved:

- no S6;
- no R640 campaign;
- no CP-1A rerun;
- no comparator campaign;
- no B2 begin-snapshot work;
- no opportunistic S5 work unless separately justified by evidence.

---

## Final ruling

**PR #89 APPROVED.**  
**D.1 COMPLETE.**  
**D.2 AUTHORIZED AS THE NEXT SEPARATE REVIEWED SLICE.**

No D.1 architectural reopening is required from the evidence reviewed here.

Continue the standing SNC process:

> Always verify, never assume.  
> Fix confirmed defects on site.  
> Correctness first.  
> Preserve structural boundedness.  
> Keep slices explicit and independently reviewable.
