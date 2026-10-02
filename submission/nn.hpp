// Neural network evaluation: per-cell stack embeddings summed into a hidden layer,
// then two small dense layers. Inputs are relative to the side to move.
#pragma once
#include "core.hpp"
#include <new>

namespace cz {

template <class T_>
struct AlignedAlloc {
  typedef T_ value_type;
  AlignedAlloc() = default;
  template <class U> AlignedAlloc(const AlignedAlloc<U>&) {}
  T_* allocate(size_t n) { return (T_*)::operator new(n * sizeof(T_), std::align_val_t(64)); }
  void deallocate(T_* p, size_t) { ::operator delete(p, std::align_val_t(64)); }
  template <class U> bool operator==(const AlignedAlloc<U>&) const { return true; }
  template <class U> bool operator!=(const AlignedAlloc<U>&) const { return false; }
};
typedef std::vector<float, AlignedAlloc<float>> fvec;

constexpr int NSCAL = 4;
constexpr int NH2 = 32;

struct Net {
  int H1 = 0, H2 = 0;
  fvec E;    // [N][64][H1]
  fvec SC;   // [H1][NSCAL]
  fvec SCT;  // [NSCAL][H1]
  fvec B1;   // [H1]
  bool avx2 = false;
  bool v2 = false;
  fvec W2;   // [H2][H1]
  fvec W2T;  // [H1][H2]
  fvec B2;   // [H2]
  fvec W3;   // [H2]
  float B3 = 0;
  float SK[NSCAL] = {};
  float SKB = 0;
  uint8_t FLIP[256];
  bool ok = false;

  bool load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    int32_t hdr[2];
    if (fread(hdr, 4, 2, f) != 2) { fclose(f); return false; }
    H1 = hdr[0]; H2 = hdr[1];
    v2 = H2 >= 1000;
    if (v2) H2 -= 1000;
    if ((H1 != 32 && H1 != 64 && H1 != 128 && H1 != 256) || H2 != NH2) { fclose(f); return false; }
    E.resize((size_t)N * (v2 ? 256 : 64) * H1); SC.resize(H1 * NSCAL); B1.resize(H1);
    W2.resize(H2 * H1); B2.resize(H2); W3.resize(H2);
    bool good = fread(E.data(), 4, E.size(), f) == E.size() && fread(SC.data(), 4, SC.size(), f) == SC.size() &&
                fread(B1.data(), 4, B1.size(), f) == B1.size() && fread(W2.data(), 4, W2.size(), f) == W2.size() &&
                fread(B2.data(), 4, B2.size(), f) == B2.size() && fread(W3.data(), 4, W3.size(), f) == W3.size() &&
                fread(&B3, 4, 1, f) == 1 && fread(SK, 4, NSCAL, f) == NSCAL && fread(&SKB, 4, 1, f) == 1;
    fclose(f);
    SCT.resize(H1 * NSCAL);
    for (int j = 0; j < H1; j++)
      for (int k = 0; k < NSCAL; k++) SCT[k * H1 + j] = SC[j * NSCAL + k];
    avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    W2T.resize(H1 * NH2);
    for (int m = 0; m < NH2; m++)
      for (int j = 0; j < H1; j++) W2T[j * NH2 + m] = W2[m * H1 + j];
    const Tables& t = T();
    for (int c = 0; c < 256; c++) {
      int h = t.H[c];
      FLIP[c] = c == 0 ? 0 : (uint8_t)((1 << h) | ((c ^ ((1 << h) - 1)) & ((1 << h) - 1)));
    }
    ok = good;
    return good;
  }

  typedef float v8 __attribute__((vector_size(32)));

