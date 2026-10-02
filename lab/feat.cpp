// Feature extractor for linear eval fitting. Reads Rec records, writes float32 rows:
// [target_margin_points(stm), target_win(stm), features...]
#include "../submission/search.hpp"
using namespace cz;
#pragma pack(push, 1)
struct Rec { uint8_t c[N]; uint8_t stm, ply, cap0, cap1; int16_t score; int8_t finalHalf; uint8_t pad; };
#pragma pack(pop)

std::vector<float> features(const Pos& p) {
  const Tables& t = T();
  int me = p.stm, op = me ^ 1;
  std::vector<float> f;
  auto add = [&](float x) { f.push_back(x); };
  // per-side accumulators
  double hcnt[2][6] = {}, ownb[2] = {}, enb[2] = {}, pieces[2] = {}, ring[2][5] = {};
  double h5own[2] = {}, h5en[2] = {}, h4own[2] = {}, h4en[2] = {};
  for (int i = 0; i < N; i++) {
    uint8_t code = p.c[i];
    if (code == EMPTY) continue;
    int x = t.TOP[code], h = t.H[code];
    pieces[0] += t.CNT[code][0]; pieces[1] += t.CNT[code][1];
    hcnt[x][h]++;
    ownb[x] += t.CNT[code][x] - 1;
    enb[x] += t.CNT[code][x ^ 1];
    ring[x][t.ring[i]]++;
    if (h == 5) { h5own[x] += t.CNT[code][x]; h5en[x] += t.CNT[code][x ^ 1]; }
    if (h == 4) { h4own[x] += t.CNT[code][x]; h4en[x] += t.CNT[code][x ^ 1]; }
  }
  // attacks: distinct enemy stacks coverable, and best 1-ply gain, for each side
  double att[2] = {}, bestGain[2] = {}, nmoves[2] = {}, capt[2] = {};
  for (int s = 0; s < 2; s++) {
    Pos q = p;
    if (q.stm != s) { q.stm = s; }
    bool tgt[N] = {};
    int bg = -100;
    Move ms[400]; int n = q.genMoves(ms);
    nmoves[s] = n;
    for (int k = 0; k < n; k++) {
      int c = ms[k] / 6, d = ms[k] % 6;
      int h = t.H[q.c[c]];
      int dest = t.walk[c][d][h - 1];
      if (dest != c && t.TOP[q.c[dest]] == (s ^ 1)) tgt[dest] = true;
      Pos r = q; r.make(ms[k]);
      int g = r.marginHalf(s) - q.marginHalf(s);
      if (g > bg) bg = g;
      if (r.cap[s] > q.cap[s]) capt[s] = std::max(capt[s], (double)(r.cap[s] - q.cap[s]));
    }
    for (int i = 0; i < N; i++) att[s] += tgt[i];
    bestGain[s] = n ? bg / 2.0 : 0;
  }
  double ph = p.ply / 150.0;
  double mat = p.marginHalf(me) / 2.0;
  std::vector<double> base = {
    hcnt[me][1] - hcnt[op][1], hcnt[me][2] - hcnt[op][2], hcnt[me][3] - hcnt[op][3],
    hcnt[me][4] - hcnt[op][4], hcnt[me][5] - hcnt[op][5],
    ownb[me] - ownb[op], enb[me] - enb[op], pieces[me] - pieces[op],
    ring[me][0] + ring[me][1] - ring[op][0] - ring[op][1], ring[me][2] - ring[op][2], ring[me][3] - ring[op][3], ring[me][4] - ring[op][4],
    h5own[me] - h5own[op], h5en[me] - h5en[op], h4own[me] - h4own[op], h4en[me] - h4en[op],
    att[me], att[op], bestGain[me], bestGain[op], capt[me], capt[op], nmoves[me] - nmoves[op],
  };
  add(mat);
  add(1.0);  // tempo/bias
  for (double b : base) add(b);
  add(mat * ph);
  add(ph);
  for (double b : base) add(b * ph);
  return f;
}

int main(int argc, char** argv) {
  FILE* in = fopen(argv[1], "rb");
  FILE* out = fopen(argv[2], "wb");
  int every = argc > 3 ? atoi(argv[3]) : 1;
  Rec r; long k = 0, w = 0; int nf = 0;
  while (fread(&r, sizeof r, 1, in) == 1) {
    if (k++ % every) continue;
    Pos p; memcpy(p.c, r.c, N); p.stm = r.stm; p.ply = r.ply; p.cap[0] = r.cap0; p.cap[1] = r.cap1; p.computeKey();
    std::vector<float> f = features(p);
    float tm = (r.stm == 0 ? r.finalHalf : -r.finalHalf) / 2.0f;
    float tw = tm > 0 ? 1.f : 0.f;
    float sc = r.score / 100.0f;
    fwrite(&tm, 4, 1, out); fwrite(&tw, 4, 1, out); fwrite(&sc, 4, 1, out);
    fwrite(f.data(), 4, f.size(), out);
    nf = f.size(); w++;
  }
  fprintf(stderr, "%ld rows, %d features\n", w, nf);
  printf("%d\n", nf);
}
