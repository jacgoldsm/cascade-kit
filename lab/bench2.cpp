#include "../submission/nn.hpp"
#include <xmmintrin.h>
using namespace cz;
int main(int argc,char**argv){ Net net; net.load(argv[1]); std::vector<Pos> ps(1000); for(int i=0;i<1000;i++){ ps[i].initial(i); }
 if(argc>2) _mm_setcsr(_mm_getcsr() | 0x8040);
 auto t0=std::chrono::steady_clock::now(); float s=0; for(int i=0;i<1000000;i++){ s+=net.eval(ps[i%1000]);} printf("1M evals %.3fs %f\n", std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count(), s); }
