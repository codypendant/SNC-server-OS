#!/usr/bin/env bash
# Durable throughput with no standby, an async standby and a sync standby
# (docs/REPLICATION.md). Primary: snc_bench, 2 shards on cores 0-1. Standby:
# snc_server --standby, 2 shards on cores 2-3, loopback. Prints CSV rows
# mode,workload,rep,ops_s,p50_ns,p99_ns. Usage: run_replication.sh [reps]
set -euo pipefail
cd "$(dirname "$0")/.."
reps=${1:-3}
work=${WORK:-/tmp/snc-repl-bench}
port=29100
uuid=5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a
echo "mode,workload,rep,ops_s,p50_ns,p99_ns"
for rep in $(seq 1 "$reps"); do
  for wl in "mix:--reads 50" "writes:--reads 0"; do
    name=${wl%%:*}; flags=${wl#*:}
    for mode in none async sync; do
      rm -rf "$work"; mkdir -p "$work"
      extra=()
      if [ "$mode" != none ]; then
        build/snc_server --shards 2 --cpus 2,3 --port 29000 --standby $port \
          --data-dir "$work/s" --space-uuid $uuid > "$work/s.log" 2>&1 &
        sb=$!
        sleep 0.5
        extra=(--replica-of 127.0.0.1:$port)
        [ "$mode" = async ] && extra+=(--async-replica)
      fi
      out=$(build/snc_bench --shards 2 --seconds 5 --remote 0 $flags --depth 256 \
              --data-dir "$work/p" --no-restart "${extra[@]}")
      if [ "$mode" != none ]; then kill -TERM $sb; wait $sb || true; fi
      ops=$(echo "$out" | awk '/ops\/s/ {print $2; exit}')
      p50=$(echo "$out" | awk '/latency ns/ {print $4; exit}')
      p99=$(echo "$out" | awk '/latency ns/ {print $8; exit}')
      echo "$mode,$name,$rep,$ops,$p50,$p99"
    done
  done
done
