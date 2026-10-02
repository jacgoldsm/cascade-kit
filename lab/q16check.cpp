#include "../submission/nn.hpp"
using namespace cz;
#pragma pack(push, 1)
struct Rec { uint8_t c[N]; uint8_t stm, ply, cap0, cap1; int16_t score; int8_t finalHalf; uint8_t pad; };
#pragma pack(pop)
int main(int argc,char**argv){ Net a,b; a.load(argv[1]); b.load(argv[1]); b.q16=false; FILE*f=fopen(argv[2],"rb"); Rec r; std::vector<Pos> ps;
 while(fread(&r,sizeof r,1,f) && ps.size()<200000){ Pos p; memcpy(p.c,r.c,N); p.stm=r.stm;p.ply=r.ply;p.cap[0]=r.cap0;p.cap[1]=r.cap1;p.computeKey(); ps.push_back(p);} 
 double mx=0, sum=0; for(auto&p:ps){ double d=fabs(a.eval(p)-b.eval(p)); mx=std::max(mx,d); sum+=d;} printf("max diff %f mean %f (logit)\n", mx, sum/ps.size());
 for(Net* n : {&a,&b}){ auto t0=std::chrono::steady_clock::now(); float s=0; for(int it=0;it<5;it++) for(auto&p:ps) s+=n->eval(p); printf("%s: %.0f ns/eval %f\n", n->q16?"q16":"f32", std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-t0).count()/(5*ps.size()), s);} }
