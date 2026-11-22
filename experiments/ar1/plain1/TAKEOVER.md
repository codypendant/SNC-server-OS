# Takeover in the plain DBOS arm

A takeover is a second app instance (executor `e2`, its own system database `ar1_sys_e2`) that starts a new workflow
`T:e<n+1>` for the same task and runs it from the start. DBOS has no cross-instance ownership, so nothing connects it to
`T:e<n>`.

- Its first transaction sets `plain.agent.epoch` to n+1 (commit `TAKEOVER`). Nothing reads the epoch back.
- Tool call ids and idempotency keys are `<workflow id>:<tool>`, so the new epoch uses new keys.
- Memory, task and call rows are written without compare-and-set; the old workflow, if alive, keeps writing.

Known limits: no fencing of the old activation (stale decisions commit), no rejection of its prepared or in-flight
calls, no outstanding set, T-I and T-N effects repeat across epochs, only T-F's own fence rejects a stale epoch.
No *-prep, *-disp, I-acc, I-unknown or TN-indoubt commits exist: a plain app has no such records.
