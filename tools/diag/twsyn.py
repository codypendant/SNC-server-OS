# DIAG ONLY: TIME_WAIT and new connections on loopback.
# A server answers one line and closes first (its side keeps TIME_WAIT); a client opens
# a fresh connection per request to the same port, as admin asks do. Counts connects
# slower than 0.5 s (a SYN refused by a TIME_WAIT socket waits for a 1 s retransmit).
# Usage: twsyn.py N [client_closes_first]
import socket, sys, threading, time
N = int(sys.argv[1]); client_first = len(sys.argv) > 2
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 0)); srv.listen(1024); port = srv.getsockname()[1]
def serve():
    while True:
        c, _ = srv.accept(); c.recv(16); c.sendall(b"OK\n")
        if client_first: c.recv(16)  # wait for the client's FIN
        c.close()
threading.Thread(target=serve, daemon=True).start()
slow = []; t0 = time.time()
for i in range(N):
    s = socket.socket(); a = time.time(); s.connect(("127.0.0.1", port)); d = time.time() - a
    s.sendall(b"Q\n"); s.recv(16); s.close()
    if d > 0.5: slow.append(round(d, 2))
print(f"{'client' if client_first else 'server'} closes first: {N} connects in {time.time()-t0:.1f}s; "
      f"slow: {len(slow)}; slowest {sorted(slow)[-3:]}", flush=True)
