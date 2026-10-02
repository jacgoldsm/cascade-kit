#include <cstdio>
#include <chrono>
#include <vector>
#include <cstdint>
#include <cstdlib>
typedef float v8 __attribute__((vector_size(32)));
int main(){ std::vector<v8> E(61*64*8); for(auto&x:E) for(int k=0;k<8;k++) x[k]=0.001f; std::vector<uint8_t> codes(61*1000); for(auto&c:codes) c=rand()%64;
 auto t0=std::chrono::steady_clock::now(); float s=0;
 for(int it=0;it<1000000;it++){ v8 a0={},a1={},a2={},a3={},a4={},a5={},a6={},a7={}; const uint8_t* c=&codes[(it%1000)*61];
   for(int i=0;i<61;i++){ const v8* e=&E[((size_t)i*64+c[i])*8]; a0+=e[0];a1+=e[1];a2+=e[2];a3+=e[3];a4+=e[4];a5+=e[5];a6+=e[6];a7+=e[7]; }
   v8 t=a0+a1+a2+a3+a4+a5+a6+a7; for(int j=0;j<8;j++) s+=t[j]; }
 printf("%.3fs %f\n", std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count(), s); }
