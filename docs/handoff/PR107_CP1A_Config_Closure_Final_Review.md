# PR #107 CP-1A configuration closure: final review

Filed verbatim from cody's message in the project thread, 2026-11-11 20:39 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #107 FINAL REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#107 — CP-1A configuration closure"

Reviewed head:

"807d7795ca3424e8a203b826a312afd5c017138e"

Decision

PR #107: APPROVED.

CP-1A PROFILE-A CONFIGURATION CLOSURE: COMPLETE.

MERGE AUTHORIZED.

CP-1A: STILL HOLD.

---

What is approved

The R640 Profile-A checkpoint configuration is now:

```
--checkpoint-s 60
--ckpt-pacing on
--ckpt-walk-batch 16
--ckpt-walk-min-us 20
--ckpt-walk-max-us 65
--ckpt-walk-ceiling-us 155
--ckpt-walk-step-us 5
--ckpt-target-ms 30000
--ckpt-max-age-ms 60000
--ckpt-io dsync
--ckpt-window 1
--ckpt-sync-tokens 0
```

"bench/pilot_plan.json" is the command-value source.

"bench/profile_check.py" independently enforces those exact Profile-A values with no deviation route.

Profile C remains unchanged.

B and D do not inherit these R640-specific values.

No pilot methodology changed.

---

Accepted validation

- "test_ckpt_cli": PASS
- "test_profile_check": PASS
- "test_pilot": PASS
- "test_s6": PASS
- "test_baseline_ha": PASS
- "test_lab_witness_guard": PASS
- "test_net_bench_rate": PASS
- closure mutations: 9/9 caught

The command-construction proof shows Profile A receives all 12 flags.

No real CP-1A run occurred.

---

Approved plan identity

New "bench/pilot_plan.json" SHA-256 recorded by the closure:

"568e9874e61480647c600fe6c52f44965331081d1d82f7a5f01b2875a87968c7"

Approved manifest e remains:

"768056bbee1d7f178ed0be1c195b821f0638a1de28a4aca1ea0afdf5444c27b6"

---

Finding from final execution-path review

"bench/pilot.py" currently records:

- "git_head"
- "plan_sha256"

but does not compare either against a frozen approved value.

A clean committed tree at an unintended newer commit would therefore be measured rather than refused.

That is not sufficient for the physical campaign freeze.

---

NEXT AND ONLY AUTHORIZED SLICE

CP-1A freeze interlock

After PR #107 merges, obtain its exact merge SHA.

Add:

"bench/cp1a_frozen.json"

Recommended contents:

```
{
  "runner_commit": "<PR107 merge SHA>",
  "plan_sha256": "568e9874e61480647c600fe6c52f44965331081d1d82f7a5f01b2875a87968c7",
  "manifest_sha256": "768056bbee1d7f178ed0be1c195b821f0638a1de28a4aca1ea0afdf5444c27b6",
  "go_record": null
}
```

"go_record" must remain null until the final independent GO.

---

pilot.py enforcement

For a real pilot, not "--quick", require all of the following before any benchmark process starts:

1. "runner_commit" is set.
2. "runner_commit" is an ancestor of HEAD.
3. No unauthorized code/configuration changed after "runner_commit".
4. Actual "bench/pilot_plan.json" SHA-256 equals frozen "plan_sha256".
5. Actual manifest SHA-256 equals frozen "manifest_sha256".
6. "go_record" is set.
7. The referenced GO-record file exists.
8. Existing clean-tree/build/governor/NUMA/isolation checks still pass.

Before GO, requirements 6/7 must intentionally make a real pilot refuse.

---

Allowed post-freeze paths

After "runner_commit", allow changes only to coordination/evidence locations needed to file GO and results, for example:

- "docs/"
- "AGENTS.md"
- "bench/cp1a_frozen.json"
- "bench/results/"

Do not allow changes to:

- "bench/pilot.py"
- "bench/pilot_plan.json"
- "bench/profile_check.py"
- tests affecting campaign behavior
- C++ product code
- workload or methodology files

A changed campaign mechanism must require another review/freeze.

---

Required tests

Add deterministic tests proving:

- unset "runner_commit" refuses;
- wrong runner commit / non-ancestor refuses;
- wrong plan hash refuses;
- wrong manifest hash refuses;
- missing GO refuses;
- GO path named but file absent refuses;
- complete correct freeze passes;
- unauthorized post-freeze "bench/pilot.py" change refuses;
- docs-only GO filing is allowed;
- quick mode remains usable for development and does not masquerade as evidence.

Also test that the runner still records the actual:

- HEAD;
- plan SHA;
- manifest SHA;
- binary SHA values.

---

Pre-GO evidence

After implementation:

- run the relevant Python gates;
- run a real-mode preflight only, or equivalent test path, demonstrating refusal before any benchmark process because "go_record" is null;
- do not run CP-1A.

Return the freeze-interlock PR for independent review.

---

After that PR merges

The sequence will be:

1. record exact freeze-interlock merge SHA if required by its design;
2. file the explicit CP-1A GO record;
3. set "go_record";
4. independently verify the GO closure;
5. run the full CP-1A from the beginning.

No manifest-d performance data is reused.

No partial old pilot state is resumed.

---

Still unauthorized

Until final GO:

- CP-1A
- competitor performance runs
- CRITERIA freeze
- REFINE round 2
- S5
- B2
- adaptive write-window controller
- global W-default change
- Agent-SNC
- Rust
- DPDK/VPP

Final ruling:

PR #107 APPROVED.

MERGE IT.

Configuration closure is complete.

CP-1A remains HOLD only for the final machine-enforced freeze/GO interlock.
