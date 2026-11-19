<!-- Filed 2026-11-19 from the project's shared files. The reviewer line is reworded and the reviewer's model name is replaced by "the coordinator" (no model identifiers
in the repository); nothing else is changed. Not an owner decision. -->

# Adversarial review of "SNC Architecture and Verification Assessment"

Reviewer: the coordinator, the project's coordinating session. Date: 2026-11-18.

**Basis and limits.** This rests on the assessment document, the project record of PRs #151 to #168, and the owner decisions. I did not re-read the design docs or the C++ from here, and I did not reproduce any model run. Linux kernel statements below are from memory and are marked "verify". Nothing here is a decision.

Tags: DEFECT (confirmed by our own record or by reasoning I am confident in), ASSUMPTION (unproven), EVIDENCE (missing), PERF, OPTIONAL. "Contract" says whether an accepted or frozen item would change.

## Verdict

The assessment is accurate and too comfortable. It lists the open items correctly, but it treats growing recovery complexity as something to organise, when our own record says it is a symptom. Six review rounds in one day each found a new break (recovery write over the only good copy, adjacent-slot misdirected write, lost-binding revival, lost-delete revival, generation ordering, reuse of the newest segment after a kill). Each fix added a mechanism. That pattern means the fault model is being discovered one counterexample at a time instead of being written down first. That is the thing to fix.

## Perspective 1: storage engineer

1. **No written fault model.** ASSUMPTION. Contract: none, it frames D-02.
   The design defends against lost writes, misdirected writes, lost directory entries and lost deletes, but nowhere is there one table saying which faults, how many per history, and which are assumed independent. Without it "safe" has no meaning and every reviewer invents a new fault. Resolution: one page, before any more design. Faults in scope, count, independence, and for each one: detect, repair, or out of scope.

2. **The watermark assumes its own write fails independently of the WAL write it guards.** ASSUMPTION. Contract: none yet; it decides whether D-02 option (c) can be accepted.
   Both live on one device. A device or cache that drops a flush can drop both, and then the watermark agrees with the damaged log and detects nothing. The watermark catches a single lost write. It does not catch a lying flush. State that limit, or place the evidence where the fault cannot reach it (the peer already learns each acknowledged position for free through replication).

3. **The proposed recovery order is wrong in two places.** DEFECT in the document's sequence, not in an accepted contract.
   Step 2 repairs metadata and step 3 completes truncations before step 4 validates the WAL, checkpoint and watermark. Completing a truncation destroys evidence, and a repair write before the whole picture is known is exactly the blocker found in PR #162. Resolution: recovery is two phases. Phase one is read-only: read everything, decide one outcome (proceed, complete an intent, refuse). Phase two writes, and every write is idempotent because recovery itself can crash. Step 6 also omits the rule we already own: a recovered node serves only after the witness places it in a term.

4. **No policy for write and fsync errors.** EVIDENCE. Contract: none.
   The document never says what SNC does when write or fsync returns an error. On Linux a failed fsync can report success on retry while the data is gone. Direct I/O avoids part of this, but directory fsyncs and renames are buffered metadata. Resolution: one rule, "any I/O error on a durability path is fatal to the process and is treated as a fault at restart; never retry", then check the C++ against it.

5. **Guarantees are filesystem-specific and the filesystem is not pinned.** ASSUMPTION. Contract: none.
   Rename durability, RENAME_NOREPLACE, directory fsync and direct-I/O behaviour differ between ext4 and XFS and by mount option, and the drive's write cache and power-loss protection matter too. Resolution: pin filesystem, mount options, kernel floor and drive cache setting as part of the storage contract.

## Perspective 2: Linux kernel behaviour (V8)

6. **The restart barrier is an empirical question and should be settled by a test, not a model.** EVIDENCE. Contract: none.
   Verify: a lock on the directory is dropped when the process dies, while in-flight direct writes hold their own reference to the data file, so a lock on the same open file as the I/O may outlive the process until those writes finish. That is kernel-version and io_uring-teardown dependent. Also verify whether truncate on ext4/XFS waits for outstanding direct I/O on the inode, which would give a simpler barrier. Resolution: read the source for the R640's kernel, then one throwaway test: hold writes in the block layer with a delay device, kill the process, watch what a second process can observe.

7. **Simpler direction: never reuse an inode the dead process could write.** OPTIONAL. Contract: could simplify proposed text, changes nothing accepted.
   New segment on every restart already follows this. The exposed files are the ones overwritten in place: the node record and the watermark. If those can also move to fresh inodes at restart, the lock stops being load-bearing.

