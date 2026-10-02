#include "../submission/nn.hpp"
#include <cmath>
using namespace cz;
int main(int argc,char**argv){ Net n; n.load(argv[1]); alignas(64) float a[2][512], b[512];
 double mx=0; long cnt=0;
 for(uint32_t sd=1;sd<300;sd++){ Pos p; p.initial(sd); n.accFull(p,a[0]); uint64_t r=sd*7; int cur=0;
  while(!p.terminal()){ Move ms[400]; int k=p.genMoves(ms); r^=r<<13;r^=r>>7;r^=r<<17; Delta d; Pos q=p; q.make(ms[r%k],&d); n.accUpdate(a[cur], a[cur^1], q, d); cur^=1; p=q;
   double e1=n.eval(p), e2=n.evalAcc(p,a[cur]); mx=std::max(mx,fabs(e1-e2)); cnt++;
   if(cnt%37==0){ n.accFull(p,b); for(int j=0;j<2*n.H1;j++) mx=std::max(mx,(double)fabs(b[j]-a[cur][j])); } } }
 printf("checked %ld, max diff %g\n", cnt, mx); }
