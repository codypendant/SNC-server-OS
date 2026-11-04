# SYSTEM_REFERENCE_REVIEW.md (template)

Copy to `<SYSTEM>_REFERENCE_REVIEW.md`. Rules: [README.md](README.md)
section 1.5 and the program's engineering rules
([REFERENCE_ARCHITECTURE_PROGRAM.md](REFERENCE_ARCHITECTURE_PROGRAM.md)).

## 0. Sources

| Source | Version / commit / date | License | Read for |
| --- | --- | --- | --- |
| | | | |

Label every claim **SHIPPED** (in the pinned code), **DOCUMENTED** (docs
only), **ROADMAP**, or **PAPER** (published, not checked in code).

## 1. What the system is, in one paragraph

## 2. Teardown

One subsection per question. Each gives the reference system's answer,
the assumptions it relies on, and SNC CURRENT from
[README.md](README.md) section 2.

State authority · Identity · Execution ownership · Atomicity ·
Acknowledgement · Retry/deduplication · Failure ambiguity · Scheduling ·
Dormancy · Migration · Backpressure · Locality · Timers · External
effects · Code upgrades · Security · Observability

## 3. Comparison matrix rows

| Mechanism | SNC CURRENT | SNC INTENDED | Reference system | Better idea | Adopt? | Why / why not | Test required | Source version |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |

## 4. Adoption candidates

Only mechanisms worth proposing. For each: the SNC contract it changes
(CORE_CONTRACTS section or invariant), the failure model rows it affects
(FAILURE_MODEL), compatibility consequences, and a deterministic
validation plan. Each one is a bounded design slice that needs review
before anything is implemented.

## 5. Where SNC is not better

Workloads where the reference system's answer is simpler or stronger,
and what SNC should do about it (improve, simplify, or leave out of Core).
