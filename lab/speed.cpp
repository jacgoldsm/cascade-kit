#include "../submission/search.hpp"
using namespace cz;
int main(int argc,char**argv){ Net net; bool useNet = argc>1 && net.load(argv[1]); Searcher s(20); if(useNet) s.net=&net; if(argc>2) s.pr.v[P_NNSCALE]=atoi(argv[2]);
 long long tn=0; double tt=0; int td=0;
 for(uint32_t sd=1; sd<=20; sd++){ Pos p; p.initial(sd*977); uint64_t r=sd; for(int k=0;k<30;k++){ Move ms[400]; int n=p.genMoves(ms); r^=r<<13;r^=r>>7;r^=r<<17; p.make(ms[r%n]); }
   s.clearTT(); s.think(p, 1e9, 1e9, 64, 300000); tn+=s.nodes; tt+=s.elapsedMs(); td+=s.depthDone; }
 printf("%s: nps %.0f k, avg depth %.2f\n", useNet?"net":"mat", tn/tt, td/20.0); }
