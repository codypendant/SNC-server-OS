# Owner decision on AR-1 G2 (2026-11-21)

## What cody answered

The coordinator's post in the project chat, 2026-11-21 19:21 UTC, verbatim:

> PR #184 is being merged now with Codex's sign-off on the preregistration. The next step needs your explicit go,
> since Codex's recommendation alone doesn't authorize it.
>
> **G2: build the AR-1 experiment and do trial runs.** That means the isolated AR-1 code, uncounted bring-up, and
> proving the checker catches the planted bugs. The counted runs (G3) stay on hold for a separate go after you see
> the G2 evidence.
>
> My recommendation is yes, with two defaults that your yes would also accept:
> - **Numbers:** the seeds, run caps, latency budget and thresholds as proposed in the document.
> - **Where it runs:** a dedicated cloud VM, which costs a little but keeps results repeatable and your machines
>   untouched.

cody's reply, 2026-11-21 19:52 UTC, verbatim (with Codex's revision 2 review re-pasted just before it, filed in
`PR184_Rev2_Codex_Review.md`):

> go

## What is decided

- **G1 and G2** (`docs/AR1_PREREG.md` revision 2, section 14 item 1): the preregistration is approved; AR-1 code
  under `experiments/ar1/` and the manifest under `bench/manifests/` are allowed; uncounted bring-up (at most 2 seeds
  per point, reported, then discarded) and the checker-adequacy runs are allowed.
- **Numbers:** the PROPOSED values in the preregistration, as written (section 14 item 3).
- **Where:** a dedicated cloud VM (section 2).

## What is not decided

- **G3**, the counted runs. It needs a separate GO after cody has seen the G2 evidence.
- Any production dependency, durability contract, product demo (package 7.2), or S2 or S3 work.
- Custom replication, raft-rs, the R640.

Codex's conditions for G2 carry over: any failure of the durability oracle, fault-point reachability, DBOS
integration assumptions or mutation adequacy is reported as UNRESOLVED, fixed on site and re-reviewed before going
further.
