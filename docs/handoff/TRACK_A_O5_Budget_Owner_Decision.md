# Track A: owner decisions on SNC O-5 and the VM budget (2026-11-28)

## What cody said
In the project chat, the coordinator wrote at 00:32:02 UTC:

> The corrected Track A plan is in [PR 199](https://github.com/codypendant/SNC-server-OS/pull/199), and it needs two yeses from you before it goes to Codex.
>
> **1. SNC and approval events (O-5).** Codex was right: our frozen SNC code has no way to receive an approval event at all. The plan's rules taken literally would settle Track A before anything runs. I recommend judging SNC on O-1 to O-4 and recording O-5 as unsupported, so the gap counts against SNC. The alternatives are writing new SNC runtime code, which is off-limits right now, or a test where the competitors can't change the result.
>
> **2. VM budget.** I recommend 30 VM-hours in total, roughly $8 on Vultr, because we're now building all four competitors.
>
> Say "yes to both" and I'll send it to Codex.

cody, at 00:56:24 UTC, in reply: "Yes."

## What is decided
- **O-5, option A** of [TRACK_A_EXECUTION_PLAN.md](../TRACK_A_EXECUTION_PLAN.md) section 3: SNC's frozen arm is judged
  on O-1 to O-4. O-5 is recorded as UNSUPPORTED and counted against SNC's layer in criteria 1 and 3. A09 and A10 are
  not run on SNC. No SNC event ingress is built.
- **VM budget:** 30 VM hours in total for counted runs, adequacy, performance and VM bring-up; at most 3 h per run set
  and 5 min per run. A cap hit stops the item and the rest is UNRESOLVED. The 12 agent-day cap is unchanged.

## What is not decided
- CRITERIA.md is not frozen. The freeze waits for Codex's review of execution plan revision 2.
- Everything listed as not decided in [TRACK_A_PA_Owner_Decision.md](TRACK_A_PA_Owner_Decision.md) stays not decided.

## After Codex's review of PR #199 (2026-11-28)
Codex held the freeze: option A changes the experiment, so it cannot be a reading of the outcome rows
([PR199_Codex_Review.txt](PR199_Codex_Review.txt)). The plan now applies the rows as written, with SNC's O-5
UNSUPPORTED (so row 1 applies to the frozen candidate). It reports SNC on O-1 to O-4 as a separately labelled
diagnostic. The coordinator's reading is that cody's yes above covers that diagnostic, not a change to the rows. Using
option A for the formal outcome would be an amendment that needs cody's explicit yes, and it is not requested.
The budget yes stands, and those numbers are hard caps.

## After Codex's review of PR #201 (2026-11-28)
Codex held that the reading above reinterprets cody's yes after the fact ([PR201_Codex_Review.txt](PR201_Codex_Review.txt)).
That reading is withdrawn. cody's yes was to option A. Because option A changes the preregistered rules, cody is asked
for an informed choice: keep the original rules (the formal outcome is row 1 before any run), or adopt option A as a
formal amendment. OWNER CHOICE PENDING. The answer is recorded here.
