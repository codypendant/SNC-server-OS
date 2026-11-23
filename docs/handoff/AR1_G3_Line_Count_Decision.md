# AR-1 G3: how plain DBOS step 2's lines are counted (owner decision)

- **Decision:** cody chose "Added lines only" on 2026-11-23 at 03:29 UTC (decision card in the AR-1 thread).
- **Rule:** L (300, section 8) is compared with the logical lines that are added in the black-formatted diff from
  plain DBOS step 1 to step 2. Blank and comment lines are not counted, and neither are removed lines.
- **Code:** `experiments/ar1/ar1/g3.py`, `lines()`. The added-plus-removed total is not used for the outcome.
- **Numbers:** when the card was posted, the diff had 254 added lines. After the review fixes to step 2 it had
  about 270. The number that counts is the one the driver measures on the frozen commit, and it goes in the G3 report.
