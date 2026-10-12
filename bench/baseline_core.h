// Conventional-engine pieces shared by baseline_bench and baseline_server:
// one shared map in 4096 lock stripes, a striped email index, and a shared
// (or partitioned) WAL with its own group-commit writer thread.
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "snc/object_id.h"
#include "snc/types/account.h"
#include "snc/wal_format.h"

namespace snc::baseline {

using namespace snc::account;

inline u64 now_ns() {
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

[[noreturn]] inline void die(const char* what) {
  std::perror(what);
  std::exit(1);
}

// ---- Objects and the shared map -------------------------------------------------

struct Obj {
  ObjectID id;
  u64 version = 0;
  u64 last_lsn = 0;
  u16 last_log = 0;  // which log holds last_lsn
  AccountV1 a{};
};


constexpr std::size_t kStripes = 4096;

// Open addressing with the full hash in each entry, like SNC's LocalMap, so
// the baseline gets the same lookup cost (docs/PREFETCH.md). Guarded by its
// stripe's mutex, except prefetch reads, which are hints and may race.
class FlatMap {
 public:
  void reserve(std::size_t n) {
    std::size_t cap = 16;
    while (cap * 4 < n * 5) cap <<= 1;
    if (cap > table_.size()) rehash(cap);
  }
  Obj* find(const ObjectID& id, u64 h) const {
    if (table_.empty()) return nullptr;
    for (std::size_t i = h & mask_;; i = (i + 1) & mask_) {
      const Entry& e = table_[i];
      if (e.obj == nullptr) return nullptr;
      if (e.hash == h && e.obj->id == id) return e.obj;
    }
  }
  void emplace(const ObjectID&, Obj* o) { insert(o, snc_hash64(o->id)); }
  void insert(Obj* o, u64 h) {
    if ((size_ + 1) * 5 > table_.size() * 4) rehash(table_.empty() ? 16 : table_.size() * 2);
    for (std::size_t i = h & mask_;; i = (i + 1) & mask_) {
      Entry& e = table_[i];
      if (e.obj == nullptr) {
        e.obj = o;
        e.hash = h;
        ++size_;
        return;
      }
      if (e.hash == h && e.obj->id == o->id) {
        e.obj = o;
        return;
      }
    }
  }
  // Prefetch hints. Valid only while the table does not grow (the run phase).
  void prefetch_entry(u64 h) const {
    if (!table_.empty()) __builtin_prefetch(&table_[h & mask_]);
  }
  void prefetch_object(u64 h) const {
    if (table_.empty()) return;
    const Entry& e = table_[h & mask_];
    Obj* o = __atomic_load_n(&e.obj, __ATOMIC_RELAXED);
    if (o != nullptr && __atomic_load_n(&e.hash, __ATOMIC_RELAXED) == h) __builtin_prefetch(o);
  }

 private:
  struct Entry {
    Obj* obj = nullptr;
    u64 hash = 0;
  };
  void rehash(std::size_t cap) {
    std::vector<Entry> old;
    old.swap(table_);
    table_.assign(cap, Entry{});
    mask_ = cap - 1;
    size_ = 0;
    for (const Entry& e : old) {
      if (e.obj) insert(e.obj, e.hash);
    }
  }
  std::vector<Entry> table_;
  std::size_t mask_ = 0, size_ = 0;
};

struct alignas(64) Stripe {
  std::mutex mu;
  FlatMap map;
};

// Secondary index Account.email -> ObjectID, striped the same way. Kept for
// parity with SNC's shard-local email index (memory and create cost).
struct alignas(64) IndexStripe {
  std::mutex mu;
  std::unordered_map<std::string, ObjectID> map;
};

struct Map {
  std::unique_ptr<Stripe[]> stripes{new Stripe[kStripes]};
  std::unique_ptr<IndexStripe[]> email{new IndexStripe[kStripes]};
  std::atomic<u64> contended{0};  // lock acquisitions that had to wait

  Stripe& stripe(const ObjectID& id) { return stripe_of(snc_hash64(id)); }
  Stripe& stripe_of(u64 h) { return stripes[(h >> 20) & (kStripes - 1)]; }

  // Same two-stage batch prefetch as SNC's, without taking any lock: warm
  // the stripes and entries, then the objects.
  void prefetch(const ObjectID* ids, u32 n) {
    constexpr u32 kBatch = 64;
    u64 h[kBatch];
    for (u32 base = 0; base < n; base += kBatch) {
      u32 m = 0;
      for (u32 i = base; i < n && i < base + kBatch; ++i) {
        h[m] = snc_hash64(ids[i]);
        Stripe& s = stripe_of(h[m]);
        __builtin_prefetch(&s.mu);
        s.map.prefetch_entry(h[m]);
        ++m;
      }
      for (u32 i = 0; i < m; ++i) stripe_of(h[i]).map.prefetch_object(h[i]);
    }
  }
  std::unique_lock<std::mutex> lock(Stripe& s) {
    std::unique_lock<std::mutex> g(s.mu, std::try_to_lock);
    if (!g.owns_lock()) {
      contended.fetch_add(1, std::memory_order_relaxed);
      g.lock();
    }
    return g;
  }
};

// ---- Shared WAL ------------------------------------------------------------------

constexpr u32 kBlock = 4096;

inline u8* aligned(std::size_t bytes) {
  void* p = nullptr;
  if (posix_memalign(&p, kBlock, bytes) != 0) die("posix_memalign");
  return static_cast<u8*>(p);
}

class SharedLog {
 public:
  SharedLog(const std::string& dir, u16 index, u32 group_commit_us, bool sync)
      : group_commit_ns_(u64{group_commit_us} * 1000), sync_(sync) {
    ::mkdir(dir.c_str(), 0755);
    std::string path = dir + "/baseline-" + std::to_string(index) + ".wal";
    path_ = path;
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_DIRECT | O_CLOEXEC, 0644);
    if (fd_ < 0) die("open log");
    grow();
    active_ = aligned(kCap);
    flushing_ = aligned(kCap);
    writer_ = std::thread([this] { run(); });
  }
  ~SharedLog() {
    stop_.store(true);
    writer_.join();
    ::close(fd_);
    ::unlink(path_.c_str());
    std::free(active_);
    std::free(flushing_);
  }

  // Called with the object's stripe lock held, so the log order of any one
  // object matches its version order.
  u64 append(RecType type, const Obj& o) {
    std::lock_guard<std::mutex> g(mu_);
    u32 size = record_size(sizeof(AccountV1));
    while (len_ + size > kCap - kBlock) {  // buffer full: let the writer catch up
      mu_.unlock();
      std::this_thread::yield();
      mu_.lock();
    }
    u64 lsn = next_lsn_++;
    encode_record(active_ + len_, type, lsn, kTypeId, o.id, o.version, 1,
                  reinterpret_cast<const u8*>(&o.a), sizeof(AccountV1));
    if (len_ == 0) first_ns_ = now_ns();
    len_ += size;
    ++records_;
    return lsn;
  }

  u64 durable() const { return durable_.load(std::memory_order_acquire); }
  // Write latency: bucket i counts writes of [2^i, 2^(i+1)) us. Read after stop.
  u64 flush_us_log2[24] = {};
  u64 flushes() const { return flushes_; }
  u64 records() const { return records_; }
  u64 written() const { return written_; }

 private:
  static constexpr u32 kCap = 8u << 20;
  static constexpr u64 kGrow = 256ull << 20;

  void grow() {
    if (::fallocate(fd_, 0, static_cast<off_t>(allocated_), static_cast<off_t>(kGrow)) != 0) {
      die("fallocate log");
    }
    allocated_ += kGrow;
    if (::fsync(fd_) != 0) die("fsync log");
  }

  void run() {
    while (true) {
      u32 len = 0;
      u64 last = 0;
      {
        std::unique_lock<std::mutex> g(mu_);
        bool due = len_ > 0 && (len_ >= (64u << 10) || now_ns() - first_ns_ >= group_commit_ns_);
        if (!due) {
          bool stopping = stop_.load() && len_ == 0;
          g.unlock();
          if (stopping) return;
          std::this_thread::sleep_for(std::chrono::microseconds(10));
          continue;
        }
        std::swap(active_, flushing_);
        len = len_;
        len_ = 0;
        last = next_lsn_ - 1;
      }
      u32 padded = (len + kBlock - 1) / kBlock * kBlock;
      std::memset(flushing_ + len, 0, padded - len);
      if (off_ + padded > allocated_) grow();
      iovec v{flushing_, padded};
      u64 t0 = now_ns();
      ssize_t n = ::pwritev2(fd_, &v, 1, static_cast<off_t>(off_), sync_ ? RWF_DSYNC : 0);
      if (n != static_cast<ssize_t>(padded)) die("log write");
      u64 us = (now_ns() - t0) / 1000;
      ++flush_us_log2[std::min(23, us == 0 ? 0 : 63 - __builtin_clzll(us))];
      off_ += padded;
      written_ += padded;
      ++flushes_;
      durable_.store(last, std::memory_order_release);
    }
  }

  std::string path_;
  int fd_ = -1;
  u64 group_commit_ns_;
  bool sync_;
  std::mutex mu_;
  u8* active_ = nullptr;
  u8* flushing_ = nullptr;
  u32 len_ = 0;
  u64 next_lsn_ = 1;
  u64 first_ns_ = 0;
  u64 records_ = 0;
  std::atomic<u64> durable_{0};
  std::atomic<bool> stop_{false};
  u64 off_ = 0, allocated_ = 0, written_ = 0, flushes_ = 0;
  std::thread writer_;
};

using Logs = std::vector<std::unique_ptr<SharedLog>>;

struct Wait {
  u16 log = 0;
  u64 lsn = 0;  // 0 = nothing to wait for
  bool found = true;
  u64 version = 0;
  i64 balance = 0;
  u64 op_count = 0;
};

// Executes one op to completion in memory and says which LSN it must wait
// for. Writes append to log `log_hint % logs.size()`.
inline Wait execute_op(Map& map, Logs& logs, u16 log_hint, const ObjectID& id, bool read, u64 now) {
  u64 h = snc_hash64(id);
  Stripe& s = map.stripe_of(h);
  auto g = map.lock(s);
  Obj* o = s.map.find(id, h);
  if (o == nullptr) return Wait{0, 0, false};
  if (read) {
    // get_balance. D7: wait until the state it saw is durable.
    return Wait{o->last_log, o->last_lsn, true, o->version, o->a.balance_cents, o->a.op_count};
  }
  o->a.balance_cents += 1;  // deposit
  o->a.op_count += 1;
  o->a.updated_ns = now;
  ++o->version;
  if (!logs.empty()) {
    o->last_log = static_cast<u16>(log_hint % logs.size());
    o->last_lsn = logs[o->last_log]->append(RecType::UPDATE, *o);
  }
  return Wait{o->last_log, o->last_lsn, true, o->version, o->a.balance_cents, o->a.op_count};
}

// Moves `amount` from one account to another as one atomic step, the way a
// locked store does it (docs/TRANSFERS.md): both stripes locked in address
// order, both objects updated, both records appended to one log back to
// back. Returns the source's state; found=false if either is missing,
// version=0 if the source lacks funds.
inline Wait transfer_op(Map& map, Logs& logs, u16 log_hint, const ObjectID& from,
                        const ObjectID& to, i64 amount, u64 now) {
  u64 hf = snc_hash64(from), ht = snc_hash64(to);
  Stripe& sf = map.stripe_of(hf);
  Stripe& st = map.stripe_of(ht);
  Stripe* first = &sf < &st ? &sf : &st;
  Stripe* second = &sf < &st ? &st : &sf;
  auto g1 = map.lock(*first);
  std::unique_lock<std::mutex> g2;
  if (second != first) g2 = map.lock(*second);
  Obj* a = sf.map.find(from, hf);
  Obj* b = st.map.find(to, ht);
  if (a == nullptr || b == nullptr) return Wait{0, 0, false};
  if (a->a.balance_cents < amount) return Wait{0, 0, true};
  a->a.balance_cents -= amount;
  a->a.op_count += 1;
  a->a.updated_ns = now;
  ++a->version;
  b->a.balance_cents += amount;
  b->a.op_count += 1;
  b->a.updated_ns = now;
  ++b->version;
  if (!logs.empty()) {
    u16 l = static_cast<u16>(log_hint % logs.size());
    a->last_log = b->last_log = l;
    a->last_lsn = logs[l]->append(RecType::UPDATE, *a);
    b->last_lsn = logs[l]->append(RecType::UPDATE, *b);
    return Wait{l, b->last_lsn, true, a->version, a->a.balance_cents, a->a.op_count};
  }
  return Wait{0, 0, true, a->version, a->a.balance_cents, a->a.op_count};
}

// Inserts a new object and its email index entry; logs the create.
inline void insert_object(Map& map, Logs& logs, u16 log_hint, Obj* o) {
  Stripe& s = map.stripe(o->id);
  {
    auto g = map.lock(s);
    s.map.emplace(o->id, o);
    if (!logs.empty()) {
      o->last_log = static_cast<u16>(log_hint % logs.size());
      o->last_lsn = logs[o->last_log]->append(RecType::CREATE, *o);
    }
  }
  std::string key(o->a.email, o->a.email_len);
  IndexStripe& is = map.email[std::hash<std::string>{}(key) & (kStripes - 1)];
  std::lock_guard<std::mutex> ig(is.mu);
  is.map.emplace(std::move(key), o->id);
}

}  // namespace snc::baseline
