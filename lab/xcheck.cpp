#include "../submission/core.hpp"
#include <iostream>
using namespace cz;
std::string csn(const Pos& p){ const Tables&t=T(); std::string s; int rl[9]={5,6,7,8,9,8,7,6,5}; int c=0; for(int r=0;r<9;r++){ if(r) s+='/'; for(int k=0;k<rl[r];k++,c++){ if(k) s+=','; int h=t.H[p.c[c]]; if(!h) s+='-'; for(int i=0;i<h;i++) s+= ((p.c[c]>>i)&1)?'b':'w'; } } s+= p.stm?" b ":" w "; s+=std::to_string(p.ply)+" "+std::to_string(p.cap[0])+" "+std::to_string(p.cap[1]); return s;}
int main(){ std::string line; long n=0,bad=0; while(std::getline(std::cin,line)){ auto a=line.find('|'), b=line.rfind('|'); Pos p; p.fromCSN(line.substr(0,a)); int m=Pos::parseMove(line.substr(a+1,b-a-1)); p.make(m); Pos q=p; q.computeKey(); if(csn(p)!=line.substr(b+1)||q.key!=p.key){bad++; if(bad<5) std::cout<<line<<"\n"<<csn(p)<<"\n";} n++;} std::cout<<n<<" checked, "<<bad<<" bad\n"; }
