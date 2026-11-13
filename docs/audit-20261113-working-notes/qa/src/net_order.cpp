// Scratch: over TCP, one client connection to shard 0's port pipelines
// deposit (batchable) then DELETE (not batchable) for an object shard 1 owns.
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include "snc/net.h"
#include "snc/snc.h"
#include "snc/types/account.h"
using namespace snc; using namespace snc::account;
struct R{ std::map<u64,wire::RespHeader> m; };
static void on(void* c,const wire::RespHeader& h,ByteView){ ((R*)c)->m[h.req_id]=h; }
static wire::ReqHeader hdr(wire::Kind k,u32 op,u64 id,ObjectID t,u64 ev=0){ wire::ReqHeader q{}; q.len=sizeof q; q.kind=k; q.op=op; q.req_id=id; q.target=t; q.expected_version=ev; return q; }
int main(int argc,char**argv){
  register_type();
  int port=atoi(argv[1]); int iters=atoi(argv[2]); int mode=atoi(argv[3]); // mode0: deposit;delete(zero bal)  mode1: deposit;withdraw(ev=1)
  net::Client c; if(!c.connect("127.0.0.1",port)){ printf("connect fail %s\n",c.error().c_str()); return 1; }
  int ot=0, ino=0, other=0;
  for(int i=0;i<iters;i++){
    R r; CreateArgs a{}; a.balance_cents = mode==0?0:100; a.created_ns=1;
    c.send(1, hdr(wire::Kind::CREATE,kTypeId,1,ObjectID{}), ByteView(&a,sizeof a));   // to shard 1's connection: owner
    while(r.m.empty()) c.poll(on,&r,1000);
    ObjectID id=r.m[1].id; if(c.owner_of(id)!=1){ other++; continue; }
    R q; AmountArgs d{10,1};
    c.send(0, hdr(wire::Kind::EXEC,kOpDeposit,10,id), ByteView(&d,sizeof d));          // via NON-owner shard 0, batchable
    if(mode==0) c.send(0, hdr(wire::Kind::DELETE,0,11,id), {});                         // not batchable
    else { AmountArgs w{5,1}; c.send(0, hdr(wire::Kind::EXEC,kOpWithdraw,11,id,1), ByteView(&w,sizeof w)); }
    while(q.m.size()<2) c.poll(on,&q,1000);
    auto &r0=q.m[10], &r1=q.m[11];
    bool over = mode==0 ? r0.status!=0 : r1.status==0;
    (over?ot:ino)++;
    if(i==0) printf("  sample req10(deposit): status=%u ver=%llu forwarded=%d | req11: status=%u ver=%llu\n",r0.status,(unsigned long long)r0.version,!!(r0.flags&2),r1.status,(unsigned long long)r1.version);
  }
  printf("port %d mode=%d iters=%d in-order=%d overtaken=%d skipped(non-shard1)=%d\n",port,mode,iters,ino,ot,other);
}
