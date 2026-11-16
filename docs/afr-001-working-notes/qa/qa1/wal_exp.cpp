// QA1 WAL corruption experiments (P1, P2, P3, P4). Independent of other reviewers.
// Usage: wal_exp <mode> <dir>
//   mid    : 100 acked durable creates, clean stop, flip one payload byte in record #50
//            (LSN order) of the newest segment, restart, count survivors, then write more.
//   first  : 100 creates, restart, 50 more creates (new newest segment), flip byte in the
//            first record of the newest segment, restart twice.
//   type   : like mid, but rewrite record #50's rec_type to 9 and recompute a valid CRC.
//   older  : like first, but corrupt record #50 of the OLDER segment.
//   zero   : like mid, but zero the whole 4 KiB block that holds record #50.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "harness.h"
#include "snc/storage.h"
#include "snc/types/account.h"
#include "snc/wal_format.h"

using namespace snc;
using namespace snc::account;
using namespace snc::test;

static std::string g_dir;

static Engine::Config cfg(bool init) {
  Engine::Config c;
  c.runtime.n_shards = 1;
  c.runtime.ring_slots = 64;
  c.expected_objects = 10000;
  c.data_dir = g_dir;
  c.init = init;
  c.wal_segment_bytes = 1u << 20;
  return c;
}

static std::vector<ObjectID> create_n(Engine& eng, int n, int base) {
  std::vector<ObjectID> ids;
  for (int i = 0; i < n; ++i) {
    CreateArgs a{};
    a.balance_cents = 1000 + base + i;
    a.created_ns = 1;
    std::string e = "u" + std::to_string(base + i) + "@x";
    a.email_len = (u8)e.size();
    std::memcpy(a.email, e.data(), e.size());
    a.name_len = 1;
    a.name[0] = 'x';
    OneResult r = one(eng, 0, [&](Completion c, void* ctx) {
      create(kTypeId, ByteView{reinterpret_cast<const u8*>(&a), sizeof(a)}, ExecOptions{}, c, ctx);
    });
    if (r.r.status != Status::OK || !r.r.durable) {
      std::printf("create failed status=%u durable=%d\n", (unsigned)r.r.status, r.r.durable);
      std::exit(3);
    }
    ids.push_back(r.r.id);
  }
  return ids;
}

static int count_present(Engine& eng, const std::vector<ObjectID>& ids, int* first_missing) {
  int ok = 0;
  *first_missing = -1;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    OneResult r = one(eng, 0, [&](Completion c, void* ctx) {
      execute(ids[i], kOpGetBalance, ByteView{}, ExecOptions{}, c, ctx);
    });
    if (r.r.status == Status::OK) ++ok;
    else if (*first_missing < 0) *first_missing = (int)i;
  }
  return ok;
}

struct Rec { std::size_t off; u64 lsn; u32 len; };

static std::vector<Rec> parse(const std::string& path, std::vector<u8>& buf) {
  FILE* f = std::fopen(path.c_str(), "rb");
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  buf.resize(n);
  if (std::fread(buf.data(), 1, n, f) != (size_t)n) std::exit(4);
  std::fclose(f);
  std::vector<Rec> out;
  std::size_t off = kBlock;
  while (off + sizeof(WalRecord) <= buf.size()) {
    u32 len = check_record(buf.data() + off, buf.size() - off);
    if (len == 0) {
      std::size_t be = (off / kBlock + 1) * kBlock;
      bool z = true;
      for (std::size_t i = off; i < std::min(be, buf.size()); ++i) z &= buf[i] == 0;
      if (!z || off % kBlock == 0) break;
      off = be;
      continue;
    }
    WalRecord r;
    std::memcpy(&r, buf.data() + off, sizeof(r));
    out.push_back({off, r.lsn, len});
    off += len;
  }
  return out;
}

static void write_back(const std::string& path, const std::vector<u8>& buf) {
  FILE* f = std::fopen(path.c_str(), "r+b");
  std::fwrite(buf.data(), 1, buf.size(), f);
  std::fclose(f);
}

static void list(const char* tag) {
  std::printf("[%s] segments:", tag);
  for (u64 s : list_segments(g_dir, 0)) {
    std::vector<u8> b;
    auto recs = parse(segment_path(g_dir, 0, s), b);
    std::printf(" %llu(recs=%zu,lsn %llu..%llu)", (unsigned long long)s, recs.size(),
                recs.empty() ? 0ull : (unsigned long long)recs.front().lsn,
                recs.empty() ? 0ull : (unsigned long long)recs.back().lsn);
  }
  std::printf("\n");
}

