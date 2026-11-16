# TLA+ models for SNC-AFR-001

These are verification artifacts for `docs/AFR_001_SAFETY_ANALYSIS.md` and
`docs/AFR_001_VERIFICATION_CLOSURE.md`. They are not product code, and they authorize no implementation.

| File | What it checks |
|---|---|
| `Authority.tla` | Claim L: `OneAuthority` (at most one node holds valid authority) and `NoStaleStandby` (no standby read lease from an older term outlives the start of a newer primary's authority), plus `R3Lemma` (the simplified R3 rule implies the full one). Rules R1-R5, R7-R9. Time is countdown timers |
| `Combined.tla` | Authority and durability together: `OneAuthority`, `AckedPreserved`, `EntryUnique`, `LogMatching`, `AckedAtPrimary` across promotion, acknowledgement, membership changes and restarts. Its abstractions are listed in its header |
| `Log.tla` | Claim D: `AckedPreserved` (I4), `EntryUnique` (I1), `LogMatching` (I2), `StateMatchesLog` (I5), `PositionedPrefix` (I3) and `HistoryUnique` (G-15: a `(term, LSN)` never identifies two different entries over the whole history). Rules R1, R2, R6 |
| `Authority.cfg`, `Log.cfg`, `Combined.cfg` | The fixed protocol. Every fix is on, and case 5c is excluded (R7 forbids it) |
| `MC_Authority.tla` | Defines the witness symmetry used by `Authority.cfg` |
| `RESULTS.md` | Every run, its configuration and its result, including counterexamples and modelling limits |

## Running

Tools: `tla2tools.jar` from the tlaplus GitHub release v1.8.0. It is not stored in the repo. The build reports
itself as `TLC2 Version 2026.11.15.002146 (rev: bc1b506)`, with sha256
`7e53aa636dac8a3cb0533f343115c0548e03fc0ce5edc7739becf58d3d0fb84f`. Java 21.

```sh
# Authority: run through MC_Authority so the SYMMETRY definition is in scope
cp Authority.cfg MC_Authority.cfg
java -XX:+UseParallelGC -cp tla2tools.jar tlc2.TLC -workers 4 -deadlock -config MC_Authority.cfg MC_Authority.tla
java -XX:+UseParallelGC -cp tla2tools.jar tlc2.TLC -workers 4 -deadlock -config Log.cfg Log.tla
java -XX:+UseParallelGC -cp tla2tools.jar tlc2.TLC -workers 4 -deadlock -config Combined.cfg Combined.tla
```

- `-deadlock` turns off deadlock checking, because bounded models stop by design.
- `Combined.cfg` as committed finishes exhaustively (about 15M states). `Authority.cfg` (every scenario, MaxTerm 3,
  one restart, two fences) and `Log.cfg` (MaxIds 3) do not finish in reasonable time. The exhaustive runs in
  `RESULTS.md` use smaller bounds, stated with each run.
- TLC keeps its state queue on disk under `states/`. It can reach several GB, so delete it after each run.

## Switches

Each `Fix*` constant switches on one fix from the analysis. Setting it to FALSE must let TLC reproduce the failure
that fix prevents. `AllowOp5c = TRUE` admits case 5c, which R7 now forbids. `Compete = TRUE` (Authority) lets two promotions race for the next version. In `Combined.tla`, `FixGrantTerm`, `FixLearnStep`, `FixAinS` and `FixW1Term` remove the indirect fencing guards, and `OpInside` and `OpOutsideDeg` add operator promotion. `FixBootRead = FALSE` lets a fenced node that comes back (`Unfence`) grant at its old term. `R3Full = TRUE` serves on all three R3
bounds instead of `learned + W` alone. `KeepS = TRUE` (Log) relaxes R2 so that promotion keeps the rest of S
unreconciled; it exists to exercise the KIP-279 iteration (M1), not as a protocol option.

`OpInside`, `OpOutside`, `Degrade`, `Standby`, `MaxRestarts` and `MaxFences` isolate one scenario at a time, so a
counterexample shows the intended defect rather than an unrelated one.
