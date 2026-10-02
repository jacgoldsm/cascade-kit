#include <cstdio>
#include <chrono>
int main(){ auto t0=std::chrono::steady_clock::now(); volatile unsigned long x=0; unsigned long y=1; for(long i=0;i<1000000000L;i++){ y = y*6364136223846793005UL + 1; } x=y; printf("%.3fs\n", std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count()); }