  template <int NH1>
  __attribute__((always_inline)) static inline float evalImpl(const Net& n, const Pos& p) {
    int me = p.stm;
    float capd = (float)(p.cap[me] - p.cap[me ^ 1]);
    float margin = (float)(p.nstk[me] + p.cap[me] - p.nstk[me ^ 1] - p.cap[me ^ 1]) + (me ? 0.5f : -0.5f);
    float ply = p.ply / 150.0f;
    float sc[NSCAL] = {margin / 10, capd / 10, ply, ply * margin / 10};
    v8 a[NH1 / 8];
    const v8* b1 = (const v8*)n.B1.data();
    const v8* scw = (const v8*)n.SCT.data();  // [NSCAL][NH1]
    for (int k = 0; k < NH1 / 8; k++)
      a[k] = b1[k] + scw[k] * sc[0] + scw[NH1 / 8 + k] * sc[1] + scw[2 * NH1 / 8 + k] * sc[2] +
             scw[3 * NH1 / 8 + k] * sc[3];
    const v8* E0 = (const v8*)n.E.data();
    const uint8_t* fl = n.FLIP;
    if (n.v2) {
      const Tables& t = T();
      uint8_t att[N + 1] = {};
      for (int i = 0; i < N; i++) {
        uint8_t c = p.c[i];
        if (c == EMPTY) continue;
        int h = t.H[c];
        uint8_t bit = t.TOP[c] == me ? 1 : 2;
        for (int d = 0; d < 6; d++)
          if (t.nb[i][d] >= 0) att[t.walk[i][d][h - 1]] |= bit;
      }
      for (int i = 0; i < N; i++) {
        int code = (me ? fl[p.c[i]] : p.c[i]) + 64 * att[i];
        const v8* e = E0 + ((size_t)i * 256 + code) * (NH1 / 8);
        for (int k = 0; k < NH1 / 8; k++) a[k] += e[k];
      }
    } else {
      for (int i = 0; i < N; i++) {
        int code = me ? fl[p.c[i]] : p.c[i];
        const v8* e = E0 + ((size_t)i * 64 + code) * (NH1 / 8);
        for (int k = 0; k < NH1 / 8; k++) a[k] += e[k];
      }
    }
    alignas(32) float acc[NH1];
    const v8 zero = {0, 0, 0, 0, 0, 0, 0, 0};
    const v8 one = {1, 1, 1, 1, 1, 1, 1, 1};
    for (int k = 0; k < NH1 / 8; k++) {
      v8 x = a[k];
      x = x < zero ? zero : x;
      x = x > one ? one : x;
      *(v8*)&acc[k * 8] = x;
    }
    v8 h[NH2 / 8];
    const v8* b2 = (const v8*)n.B2.data();
    for (int m = 0; m < NH2 / 8; m++) h[m] = b2[m];
    const v8* w2 = (const v8*)n.W2T.data();
    for (int j = 0; j < NH1; j++) {
      float x = acc[j];
      if (x == 0.0f) continue;
      const v8* w = w2 + j * (NH2 / 8);
      for (int m = 0; m < NH2 / 8; m++) h[m] += w[m] * x;
    }
    const v8* w3 = (const v8*)n.W3.data();
    v8 o = zero;
    for (int m = 0; m < NH2 / 8; m++) {
      v8 x = h[m];
      x = x < zero ? zero : x;
      x = x > one ? one : x;
      o += x * w3[m];
    }
    float out = n.B3 + n.SKB;
    for (int k = 0; k < NSCAL; k++) out += n.SK[k] * sc[k];
    for (int k = 0; k < 8; k++) out += o[k];
    return out;
  }

  template <int NH1>
  __attribute__((target("avx2,fma"))) static float evalAvx2(const Net& n, const Pos& p) { return evalImpl<NH1>(n, p); }
  template <int NH1>
  static float evalGeneric(const Net& n, const Pos& p) { return evalImpl<NH1>(n, p); }

  template <int NH1>
  float evalH(const Pos& p) const { return avx2 ? evalAvx2<NH1>(*this, p) : evalGeneric<NH1>(*this, p); }

