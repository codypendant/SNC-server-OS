# S3-D writeback micro-characterization (dev VM, 2026-11-08)

Architectural evidence for docs/S3D_WRITEBACK.md. Not campaign evidence:
no R640, no CP-1A, no comparators, and no winner chosen on throughput.

- Bench: `bench/ckpt_writeback_char.cpp` (target `ckpt_writeback_char`),
  built at 0b11b30.
- Matrix: `run.sh build DATA_DIR runs.txt`, 17 cells x 3 reps in rotated
  order, 51 runs, 0 failed, 05:08:30Z-05:16:31Z. Every run is in
  runs.txt, in the order it ran; the header lines record the kernel,
  filesystem, scheduler, write cache and dirty thresholds.
- Probe: `ckpt_writeback_char probe DIR /dev/shm` (output quoted in
  docs/S3D_WRITEBACK.md section 4).

Fields: write_ms / sync_ms = part write phase and final fdatasync time;
`<phase>_p99us` etc. = WAL record latency (open loop, from its 250 us
slot) for records due in that phase; `<phase>_dirty_max_mib` = max of
/proc/meminfo Dirty+Writeback sampled every 5 ms; `<phase>_flushes` =
WAL writes (group commits); writer_cpu_ms = checkpoint writer threads'
user+system CPU.

A note on the partial commit: an earlier commit on this branch holds a
partial runs.txt, taken while the matrix was still running, because a
hook required the working tree to be clean. The final runs.txt replaces
it; nothing was rerun or removed.
