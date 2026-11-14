// DIAG (never merge): the WAL's write pattern outside SNC. One io_uring
// O_DIRECT|RWF_DSYNC 4 KiB write into fresh fallocated space every 20 ms;
// mode "spin" reaps like the shard loop (peek + sched_yield, pinned to a
// CPU; "spinget" also enters the kernel with GETEVENTS at each yield;
// "spinfree" is not pinned; "spinaff" lets io-wq run on every CPU;
// "pinwait" is pinned but blocks), mode "wait" blocks in io_uring_wait_cqe. Logs any write > 200 ms,
// and every 60 s a count line.
#define _GNU_SOURCE
#include <fcntl.h>
#include <liburing.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(int argc, char** argv) {
  const char* path = argv[1]; const char* m = argv[2]; int spin = strncmp(m, "spin", 4) == 0; int pin = strcmp(m, "spinfree") != 0 && strcmp(m, "wait") != 0; int get = strcmp(m, "spinget") == 0; int aff = strcmp(m, "spinaff") == 0; int cpu = argc > 3 ? atoi(argv[3]) : 0;
  if (pin) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); sched_setaffinity(0, sizeof(s), &s); }
  int fd = open(path, O_RDWR | O_CREAT | O_DIRECT, 0644);
  if (fd < 0) { perror("open"); return 1; }
  struct io_uring ring; if (io_uring_queue_init(256, &ring, 0)) { perror("uring"); return 1; }
  if (aff) { cpu_set_t a; CPU_ZERO(&a); for (int c = 0; c < 64; ++c) CPU_SET(c, &a); printf("URING_PROBE spinaff iowq_aff=%d\n", io_uring_register_iowq_aff(&ring, sizeof(a), &a)); }
  void* buf; posix_memalign(&buf, 4096, 4096); memset(buf, 0, 4096);
  const long seg = 64L << 20; long off = 0, n = 0, slow = 0; double last = now();
  for (;;) {
    if (off % seg == 0) { ftruncate(fd, 0); posix_fallocate(fd, 0, seg); fsync(fd); off = 0; }
    struct iovec iov = {buf, 4096};
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_writev2(sqe, fd, &iov, 1, off, RWF_DSYNC);
    double t0 = now(); int sr = io_uring_submit(&ring);
    if (sr < 1) printf("URING_PROBE %s submit=%d\n", argv[2], sr);
    struct io_uring_cqe* cqe; double ly = t0;
    for (;;) {
      if (spin) {
        if (io_uring_peek_cqe(&ring, &cqe) == 0) break;
        if (now() - ly > 50e-6) { if (get) io_uring_get_events(&ring); sched_yield(); ly = now(); }
      } else if (io_uring_wait_cqe(&ring, &cqe) == 0) break;
    }
    double d = now() - t0; int res = cqe->res; io_uring_cqe_seen(&ring, cqe); off += 4096; ++n;
    if (d > 0.2 || res != 4096) { ++slow; printf("URING_PROBE %s %.1f write %.3f s res=%d\n", argv[2], t0, d, res); fflush(stdout); }
    if (now() - last > 60) { printf("URING_PROBE %s alive n=%ld slow=%ld\n", argv[2], n, slow); fflush(stdout); last = now(); }
    usleep(20000);
  }
}
