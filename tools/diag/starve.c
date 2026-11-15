// DIAG ONLY: can pinned busy threads starve CPU 0's softirq thread? No SNC code.
// starve T MODE SECS: T threads pinned to CPU 0 run MODE (spin | sleep: 50 us work then
// a 1 us nanosleep | yield: sched_yield every 50 us). Meanwhile a client thread pinned to
// CPU 0 does connect/send/recv/close to a server pinned to CPU 1 (like an admin ask),
// and reports asks per second, the slowest ask, and asks slower than 1 s.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static int mode, port; static atomic_int stop;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void pin(int c) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(c, &s); pthread_setaffinity_np(pthread_self(), sizeof s, &s); }
static void* spinner(void* a) {
  (void)a; pin(0); volatile unsigned long x = 0; double last = now();
  while (!stop) {
    x++;
    if (mode && now() - last >= 50e-6) {
      if (mode == 1) { struct timespec ts = {0, 1000}; nanosleep(&ts, 0); } else sched_yield();
      last = now();
    }
  }
  return 0;
}
static void* server(void* a) {
  int l = *(int*)a; pin(1);
  while (!stop) { int c = accept(l, 0, 0); if (c < 0) continue; char b[8]; recv(c, b, sizeof b, 0); send(c, "OK\n", 3, 0); close(c); }
  return 0;
}
int main(int argc, char** argv) {
  int T = atoi(argv[1]); mode = !strcmp(argv[2], "sleep") ? 1 : !strcmp(argv[2], "yield") ? 2 : 0; int secs = atoi(argv[3]);
  int l = socket(AF_INET, SOCK_STREAM, 0), one = 1; setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; bind(l, (void*)&a, sizeof a);
  socklen_t al = sizeof a; getsockname(l, (void*)&a, &al); listen(l, 1024);
  pthread_t th[64], sv; pthread_create(&sv, 0, server, &l);
  for (int i = 0; i < T; i++) pthread_create(&th[i], 0, spinner, 0);
  pin(0); double t0 = now(), worst = 0, sec0 = t0; long n = 0, slow = 0, nsec = 0, minrate = -1;
  while (now() - t0 < secs) {
    double s = now(); int c = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(c, (void*)&a, sizeof a) == 0) { send(c, "Q\n", 2, 0); char b[8]; recv(c, b, sizeof b, 0); }
    close(c); double d = now() - s; n++; nsec++; if (d > worst) worst = d; if (d > 1) slow++;
    if (now() - sec0 >= 1) { if (minrate < 0 || nsec < minrate) minrate = nsec; nsec = 0; sec0 = now(); }
  }
  stop = 1;
  printf("T=%d mode=%s: %ld asks in %d s, slowest %.3f s, %ld over 1 s, fewest in a second %ld\n", T, argv[2], n, secs, worst, slow, minrate);
  fflush(stdout); _exit(0);
}
