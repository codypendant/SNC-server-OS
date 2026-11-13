// Scratch: a second Client instance reusing a client_id restarts its sequence at 1 against live sessions.
#include <cstdio>
#include <map>
#include "snc/net.h"
#include "snc/snc.h"
#include "snc/types/account.h"
using namespace snc; using namespace snc::account;
struct R{ std::map<u64,std::pair<wire::RespHeader,std::vector<u8>>> m; };
static void on(void* c,const wire::RespHeader& h,ByteView b){ ((R*)c)->m[h.req_id]={h,std::vector<u8>(b.data,b.data+b.len)}; }
static wire::ReqHeader hdr(wire::Kind k,u32 op,u64 id,ObjectID t){ wire::ReqHeader q{}; q.len=sizeof q; q.kind=k; q.op=op; q.req_id=id; q.target=t; return q; }
static wire::RespHeader call(net::Client& c,u16 sh,wire::ReqHeader h,ByteView a){ R r; c.send(sh,h,a); while(r.m.empty()) c.poll(on,&r,2000); return r.m.begin()->second.first; }
static i64 balance(net::Client& c,ObjectID id){ R r; c.send(c.owner_of(id),hdr(wire::Kind::EXEC,kOpGetBalance,500,id),{}); while(r.m.empty()) c.poll(on,&r,2000); BalanceResult b; memcpy(&b,r.m.begin()->second.second.data(),sizeof b); return b.balance_cents; }
int main(int argc,char**argv){
  register_type(); int port=atoi(argv[1]); const u64 CID=77;
  ObjectID id;
  { net::Client c; c.connect("127.0.0.1",port); c.enable_session(CID); // fresh unsequenced? creates are sequenced with session
    CreateArgs a{}; a.balance_cents=0; a.created_ns=1; auto h=call(c,0,hdr(wire::Kind::CREATE,kTypeId,1,ObjectID{}),ByteView(&a,sizeof a)); id=h.id;
    for(int i=0;i<3;i++){ AmountArgs d{100,1}; auto r=call(c,c.owner_of(id),hdr(wire::Kind::EXEC,kOpDeposit,10+i,id),ByteView(&d,sizeof d)); printf("incarnation1 deposit#%d status=%u replayed=%d\n",i+1,r.status,!!(r.flags&4)); }
    printf("balance after incarnation 1 = %lld (3 x 100 applied)\n",(long long)balance(c,id)); }
  { net::Client c; c.connect("127.0.0.1",port); c.enable_session(CID);   // "restarted client", same client_id, sequence counters restart at 1
    for(int i=0;i<4;i++){ AmountArgs d{1000,1}; auto r=call(c,c.owner_of(id),hdr(wire::Kind::EXEC,kOpDeposit,20+i,id),ByteView(&d,sizeof d)); printf("incarnation2 deposit#%d (+1000) status=%u replayed=%d version=%llu\n",i+1,r.status,!!(r.flags&4),(unsigned long long)r.version); }
    printf("balance after incarnation 2 = %lld (expected 300+4000=4300 if all applied)\n",(long long)balance(c,id)); }
}
