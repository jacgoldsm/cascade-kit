#include <cstdio>
#include <chrono>
#include <vector>
#include <cstdint>
#include <algorithm>
int main(){ std::vector<float> E(61*64*64, 0.001f); std::vector<uint8_t> codes(61*1000); for(auto&c:codes) c=2;
 auto t0=std::chrono::steady_clock::now(); float s=0;
 for(int it=0;it<1000000;it++){ alignas(32) float acc[64]={0}; const uint8_t* c=&codes[(it%1000)*61];
   for(int i=0;i<61;i++){ const float* __restrict e=&E[((size_t)i*64+c[i])*64]; for(int j=0;j<64;j++) acc[j]+=e[j]; }
   for(int j=0;j<64;j++) s+=acc[j]; }
 printf("%.3fs %f\n", std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count(), s); }
