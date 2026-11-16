#include <cstdio>
#include "harness.h"
#include "snc/types/account.h"
using namespace snc; using namespace snc::account; using namespace snc::test;
int main(){ account::register_type();
  Engine::Config c; c.runtime.n_shards=1; Engine e(c); e.start();
  auto mk=[&](i64 bal){ return one(e,0,[&](Completion d,void*x){ CreateArgs a{}; a.balance_cents=bal; a.created_ns=1; a.name_len=1;a.name[0]='x'; create(kTypeId,ByteView(&a,sizeof(a)),{},d,x);}).r.id; };
  ObjectID A=mk(100), B=mk(100);
  auto dep=[&](ObjectID t,u64 cid,u64 seq){ return one(e,0,[&](Completion d,void*x){ AmountArgs a{5,1}; ExecOptions o; o.client_id=cid; o.seq=seq; execute(t,kOpDeposit,ByteView(&a,sizeof(a)),o,d,x);}).r; };
  auto bal=[&](ObjectID t){ auto r=one(e,0,[&](Completion d,void*x){ execute(t,kOpGetBalance,{},{},d,x);}); BalanceResult b{}; std::memcpy(&b,r.bytes.data(),sizeof(b)); return b.balance_cents; };
  auto r1=dep(A,7,1); std::printf("client7 seq1 deposit A: %s replayed=%d id==A %d\n",status_name(r1.status),r1.replayed,r1.id==A);
  auto r2=dep(B,7,1); std::printf("reused client7 seq1 deposit B: %s replayed=%d id==A %d id==B %d\n",status_name(r2.status),r2.replayed,r2.id==A,r2.id==B);
  std::printf("balances A=%lld B=%lld (B unchanged => the OK for B was false)\n",(long long)bal(A),(long long)bal(B));
  e.stop(); }