  // ---- incremental accumulators ----
  // acc[0] holds the embedding sum with white-relative codes, acc[1] with black-relative
  // codes; each is NH1 floats (64-byte aligned, up to 256).
  template <int NH1>
  __attribute__((always_inline)) static inline void accFullImpl(const Net& n, const Pos& p, float* acc) {
    v8 a0[NH1 / 8], a1[NH1 / 8];
    for (int k = 0; k < NH1 / 8; k++) { a0[k] = v8{}; a1[k] = v8{}; }
    const v8* E0 = (const v8*)n.E.data();
    for (int i = 0; i < N; i++) {
      const v8* e0 = E0 + ((size_t)i * 64 + p.c[i]) * (NH1 / 8);
      const v8* e1 = E0 + ((size_t)i * 64 + n.FLIP[p.c[i]]) * (NH1 / 8);
      for (int k = 0; k < NH1 / 8; k++) { a0[k] += e0[k]; a1[k] += e1[k]; }
    }
    v8* o = (v8*)acc;
    for (int k = 0; k < NH1 / 8; k++) { o[k] = a0[k]; o[NH1 / 8 + k] = a1[k]; }
  }
  template <int NH1>
  __attribute__((always_inline)) static inline void accUpdateImpl(const Net& n, const float* in, float* out,
                                                                 const Pos& child, const Delta& d) {
    v8 a0[NH1 / 8], a1[NH1 / 8];
    const v8* iv = (const v8*)in;
    for (int k = 0; k < NH1 / 8; k++) { a0[k] = iv[k]; a1[k] = iv[NH1 / 8 + k]; }
    const v8* E0 = (const v8*)n.E.data();
    for (int j = 0; j < d.n; j++) {
      int i = d.cell[j];
      uint8_t b = d.before[j], f = child.c[i];
      if (b == f) continue;
      const v8* eb0 = E0 + ((size_t)i * 64 + b) * (NH1 / 8);
      const v8* ef0 = E0 + ((size_t)i * 64 + f) * (NH1 / 8);
      const v8* eb1 = E0 + ((size_t)i * 64 + n.FLIP[b]) * (NH1 / 8);
      const v8* ef1 = E0 + ((size_t)i * 64 + n.FLIP[f]) * (NH1 / 8);
      for (int k = 0; k < NH1 / 8; k++) { a0[k] += ef0[k] - eb0[k]; a1[k] += ef1[k] - eb1[k]; }
    }
    v8* o = (v8*)out;
    for (int k = 0; k < NH1 / 8; k++) { o[k] = a0[k]; o[NH1 / 8 + k] = a1[k]; }
  }
  // Evaluates from a precomputed accumulator pair.
  template <int NH1>
  __attribute__((always_inline)) static inline float evalAccImpl(const Net& n, const Pos& p, const float* acc2) {
    int me = p.stm;
    float capd = (float)(p.cap[me] - p.cap[me ^ 1]);
    float margin = (float)(p.nstk[me] + p.cap[me] - p.nstk[me ^ 1] - p.cap[me ^ 1]) + (me ? 0.5f : -0.5f);
    float ply = p.ply / 150.0f;
    float sc[NSCAL] = {margin / 10, capd / 10, ply, ply * margin / 10};
    const v8* b1 = (const v8*)n.B1.data();
    const v8* scw = (const v8*)n.SCT.data();
    const v8* av = (const v8*)acc2 + me * (NH1 / 8);
    const v8 zero = {0, 0, 0, 0, 0, 0, 0, 0};
    const v8 one = {1, 1, 1, 1, 1, 1, 1, 1};
    alignas(32) float acc[NH1];
    for (int k = 0; k < NH1 / 8; k++) {
      v8 x = av[k] + b1[k] + scw[k] * sc[0] + scw[NH1 / 8 + k] * sc[1] + scw[2 * NH1 / 8 + k] * sc[2] +
             scw[3 * NH1 / 8 + k] * sc[3];
      x = x < zero ? zero : x;
      x = x > one ? one : x;
      *(v8*)&acc[k * 8] = x;
    }
    return n.head<NH1>(acc, sc);
  }
  template <int NH1>
  __attribute__((always_inline)) inline float head(const float* acc, const float* sc) const {
    const v8 zero = {0, 0, 0, 0, 0, 0, 0, 0};
    const v8 one = {1, 1, 1, 1, 1, 1, 1, 1};
    v8 h[NH2 / 8];
    const v8* b2 = (const v8*)B2.data();
    for (int m = 0; m < NH2 / 8; m++) h[m] = b2[m];
    const v8* w2 = (const v8*)W2T.data();
    for (int j = 0; j < NH1; j++) {
      float x = acc[j];
      if (x == 0.0f) continue;
      const v8* w = w2 + j * (NH2 / 8);
      for (int m = 0; m < NH2 / 8; m++) h[m] += w[m] * x;
    }
    const v8* w3 = (const v8*)W3.data();
    v8 o = zero;
    for (int m = 0; m < NH2 / 8; m++) {
      v8 x = h[m];
      x = x < zero ? zero : x;
      x = x > one ? one : x;
      o += x * w3[m];
    }
    float out = B3 + SKB;
    for (int k = 0; k < NSCAL; k++) out += SK[k] * sc[k];
    for (int k = 0; k < 8; k++) out += o[k];
    return out;
  }

