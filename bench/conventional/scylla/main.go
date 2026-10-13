// Load generator for ScyllaDB in the conventional-stack comparison
// (docs/CONVENTIONAL.md): the same accounts workload as SNC, Redis and
// PostgreSQL. 50% reads (balance and op count of one account), 50% deposits.
//
// ScyllaDB has no single statement that adds to a regular column and
// returns the result, so --deposit picks how a deposit is written:
//   counter  UPDATE of a counter table (balance + 1, op_count + 1): atomic,
//            returns nothing; the closest match.
//   lwt      read, then UPDATE ... IF op_count = <read>: atomic
//            read-modify-write (Paxos), returns the new values, retries on
//            conflict; the exact semantics.
//   blind    UPDATE of fixed values: not atomic, an upper bound only.
//
// Uses the shard-aware ScyllaDB driver, so each request goes to the core
// that owns its partition, as SNC's owner-routed client does.
package main

import (
	"flag"
	"fmt"
	"math/rand"
	"os"
	"sort"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gocql/gocql"
)

func main() {
	host := flag.String("host", "127.0.0.1", "ScyllaDB address")
	load := flag.Bool("load", false, "create the schema and 1M accounts, then exit")
	objects := flag.Int("objects", 1000000, "accounts")
	seconds := flag.Int("seconds", 8, "run time")
	conc := flag.Int("concurrency", 512, "requests in flight")
	deposit := flag.String("deposit", "counter", "counter, lwt or blind")
	reads := flag.Int("reads", 50, "percent reads")
	flag.Parse()

	cluster := gocql.NewCluster(*host)
	cluster.Consistency = gocql.One
	cluster.Timeout = 10 * time.Second
	cluster.NumConns = 4
	cluster.PoolConfig.HostSelectionPolicy = gocql.TokenAwareHostPolicy(gocql.RoundRobinHostPolicy())
	s, err := cluster.CreateSession()
	if err != nil {
		fmt.Fprintln(os.Stderr, "connect:", err)
		os.Exit(1)
	}
	defer s.Close()

	if *load {
		loadData(s, *objects)
		return
	}

	keyspace := "snc"
	readQ := fmt.Sprintf("SELECT balance, op_count FROM %s.accounts WHERE id = ?", keyspace)
	if *deposit == "counter" {
		readQ = fmt.Sprintf("SELECT balance, op_count FROM %s.account_counters WHERE id = ?", keyspace)
	}

	var ops, errs atomic.Int64
	lats := make([][]int64, *conc)
	stop := time.Now().Add(time.Duration(*seconds) * time.Second)
	var wg sync.WaitGroup
	for w := 0; w < *conc; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			rng := rand.New(rand.NewSource(int64(w) + 1))
			var bal, cnt int64
			for time.Now().Before(stop) {
				id := int64(rng.Intn(*objects) + 1)
				t0 := time.Now()
				var err error
				if rng.Intn(100) < *reads {
					err = s.Query(readQ, id).Scan(&bal, &cnt)
				} else {
					err = depositOp(s, *deposit, id, &bal, &cnt)
				}
				lats[w] = append(lats[w], time.Since(t0).Nanoseconds())
				if err != nil {
					errs.Add(1)
				} else {
					ops.Add(1)
				}
			}
		}(w)
	}
	wg.Wait()
	var all []int64
	for _, l := range lats {
		all = append(all, l...)
	}
	sort.Slice(all, func(i, j int) bool { return all[i] < all[j] })
	pct := func(p float64) int64 {
		if len(all) == 0 {
			return 0
		}
		return all[int(float64(len(all)-1)*p)]
	}
	fmt.Printf("  ops/s %d  errors %d\n", ops.Load()/int64(*seconds), errs.Load())
	fmt.Printf("  latency ns  p50 %d  p95 %d  p99 %d  p99.9 %d\n", pct(0.5), pct(0.95), pct(0.99), pct(0.999))
}

func depositOp(s *gocql.Session, mode string, id int64, bal, cnt *int64) error {
	now := time.Now().UnixNano()
	switch mode {
	case "counter":
		return s.Query("UPDATE snc.account_counters SET balance = balance + 1, op_count = op_count + 1 WHERE id = ?", id).Exec()
	case "blind":
		return s.Query("UPDATE snc.accounts SET balance = ?, op_count = ?, updated_ns = ? WHERE id = ?", int64(1001), int64(1), now, id).Exec()
	case "lwt":
		for {
			if err := s.Query("SELECT balance, op_count FROM snc.accounts WHERE id = ?", id).Consistency(gocql.Serial).Scan(bal, cnt); err != nil {
				return err
			}
			m := map[string]interface{}{}
			applied, err := s.Query("UPDATE snc.accounts SET balance = ?, op_count = ?, updated_ns = ? WHERE id = ? IF op_count = ?",
				*bal+1, *cnt+1, now, id, *cnt).SerialConsistency(gocql.LocalSerial).MapScanCAS(m)
			if err != nil {
				return err
			}
			if applied {
				*bal, *cnt = *bal+1, *cnt+1
				return nil
			}
		}
	}
	return fmt.Errorf("unknown deposit mode %q", mode)
}

func loadData(s *gocql.Session, n int) {
	for _, q := range []string{
		"CREATE KEYSPACE IF NOT EXISTS snc WITH replication = {'class': 'SimpleStrategy', 'replication_factor': 1}",
		"CREATE TABLE IF NOT EXISTS snc.accounts (id bigint PRIMARY KEY, balance bigint, op_count bigint, created_ns bigint, updated_ns bigint, email text, name text)",
		"CREATE TABLE IF NOT EXISTS snc.account_counters (id bigint PRIMARY KEY, balance counter, op_count counter)",
		"CREATE INDEX IF NOT EXISTS accounts_email ON snc.accounts (email)",
	} {
		if err := s.Query(q).Exec(); err != nil {
			fmt.Fprintln(os.Stderr, q, err)
			os.Exit(1)
		}
	}
	var wg sync.WaitGroup
	const workers = 256
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			for i := w + 1; i <= n; i += workers {
				id := int64(i)
				for {
					err := s.Query("INSERT INTO snc.accounts (id, balance, op_count, created_ns, updated_ns, email, name) VALUES (?, 1000, 0, 0, 0, ?, ?)",
						id, fmt.Sprintf("user%d@example.com", i), fmt.Sprintf("User %d", i)).Exec()
					if err == nil {
						err = s.Query("UPDATE snc.account_counters SET balance = balance + 1000, op_count = op_count + 0 WHERE id = ?", id).Exec()
					}
					if err == nil {
						break
					}
				}
			}
		}(w)
	}
	wg.Wait()
}
