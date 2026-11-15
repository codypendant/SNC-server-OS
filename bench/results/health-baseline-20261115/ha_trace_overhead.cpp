// Trace overhead microbenchmark for docs/HEALTH_BASELINE.md (scratch, not built by CMake).
// g++ -O2 -std=c++20 -Itools -Iinclude bench/results/health-baseline-20261115/ha_trace_overhead.cpp -o /tmp/b -lpthread
// /tmp/b 20000; SNC_TRACE=1 /tmp/b 20000 2>/dev/null
#include "ha.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <sys/socket.h>
#include <netinet/in.h>
using namespace snc;
static double now(){return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int main(int argc,char**argv){
  int n=atoi(argv[1]);
  // scope cost
  volatile size_t sink=0; double t0=now();
  for(int i=0;i<10000000;i++){ ha::Caller a("watch"); ha::Caller b("propose"); sink+=ha::caller_path().size(); }
  double scope=(now()-t0)/10000000/2;
  int l=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(l,SOL_SOCKET,SO_REUSEADDR,&one,4);
  sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); bind(l,(sockaddr*)&a,sizeof a); listen(l,1024);
  socklen_t al=sizeof a; getsockname(l,(sockaddr*)&a,&al); unsigned port=ntohs(a.sin_port);
  std::thread srv([&]{ for(;;){int c=accept(l,0,0); if(c<0)return; char b[256]; ssize_t r=read(c,b,sizeof b); (void)r; ssize_t w=write(c,"OK\n",3);(void)w; close(c);} });
  srv.detach();
  std::string rep; for(int i=0;i<500;i++) ha::ask("127.0.0.1",port,"HEALTH",rep,1000);
  t0=now(); int ok=0;
  for(int i=0;i<n;i++){ ha::Caller c("watch"); ok+=ha::ask("127.0.0.1",port,"READ ha/0 x",rep,1000);} 
  double ask=(now()-t0)/n;
  printf("trace=%d scope_ns=%.2f ask_us=%.2f ok=%d/%d\n",(int)trace::enabled(),scope,ask/1000,ok,n);
}