  template <int NH1>
  __attribute__((target("avx2,fma"))) static void accFullAvx2(const Net& n, const Pos& p, float* acc) { accFullImpl<NH1>(n, p, acc); }
  template <int NH1>
  static void accFullGen(const Net& n, const Pos& p, float* acc) { accFullImpl<NH1>(n, p, acc); }
  template <int NH1>
  __attribute__((target("avx2,fma"))) static void accUpdAvx2(const Net& n, const float* in, float* out, const Pos& c, const Delta& d) { accUpdateImpl<NH1>(n, in, out, c, d); }
  template <int NH1>
  static void accUpdGen(const Net& n, const float* in, float* out, const Pos& c, const Delta& d) { accUpdateImpl<NH1>(n, in, out, c, d); }
  template <int NH1>
  __attribute__((target("avx2,fma"))) static float evalAccAvx2(const Net& n, const Pos& p, const float* a) { return evalAccImpl<NH1>(n, p, a); }
  template <int NH1>
  static float evalAccGen(const Net& n, const Pos& p, const float* a) { return evalAccImpl<NH1>(n, p, a); }

#define CZ_DISPATCH(fn, ...)                                                                   \
  switch (H1) {                                                                                \
    case 32: return avx2 ? fn##Avx2<32>(__VA_ARGS__) : fn##Gen<32>(__VA_ARGS__);              \
    case 64: return avx2 ? fn##Avx2<64>(__VA_ARGS__) : fn##Gen<64>(__VA_ARGS__);              \
    case 128: return avx2 ? fn##Avx2<128>(__VA_ARGS__) : fn##Gen<128>(__VA_ARGS__);           \
    default: return avx2 ? fn##Avx2<256>(__VA_ARGS__) : fn##Gen<256>(__VA_ARGS__);            \
  }
  void accFull(const Pos& p, float* acc) const { CZ_DISPATCH(accFull, *this, p, acc) }
  void accUpdate(const float* in, float* out, const Pos& child, const Delta& d) const {
    CZ_DISPATCH(accUpd, *this, in, out, child, d)
  }
  float evalAcc(const Pos& p, const float* acc) const { CZ_DISPATCH(evalAcc, *this, p, acc) }
#undef CZ_DISPATCH

  // Returns the logit of the side to move winning.
  float eval(const Pos& p) const {
    switch (H1) {
      case 32: return evalH<32>(p);
      case 64: return evalH<64>(p);
      case 128: return evalH<128>(p);
      default: return evalH<256>(p);
    }
  }
};

}  // namespace cz
