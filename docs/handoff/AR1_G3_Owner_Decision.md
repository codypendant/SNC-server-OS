# Owner decision: AR-1 G3 counted runs

On 2026-11-23 at 00:21 UTC, cody tapped "Go for G3" on a decision card the coordinator posted in the project chat
(card `cmsg_01AUrWNtcahq5pdcaSkZF517FN4LTw8uYpeAVWMJhAgQMV`). The card asked "Give the go for the AR-1 G3 counted
runs?", and the chosen option said: "Counted runs start on the dedicated VM under the accepted preregistration, and
I ping you with the results." Codex had recommended GO (`docs/handoff/PR185_Codex_Updated_Review.md`).

- **AUTHORIZED:** the G3 counted campaign of `docs/AR1_PREREG.md` revision 3 (accepted 2026-11-22), and nothing
  else. Its conditions:
  - it runs on the dedicated VM;
  - the environment is the hash-pinned one;
  - the implementation SHAs are frozen before any counted run (5.5);
  - the full Phase 0 runs first, at 20 runs per check, as a hard gate;
  - a Phase 0 failure or an UNRESOLVED result stops the campaign under section 9.
- **PROPOSED numbers:** they take the values written in the prereg, as in the G2 go.
- **NOT AUTHORIZED:**
  - Track A;
  - S2 or S3 work;
  - any production dependency;
  - the product-gate steps of section 11, of which step 1 needs its own authorization.