## Perspective 3: simplicity (KISS)

8. **Nonce plus prev_nonce is a weak form of a known technique.** OPTIONAL, worth evaluating before D-06 freezes. Contract: would revise proposed D-02 text and OD-7 wording.
   etcd chains a running checksum through every record and across segment files. That gives the segment link, stale-segment refusal and a truncation binding (position plus chain value) from one mechanism at record granularity. Verify against etcd's WAL source, then compare honestly with R-CHAIN.

9. **Same-ID replacement may be unnecessary.** ASSUMPTION. Contract: touches OD-9, recorded today as a design direction on my recommendation.
   The ID exists to tell fresh from stale. Keeping it across a replacement removes that power, which is why the C15 case needed a model. If replacement only ever happens to the segment that becomes the newest (restart and truncation both look like that), there is no successor link to preserve and a fresh ID costs nothing. I want the design note to answer: when is a replaced segment not the newest? If never, drop same-ID.

10. **Which rollback fields can the witness cover?** OPTIONAL. Contract: none.
    A node must reach the witness before serving. The witness knows the current term, so term rollback is detectable without local redundancy. The four-slot record may only need to protect truncation and reseed intents.

11. **64-bit random IDs are fine.** No change. Collision odds are far below the drive's own error rate. The real question is the ID's source at early boot, one line in the format.

## Perspective 4: operator

12. **Every new check is a new reason to refuse to start, and the repair path does not exist.** EVIDENCE. Contract: none.
    "Refuse" is the answer to every detected fault. With two copies, one refusing node leaves the group at one copy. The way back is per-stream reseed, which the D-02 review found has no written procedure. Reseed is therefore the load-bearing repair path, not a side item. Resolution: finish it first, and give each refusal reason an operator action.

## Perspective 5: owner and product

13. **Cost is unmeasured and there is no plan B.** PERF. Contract: none; needs owner authorization.
    The watermark needs evidence that is durable separately from the write it guards, which points to a second flush per acknowledgement batch on each node. Sync standby already costs about 40% of throughput and the X-2 limit is a 10% drop. The document says to measure after the design is stable and never weaken safety. If the cost is large the design changes and the verification is wasted. Resolution: one coarse number now (cost of the extra synchronous write on the R640's drive), throwaway, no product code, clearly not X-2. Then decide whether to keep refining option (c).

14. **Prior art gap.** EVIDENCE. Contract: none.
    PR #161 compared eight systems. It did not cover "Protocol-Aware Recovery for Consensus-Based Storage" (FAST 2018), which is about exactly this: storage faults in a replicated log and why purely local handling is unsafe. Under the standing prior-art rule it should be read before D-02 is accepted. It bears directly on Codex's remark that the per-node conclusion follows from our frozen rules and is not inherent.

15. **Ambition level is undecided.** ASSUMPTION. Contract: owner decision on D-02's framing.
    All of this is generic replicated-log durability. None of it tests what makes SNC different. PR #161 found that no surveyed system detects these faults on one node, so SNC is trying to beat the industry here before proving its own thesis. Option worth an explicit decision: accept an industry-level fault model for the first Rust version with format fields reserved for the detectors, and stage the stronger guarantee. That is a stated choice, not a silent weakening.

16. **Unreviewed fixes are accumulating.** DEFECT in process. Contract: none.
    Almost every PR today merged with "the fixes have not been re-reviewed". The normative text is now spread over many documents. Before acceptance there must be one consolidated storage contract that supersedes them, reviewed once as a whole.

## What needs a model and what does not

- Kernel test: V8 restart barrier.
- Written argument plus crash-injection test: spare header, temporary files, EEXIST.
- Reading the frozen text: the acknowledgement rule (I agree a recorded revision is the honest route).
- Small model, justified: checkpoint, floor and reclaim ordering (O-FLOOR).
- No model until the one-page recovery state machine exists: composition.

## Smallest decisive set before D-02 acceptance

1. Fault model table (item 1).
2. One-page two-phase recovery procedure (item 3).
3. V8 kernel reading and kill test (item 6).
4. Coarse watermark cost number (item 13).
5. Per-stream reseed procedure, reviewed (item 12).
6. Owner ruling on the acknowledgement rule revision.
7. O-FLOOR small model.
8. I/O error policy and pinned filesystem (items 4, 5).
9. Read the FAST 2018 paper and the etcd WAL chain, then confirm or simplify (items 8, 14).