// Runs a restart in a child so a storage_fatal abort is observed as an exit status.
static int restart_child(const std::vector<ObjectID>& ids, int extra_writes, const char* tag) {
  std::fflush(stdout);
  pid_t p = fork();
  if (p == 0) {
    Engine eng(cfg(false));
    eng.start();
    const auto& rr = eng.recovery();
    int fm;
    int ok = count_present(eng, ids, &fm);
    std::printf("[%s] start OK: wal_records_replayed=%llu torn_tails=%llu objects=%llu; acked=%zu present=%d first_missing_idx=%d\n",
                tag, (unsigned long long)rr.wal_records_replayed, (unsigned long long)rr.torn_tails,
                (unsigned long long)rr.objects, ids.size(), ok, fm);
    if (extra_writes) create_n(eng, extra_writes, 100000);
    if (extra_writes && getenv("CKPT")) { auto cr = eng.checkpoint(); std::printf("[%s] checkpoint seq=%llu segments_deleted=%llu\n", tag, (unsigned long long)cr.seq, (unsigned long long)cr.segments_deleted); }
    eng.stop();
    std::fflush(stdout);
    _exit(0);
  }
  int st;
  waitpid(p, &st, 0);
  std::printf("[%s] child exit: %s %d\n", tag, WIFEXITED(st) ? "status" : "signal",
              WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
  return st;
}

int main(int argc, char** argv) {
  register_type();
  std::string mode = argv[1];
  g_dir = argv[2];
  std::vector<ObjectID> ids;
  {
    Engine eng(cfg(true));
    eng.start();
    ids = create_n(eng, 100, 0);
    eng.stop();
  }
  if (mode == "first" || mode == "older") {
    Engine eng(cfg(false));
    eng.start();
    auto more = create_n(eng, 50, 1000);
    ids.insert(ids.end(), more.begin(), more.end());
    eng.stop();
  }
  list("before damage");
  auto segs = list_segments(g_dir, 0);
  u64 target_seg = (mode == "older") ? segs[segs.size() - 2] : segs.back();
  // newest segment with records (after restarts an empty newest segment may exist)
  if (mode != "older") {
    for (std::size_t i = segs.size(); i-- > 0;) {
      std::vector<u8> b;
      if (!parse(segment_path(g_dir, 0, segs[i]), b).empty()) { target_seg = segs[i]; break; }
    }
  }
  std::string path = segment_path(g_dir, 0, target_seg);
  std::vector<u8> buf;
  auto recs = parse(path, buf);
  std::size_t k = (mode == "first") ? 0 : 49;
  if (k >= recs.size()) { std::printf("not enough records (%zu)\n", recs.size()); return 5; }
  Rec R = recs[k];
  std::printf("damaging segment %llu (newest with records=%s) record idx %zu lsn %llu off %zu, mode=%s\n",
              (unsigned long long)target_seg, target_seg == segs.back() ? "is last file" : "not last file",
              k, (unsigned long long)R.lsn, R.off, mode.c_str());
  if (mode == "type") {
    WalRecord h;
    std::memcpy(&h, buf.data() + R.off, sizeof(h));
    h.rec_type = 9;
    std::memcpy(buf.data() + R.off, &h, sizeof(h));
    u32 crc = crc32c(buf.data() + R.off + 4, R.len - 4);
    std::memcpy(buf.data() + R.off, &crc, 4);
    std::printf("record crc recomputed: check_record now returns %u\n", check_record(buf.data() + R.off, buf.size() - R.off));
  } else if (mode == "zero") {
    std::size_t b0 = R.off / kBlock * kBlock;
    std::memset(buf.data() + b0, 0, kBlock);
    std::printf("zeroed block at %zu\n", b0);
  } else {
    buf[R.off + sizeof(WalRecord) + 10] ^= 0x01;  // one bit in the payload
  }
  write_back(path, buf);
  if (argc > 3) return 0;
  restart_child(ids, 10, "restart 1 (+10 new creates)");
  list("after restart 1");
  restart_child(ids, 0, "restart 2");
  list("after restart 2");
  return 0;
}
