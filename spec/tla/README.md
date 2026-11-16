# TLA+ models for SNC-AFR-001

These are verification artifacts for `docs/AFR_001_SAFETY_ANALYSIS.md`. They are not product code, and they authorize
no implementation.

| File | What it checks |
|---|---|
| `Authority.tla` | Claim L: `OneAuthority` (at most one node holds valid authority) and `NoStaleStandby` (no standby read lease from an older term outlives the start of a newer primary's authority). Rules R1-R5, R7-R9 |
| `Log.tla` | Claim D: `AckedPreserved` (I4), `EntryUnique` (I1), `LogMatching` (I2), `StateMatchesLog` (I5) and `PositionedPrefix` (I3). Rules R1, R2, R6 |
| `Authority.cfg`, `Log.cfg` | The fixed protocol. Every fix is on, and case 5c is excluded |
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
```

- `-deadlock` turns off deadlock checking, because bounded models stop by design.
- The default `MaxTime = 7` (Authority) and `MaxIds = 3` (Log) do not finish exhaustively in reasonable time. The
  exhaustive runs in `RESULTS.md` use smaller bounds, which are stated with each run.

## Switches

Each `Fix*` constant switches on one fix from the analysis. Setting it to FALSE must let TLC reproduce the failure
that fix prevents. `AllowOp5c = TRUE` admits case 5c, which the analysis marks UNRESOLVED.

`OpInside`, `OpOutside`, `Degrade`, `Standby`, `MaxRestarts` and `MaxFences` isolate one scenario at a time, so a
counterexample shows the intended defect rather than an unrelated one.
