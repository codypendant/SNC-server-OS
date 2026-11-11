# fsync latency probe: write 4 KiB + fsync + rename + dir fsync, like the
# witness acceptor save (tools/paxos.h save()), for N seconds.
import os, sys, time, tempfile
secs = float(sys.argv[1]); d = tempfile.mkdtemp(prefix="fsync-probe-", dir=sys.argv[2])
lat = []; end = time.time() + secs
while time.time() < end:
    t = time.perf_counter()
    p = os.path.join(d, "s.tmp")
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644); os.write(fd, b"x" * 4096); os.fsync(fd); os.close(fd)
    os.rename(p, os.path.join(d, "s")); dfd = os.open(d, os.O_RDONLY); os.fsync(dfd); os.close(dfd)
    lat.append((time.perf_counter() - t) * 1000)
os.unlink(os.path.join(d, "s")); os.rmdir(d)
lat.sort(); n = len(lat)
print(f"n={n} p50={lat[n//2]:.2f}ms p99={lat[int(n*.99)]:.2f}ms p999={lat[int(n*.999)]:.2f}ms max={lat[-1]:.1f}ms over100ms={sum(x>100 for x in lat)} over300ms={sum(x>300 for x in lat)}")
