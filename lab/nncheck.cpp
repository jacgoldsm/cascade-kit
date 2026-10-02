#include "../submission/nn.hpp"
using namespace cz;
#pragma pack(push, 1)
struct Rec { uint8_t c[N]; uint8_t stm, ply, cap0, cap1; int16_t score; int8_t finalHalf; uint8_t pad; };
#pragma pack(pop)
int main(int argc,char**argv){ Net net; printf("load %d\n", net.load(argv[1])); FILE*f=fopen(argv[2],"rb"); Rec r; for(int k=0;k<5 && fread(&r,sizeof r,1,f);k++){ Pos p; memcpy(p.c,r.c,N); p.stm=r.stm;p.ply=r.ply;p.cap[0]=r.cap0;p.cap[1]=r.cap1;p.computeKey(); printf("%f\n", net.eval(p)); }
 auto t0=std::chrono::steady_clock::now(); Pos p; p.initial(3); float s=0; for(int i=0;i<1000000;i++){ p.c[i%61]^=0; s+=net.eval(p);} printf("1M evals %.3fs %f\n", std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count(), s); }
