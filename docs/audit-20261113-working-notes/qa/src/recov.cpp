// Scratch: write N acknowledged accounts (1 shard), or open an existing dir and report what recovery sees.
#include <cstdio>
#include <cstring>
#include <latch>
#include <string>
#include <vector>
#include "snc/snc.h"
#include "snc/storage.h"
#include "snc/types/account.h"
using namespace snc; using namespace snc::account;
static Engine::Config cfg(const std::string& d,bool init){ Engine::Config c; c.runtime.n_shards=1; c.runtime.ring_slots=64; c.expected_objects=10000; c.data_dir=d; c.init=init; c.wal_segment_bytes=1u<<20; return c; }
int main(int argc,char**argv){
  register_type(); std::string mode=argv[1], d=argv[2];
  if(mode=="write"){
    int n=atoi(argv[3]); Engine eng(cfg(d,true)); eng.start(); { auto dp=eng.data_plane();
    for(int i=1;i<=n;i++){
      std::latch l(1); int st=-1; bool dur=false; struct K{std::latch*l;int*st;bool*d;} k{&l,&st,&dur};
      dp.submit(0,[&]{ CreateArgs a{}; a.balance_cents=i; a.created_ns=1; a.name_len=1; a.name[0]='x';
        create(kTypeId,ByteView(&a,sizeof a),{},[](void*c,const ExecResult&r){auto*k=(K*)c;*k->st=(int)r.status;*k->d=r.durable;k->l->count_down();},&k);});
      l.wait(); if(st!=0||!dur){ printf("create %d status=%d durable=%d\n",i,st,dur); return 1; }
    }
    } if(argc>4) { auto r=eng.checkpoint(); printf("checkpoint seq=%llu objects=%llu\n",(unsigned long long)r.seq,(unsigned long long)r.objects);} eng.stop(); printf("wrote %d acknowledged durable creates, clean stop\n",n); return 0;
  }
  // check
  Engine eng(cfg(d,false)); eng.start(); const auto& r=eng.recovery();
  printf("recovery: ckpt_seq=%llu images=%llu wal_records_replayed=%llu torn_tails=%llu objects=%llu\n",(unsigned long long)r.checkpoint_seq,(unsigned long long)r.images_loaded,(unsigned long long)r.wal_records_replayed,(unsigned long long)r.torn_tails,(unsigned long long)r.objects);
  std::vector<i64> bal; eng.for_each_object(0,[&](const ObjectHeader& h){ AccountV1 a; std::memcpy(&a,h.payload(),sizeof a); bal.push_back(a.balance_cents);});
  std::sort(bal.begin(),bal.end()); i64 mx=bal.empty()?0:bal.back(); int missing=0; std::string miss;
  std::vector<bool> have(mx+1); for(i64 b:bal) have[b]=true;
  int total=argc>3?atoi(argv[3]):(int)mx; for(int i=1;i<=total;i++) if(i>mx||!have[i]){ if(missing<12) miss+=std::to_string(i)+" "; missing++; }
  printf("accounts present=%zu, max balance-tag=%lld, acknowledged-but-missing=%d (first: %s)\n",bal.size(),(long long)mx,missing,miss.c_str());
  eng.stop(); printf("clean exit\n"); return 0;
}
