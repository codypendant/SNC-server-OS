// Scratch: does a non-batchable request overtake an earlier batchable one (shard0 -> shard1)?
#include <atomic>
#include <cstdio>
#include <latch>
#include <vector>
#include <mutex>
#include <string>
#include <unistd.h>
#include "snc/snc.h"
#include "snc/types/account.h"
using namespace snc; using namespace snc::account;

struct Rec { std::mutex mu; std::vector<int> order; Status st[2]; u64 ver[2]; };
struct C { Rec* r; int idx; std::latch* l; };
static void done(void* c, const ExecResult& r){ auto* x=(C*)c; { std::lock_guard<std::mutex> g(x->r->mu); x->r->order.push_back(x->idx); x->r->st[x->idx]=r.status; x->r->ver[x->idx]=r.version; } x->l->count_down(); }

static ObjectID make(Engine& eng, u16 shard, i64 bal=100){
  ObjectID out{}; std::latch l(1);
  struct K{ObjectID* o; std::latch* l;} k{&out,&l};
  auto dp = eng.data_plane();
  dp.submit(shard,[&]{ CreateArgs a{}; a.balance_cents=bal; a.created_ns=1; a.email_len=0; a.name_len=0;
    create(kTypeId, ByteView(&a,sizeof a), {}, [](void*c,const ExecResult& r){ auto*k=(K*)c; *k->o=r.id; k->l->count_down(); }, &k); });
  l.wait(); return out;
}
int run(bool batch, int mode, int iters){
  Engine::Config cfg; cfg.runtime.n_shards=2; cfg.runtime.ring_slots=64; cfg.expected_objects=10000; cfg.batch_remote=batch; cfg.mint_local=true;
  Engine eng(cfg); eng.start();
  int overtaken=0, inorder=0; std::string seq;
  for(int i=0;i<iters;i++){
    ObjectID id = make(eng,1, mode==0?0:100); if(getenv("SLEEPUS")) usleep(atoi(getenv("SLEEPUS")));
    Rec rec; std::latch l(2); C c0{&rec,0,&l}, c1{&rec,1,&l};
    auto dp = eng.data_plane();
    dp.submit(0,[&]{
      AmountArgs a{10,1};
      execute(id, kOpDeposit, ByteView(&a,sizeof a), {}, done, &c0);          // op0: batchable
      if(mode==0){ remove(id, {}, done, &c1); }                                 // op1: DELETE_REQ (not batchable)
      else { ExecOptions o; o.expected_version=1; AmountArgs w{5,1}; execute(id,kOpWithdraw,ByteView(&w,sizeof w),o,done,&c1);} // conditional on v1
    });
    l.wait();
    // completion order doesn't matter; look at outcomes
    bool ot;
    if(mode==0) ot = rec.st[0]!=Status::OK;           // deposit failed because delete ran first
    else ot = rec.st[1]==Status::OK;                  // conditional on v1 succeeded => it ran before the deposit
    (ot?overtaken:inorder)++; seq.push_back(ot?'O':'.');
    if(i==0) std::printf("  sample: op0 status=%d ver=%llu | op1 status=%d ver=%llu\n",(int)rec.st[0],(unsigned long long)rec.ver[0],(int)rec.st[1],(unsigned long long)rec.ver[1]);
  }
  eng.stop(); std::printf("  pattern(first 120): %s\n", seq.substr(0,120).c_str());
  std::printf("batch_remote=%d mode=%s iters=%d: in-order=%d overtaken=%d\n",batch,mode==0?"deposit;delete":"deposit;withdraw(expected_version=1)",iters,inorder,overtaken);
  return 0;
}
int main(){ register_type(); run(true,0,200); run(true,1,200); run(false,0,200); run(false,1,200); }
