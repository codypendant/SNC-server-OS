// D2 probe: acknowledged durable creates, clean stop, one byte flipped mid-record
// in the newest WAL segment, restart: count objects and torn_tails.
#include <cstdio>
#include <cstring>
#include <string>
#include "harness.h"
#include "snc/types/account.h"
using namespace snc; using namespace snc::account; using namespace snc::test;
static std::string D;
Engine::Config cfg(bool init){ Engine::Config c; c.runtime.n_shards=1; c.runtime.ring_slots=64; c.data_dir=D; c.init=init; c.wal_segment_bytes=1u<<20; return c;}
int main(int argc,char**argv){
  D=argv[1]; const int N=8000; account::register_type();
  u64 durable_ok=0;
  { Engine e(cfg(true)); e.start();
    run_all(e,N,[&](u16,u64 i,Completion c,void*x){ CreateArgs a{}; a.balance_cents=(i64)i; a.created_ns=1; a.name_len=1; a.name[0]='x';
        create(kTypeId,ByteView(&a,sizeof(a)),{},c,x);},
      [&](u16,u64,const ExecResult& r){ if(r.status==Status::OK && r.durable) ++durable_ok; });
    u64 objs=e.stats(0).objects; e.stop();
    std::printf("run1: acked durable creates=%llu objects=%llu\n",(unsigned long long)durable_ok,(unsigned long long)objs); }
  auto segs=list_segments(D,0); std::string p=segment_path(D,0,segs.front());
  long off = 4096 + 300*100; // inside the record area, well before the end
  FILE*f=std::fopen(p.c_str(),"r+b"); std::fseek(f,off,SEEK_SET); int b=std::fgetc(f); std::fseek(f,off,SEEK_SET); std::fputc(b^0x01,f); std::fclose(f);
  std::printf("flipped one byte at offset %ld of %s (segments=%zu)\n",off,p.c_str(),segs.size());
  { Engine e(cfg(false)); e.start();
    std::printf("run2: start OK; objects=%llu torn_tails=%llu wal_records_replayed=%llu\n",
      (unsigned long long)e.stats(0).objects,(unsigned long long)e.recovery().torn_tails,(unsigned long long)e.recovery().wal_records_replayed);
    e.stop(); }
  { Engine e(cfg(false)); e.start();
    std::printf("run3 (restart again): objects=%llu torn_tails=%llu\n",(unsigned long long)e.stats(0).objects,(unsigned long long)e.recovery().torn_tails); e.stop(); }
}
