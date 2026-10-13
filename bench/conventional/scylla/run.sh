#!/bin/bash
# ScyllaDB in the conventional-stack comparison (docs/CONVENTIONAL.md).
# Server on cores 0-1 (2 shards, Docker, host network, data on the host
# disk), load generator on cores 2-3. Usage: run.sh memory|durable [reps]
# Needs: docker with scylladb/scylla:6.2, Go 1.22+.
set -e
mode=$1; reps=${2:-3}
here=$(cd "$(dirname "$0")" && pwd)
work=${WORK:-/tmp/snc-conventional}/scylla
bin=$work/scyllabench
mkdir -p "$work"
(cd "$here" && go build -o "$bin" .)
sync_flag=()
[ "$mode" = durable ] && sync_flag=(--commitlog-sync batch)  # ack after the commitlog fsync
docker rm -f snc-scylla >/dev/null 2>&1 || true
docker run -d --name snc-scylla --network host --cpuset-cpus 0,1 -v "$work/data:/var/lib/scylla" \
  scylladb/scylla:6.2 --smp 2 --memory 6G --overprovisioned 0 --developer-mode 1 \
  --listen-address 127.0.0.1 --rpc-address 127.0.0.1 --seeds 127.0.0.1 "${sync_flag[@]}" >/dev/null
until docker logs snc-scylla 2>&1 | grep -q "init - serving"; do sleep 2; done
if [ ! -f "$work/loaded" ]; then taskset -c 2,3 "$bin" -load && touch "$work/loaded"; fi
# One untimed warm-up run: the first run after a restart pays for commitlog
# replay and compaction.
GOMAXPROCS=2 taskset -c 2,3 "$bin" -seconds 4 -deposit counter >/dev/null
echo "engine,mode,config,rep,ops_s,p50_ns,p99_ns"
for d in counter blind lwt; do for r in $(seq 0 $((reps - 1))); do
  out=$(GOMAXPROCS=2 taskset -c 2,3 "$bin" -seconds 8 -concurrency 256 -deposit $d)
  echo "scylla,$mode,deposit=$d c=256 shard-aware,$r,$(echo "$out" | awk '/ops\/s/{print $2}'),$(echo "$out" | awk '/latency/{print $4}'),$(echo "$out" | awk '/latency/{print $8}')"
done; done
docker rm -f snc-scylla >/dev/null
