#include "../submission/core.hpp"
using namespace cz;
long perft(const Pos& p, int d){ if(d==0) return 1; if(p.terminal()) return 0; Move m[400]; int n=p.genMoves(m); if(d==1) return n; long t=0; for(int i=0;i<n;i++){Pos q=p;q.make(m[i]); Pos r=q; r.computeKey(); if(r.key!=q.key||r.nstk[0]!=q.nstk[0]||r.nstk[1]!=q.nstk[1]){printf("KEY MISMATCH\n");exit(1);} t+=perft(q,d-1);} return t;}
int main(){ Pos p; p.initial(1); 
 const char* csn="w,w,b,b,w/w,w,b,b,w,b/b,w,w,b,w,b,w/w,w,b,b,b,b,w,b/b,w,w,w,-,b,b,b,w/w,b,w,w,w,w,b,b/b,w,b,w,b,b,w/w,b,w,w,b,b/b,w,w,b,b w 0 0 0";
 Pos q; q.fromCSN(csn); printf("same=%d\n", memcmp(p.c,q.c,N)==0);
 for(int d=1;d<=4;d++){ auto t0=std::chrono::steady_clock::now(); long x=perft(p,d); double s=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count(); printf("perft %d = %ld (%.2fs)\n",d,x,s);} }
