#include "../submission/search.hpp"
using namespace cz;
int main(){ Pos p; long bad=0,n=0; for(uint32_t sd=1;sd<200;sd++){ p.initial(sd); uint64_t r=sd; while(!p.terminal()){ Move ms[400]; int k=p.genMoves(ms); for(int i=0;i<k;i++){ Pos q=p; q.make(ms[i]); int g1=q.marginHalf(p.stm)-p.marginHalf(p.stm); int c; int g2=Searcher::gain(p,ms[i],&c); if(g1!=g2||c!=q.cap[p.stm]-p.cap[p.stm]) bad++; n++;} r^=r<<13;r^=r>>7;r^=r<<17; p.make(ms[r%k]); } } printf("%ld/%ld bad\n",bad,n); }
