// Trainer for the Cascade evaluation net.
//   train <out.net> <H> <epochs> <lr> <valFraction> file1.bin file2.bin ...
// Records are 72 bytes (see engine.cpp). Input features are side-to-move relative:
// colours are swapped when black is to move, so "own" pieces are always colour 0.
// Target: final margin from the side to move's view. Prediction: margin_now + residual.
#include <omp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace std;
typedef uint8_t u8;
typedef int8_t i8;
typedef int16_t i16;

struct Rec {
  u8 code[61];
  u8 side, ply, capW, capB;
  i8 finalMargin;
  u8 whiteWon;
  i16 score;
  u8 pad[2];
};
static_assert(sizeof(Rec) == 72, "rec");

static const int N = 61, KC = 64;
static int H = 32;
static int perm[12][N];
static u8 FLP[KC];

static void initSym() {
  int R = 4;
  int idx[9][9];
  memset(idx, -1, sizeof(idx));
  int qs[N], rs[N], n = 0;
  for (int r = -R; r <= R; r++) {
    int qmin = max(-R, -R - r), qmax = min(R, R - r);
    for (int q = qmin; q <= qmax; q++) {
      idx[r + R][q + R] = n;
      qs[n] = q;
      rs[n] = r;
      n++;
    }
  }
  for (int s = 0; s < 12; s++) {
    int rot = s % 6, refl = s / 6;
    for (int c = 0; c < N; c++) {
      int q = qs[c], r = rs[c];
      if (refl) {
        r = -q - r;  // reflection: (q, r) -> (q, -q-r)
      }
      for (int k = 0; k < rot; k++) {
        int q2 = -r, r2 = q + r;  // 60 degree rotation
        q = q2;
        r = r2;
      }
      perm[s][c] = idx[r + R][q + R];
      if (perm[s][c] < 0) {
        fprintf(stderr, "bad symmetry\n");
        exit(1);
      }
    }
  }
  for (int c = 2; c < KC; c++) FLP[c] = (u8)(c ^ ((1 << (31 - __builtin_clz(c))) - 1));
}

struct Net {
  vector<float> W1;  // [N*KC][H]
  vector<float> B1, WD, V;  // WD [3][H]
  float C0 = 0;
  void init(int h, unsigned seed) {
    H = h;
    mt19937 rng(seed);
    normal_distribution<float> nd(0.f, 1.f);
    W1.assign((size_t)N * KC * H, 0);
    for (auto& w : W1) w = nd(rng) * 0.05f;
    B1.assign(H, 0);
    WD.assign(3 * H, 0);
    for (auto& w : WD) w = nd(rng) * 0.1f;
    V.assign(H, 0);
    for (auto& w : V) w = nd(rng) * 0.1f;
    C0 = 0;
  }
};

struct Sample {
  int cells[N];
  int nact;
  float xc, xp, xd;
  float mnow, y;
};

// Build a side-to-move relative sample; sym selects a board symmetry.
static float LAMBDA = 0.f;
static size_t valCount = 0;
static int SMOOTH = 1;
static inline void makeSample(const Rec& r, int sym, Sample& s) {
  int stm = r.side;
  s.nact = 0;
  int own = 0, opp = 0;
  for (int c = 0; c < N; c++) {
    int cd = r.code[c];
    if (cd <= 1) continue;
    int top = cd & 1;
    if (top == stm) own++;
    else opp++;
    int cv = stm ? FLP[cd] : cd;
    s.cells[s.nact++] = perm[sym][c] * KC + cv;
  }
  int capOwn = stm ? r.capB : r.capW, capOpp = stm ? r.capW : r.capB;
  int sd = own - opp, cd = capOwn - capOpp;
  s.xc = cd * 0.1f;
  s.xp = r.ply * (1.0f / 150.0f);
  s.xd = sd * 0.1f;
  s.mnow = (float)(sd + cd);
  int fm = r.finalMargin;  // white - black
  float yo = (float)(stm ? -fm : fm);
  float sc = r.score * 0.01f;  // white-absolute search score (points)
  if (stm) sc = -sc;
  sc = max(-30.f, min(30.f, sc));
  s.y = (1.f - LAMBDA) * yo + LAMBDA * sc;
}

struct Grad {
  vector<float> W1, B1, WD, V;
  float C0;
  double loss;
  void init() {
    W1.assign((size_t)N * KC * H, 0);
    B1.assign(H, 0);
    WD.assign(3 * H, 0);
    V.assign(H, 0);
    C0 = 0;
    loss = 0;
  }
  void zeroSparse(const vector<int>& touched) {
    for (int f : touched) memset(&W1[(size_t)f * H], 0, H * sizeof(float));
  }
};

static inline float forward(const Net& net, const Sample& s, float* hid) {
  float pre[64];
  for (int j = 0; j < H; j++) pre[j] = net.B1[j] + net.WD[j] * s.xc + net.WD[H + j] * s.xp + net.WD[2 * H + j] * s.xd;
  for (int a = 0; a < s.nact; a++) {
    const float* w = &net.W1[(size_t)s.cells[a] * H];
    for (int j = 0; j < H; j++) pre[j] += w[j];
  }
  float r = net.C0;
  for (int j = 0; j < H; j++) {
    hid[j] = pre[j] > 0.f ? pre[j] : 0.f;
    r += net.V[j] * hid[j];
  }
  return s.mnow + r;
}

static void saveNet(const Net& net, const char* path) {
  FILE* f = fopen(path, "wb");
  fwrite("CNN1", 1, 4, f);
  int h = H;
  fwrite(&h, 4, 1, f);
  fwrite(net.W1.data(), 4, net.W1.size(), f);
  fwrite(net.B1.data(), 4, H, f);
  fwrite(net.WD.data(), 4, 3 * H, f);
  fwrite(net.V.data(), 4, H, f);
  fwrite(&net.C0, 4, 1, f);
  fclose(f);
}

static bool loadNet(Net& net, const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  char m[4];
  int h;
  if (fread(m, 1, 4, f) != 4 || fread(&h, 4, 1, f) != 1) return false;
  H = h;
  net.W1.resize((size_t)N * KC * H);
  net.B1.resize(H);
  net.WD.resize(3 * H);
  net.V.resize(H);
  bool ok = fread(net.W1.data(), 4, net.W1.size(), f) == net.W1.size();
  ok = ok && fread(net.B1.data(), 4, H, f) == (size_t)H;
  ok = ok && fread(net.WD.data(), 4, 3 * H, f) == (size_t)(3 * H);
  ok = ok && fread(net.V.data(), 4, H, f) == (size_t)H;
  ok = ok && fread(&net.C0, 4, 1, f) == 1;
  fclose(f);
  return ok;
}

int main(int argc, char** argv) {
  initSym();
  const char* outPath = "out.net";
  H = 32;
  int epochs = 10;
  float lr0 = 0.003f;
  double valFrac = 0.1;
  const char* initPath = nullptr;
  vector<const char*> files;
  for (int i = 1; i < argc; i++) {
    string a = argv[i];
    if (a == "--out") outPath = argv[++i];
    else if (a == "--H") H = atoi(argv[++i]);
    else if (a == "--epochs") epochs = atoi(argv[++i]);
    else if (a == "--lr") lr0 = atof(argv[++i]);
    else if (a == "--val") valFrac = atof(argv[++i]);
    else if (a == "--lambda") LAMBDA = atof(argv[++i]);
    else if (a == "--smooth") SMOOTH = atoi(argv[++i]);
    else if (a == "--init") initPath = argv[++i];
    else files.push_back(argv[i]);
  }
  if (files.empty()) {
    fprintf(stderr, "usage: train [--out f] [--H n] [--epochs n] [--lr x] [--val frac] [--lambda x] [--init net] files...\n");
    return 1;
  }
  vector<Rec> data;
  for (const char* fn : files) {
    FILE* f = fopen(fn, "rb");
    if (!f) continue;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t cnt = len / sizeof(Rec);
    size_t old = data.size();
    data.resize(old + cnt);
    size_t got = fread(&data[old], sizeof(Rec), cnt, f);
    data.resize(old + got);
    fclose(f);
  }
  // Proven-result search scores (saturated) carry no margin information: use the realised margin.
  for (auto& r : data)
    if (abs((int)r.score) >= 8000) r.score = (i16)(r.finalMargin * 100);
  if (SMOOTH > 1) {
    // average the white-absolute search score over the next SMOOTH positions of the same game
    vector<float> sm(data.size());
    for (size_t i = 0; i < data.size(); i++) {
      float sum = 0;
      int cnt = 0;
      for (int k = 0; k < SMOOTH && i + k < data.size(); k++) {
        if (k > 0 && data[i + k].ply != data[i].ply + k) break;
        sum += data[i + k].score;
        cnt++;
      }
      sm[i] = sum / cnt;
    }
    for (size_t i = 0; i < data.size(); i++) data[i].score = (i16)lrintf(sm[i]);
  }
  // Split by game (a game starts at ply 0): every 1/valFrac-th game goes to validation.
  {
    int every = valFrac > 0 ? (int)(1.0 / valFrac + 0.5) : 0;
    vector<Rec> tr, va;
    tr.reserve(data.size());
    long gid = -1;
    for (auto& r : data) {
      if (r.ply == 0) gid++;
      if (every && gid % every == every - 1) va.push_back(r);
      else tr.push_back(r);
    }
    size_t ntr = tr.size();
    data = move(tr);
    data.insert(data.end(), va.begin(), va.end());
    printf("games %ld\n", gid + 1);
    (void)ntr;
    size_t total0 = data.size();
    (void)total0;
    // remember split
    valCount = va.size();
  }
  size_t total = data.size();
  size_t nval = valCount;
  size_t ntrain = total - nval;
  printf("records %zu train %zu val %zu H=%d lambda %.2f\n", total, ntrain, nval, H, LAMBDA);

  Net net;
  if (initPath) {
    if (!loadNet(net, initPath)) {
      fprintf(stderr, "cannot load init net\n");
      return 1;
    }
  } else net.init(H, 12345);

  // baselines on validation
  {
    double sse0 = 0;
    Sample s;
    for (size_t i = ntrain; i < total; i++) {
      makeSample(data[i], 0, s);
      sse0 += (s.y - s.mnow) * (s.y - s.mnow);
    }
    printf("baseline (margin_now) val MSE %.4f\n", sse0 / max<size_t>(1, nval));
  }

  int nthreads = omp_get_max_threads();
  vector<Grad> grads(nthreads);
  for (auto& g : grads) g.init();
  // Adam state
  vector<float> mW1(net.W1.size(), 0), vW1(net.W1.size(), 0);
  vector<float> mB1(H, 0), vB1(H, 0), mWD(3 * H, 0), vWD(3 * H, 0), mV(H, 0), vV(H, 0);
  float mC0 = 0, vC0 = 0;
  const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
  const int BATCH = 512;
  vector<size_t> order(ntrain);
  for (size_t i = 0; i < ntrain; i++) order[i] = i;
  mt19937_64 rng(777);
  long step = 0;
  long stepsPerEpoch = (long)(ntrain / BATCH);
  long totalSteps = stepsPerEpoch * epochs;

  for (int ep = 0; ep < epochs; ep++) {
    shuffle(order.begin(), order.end(), rng);
    double trainLoss = 0;
    float lr = lr0;
    for (long bi = 0; bi < stepsPerEpoch; bi++) {
      step++;
      // lr schedule: cosine decay to 5%
      float prog = (float)step / (float)totalSteps;
      lr = lr0 * (0.05f + 0.95f * 0.5f * (1.f + cosf(3.14159265f * prog)));
      // clear grads
      for (auto& g : grads) {
        fill(g.W1.begin(), g.W1.end(), 0.f);
        fill(g.B1.begin(), g.B1.end(), 0.f);
        fill(g.WD.begin(), g.WD.end(), 0.f);
        fill(g.V.begin(), g.V.end(), 0.f);
        g.C0 = 0;
        g.loss = 0;
      }
#pragma omp parallel
      {
        int t = omp_get_thread_num();
        Grad& g = grads[t];
        Sample s;
        float hid[64];
        mt19937 lrng((unsigned)(step * 131 + t));
#pragma omp for schedule(static)
        for (int k = 0; k < BATCH; k++) {
          size_t idx = order[bi * BATCH + k];
          int sym = lrng() % 12;
          makeSample(data[idx], sym, s);
          float pred = forward(net, s, hid);
          float err = pred - s.y;
          g.loss += (double)err * err;
          float dr = 2.f * err / BATCH;
          g.C0 += dr;
          float dp[64];
          for (int j = 0; j < H; j++) {
            g.V[j] += dr * hid[j];
            dp[j] = hid[j] > 0.f ? dr * net.V[j] : 0.f;
            g.B1[j] += dp[j];
            g.WD[j] += dp[j] * s.xc;
            g.WD[H + j] += dp[j] * s.xp;
            g.WD[2 * H + j] += dp[j] * s.xd;
          }
          for (int a = 0; a < s.nact; a++) {
            float* gw = &g.W1[(size_t)s.cells[a] * H];
            for (int j = 0; j < H; j++) gw[j] += dp[j];
          }
        }
      }
      // reduce + Adam
      float bc1 = 1.f - powf(b1, (float)step), bc2 = 1.f - powf(b2, (float)step);
      auto adam = [&](float& w, float& m, float& v, float g) {
        m = b1 * m + (1.f - b1) * g;
        v = b2 * v + (1.f - b2) * g * g;
        w -= lr * (m / bc1) / (sqrtf(v / bc2) + eps);
      };
      double bl = 0;
      for (auto& g : grads) bl += g.loss;
      trainLoss += bl / BATCH;
#pragma omp parallel for schedule(static)
      for (long i = 0; i < (long)net.W1.size(); i++) {
        float g = 0;
        for (int t = 0; t < nthreads; t++) g += grads[t].W1[i];
        if (g != 0.f || mW1[i] != 0.f) adam(net.W1[i], mW1[i], vW1[i], g);
      }
      for (int j = 0; j < H; j++) {
        float g = 0;
        for (int t = 0; t < nthreads; t++) g += grads[t].B1[j];
        adam(net.B1[j], mB1[j], vB1[j], g);
        g = 0;
        for (int t = 0; t < nthreads; t++) g += grads[t].V[j];
        adam(net.V[j], mV[j], vV[j], g);
      }
      for (int j = 0; j < 3 * H; j++) {
        float g = 0;
        for (int t = 0; t < nthreads; t++) g += grads[t].WD[j];
        adam(net.WD[j], mWD[j], vWD[j], g);
      }
      {
        float g = 0;
        for (int t = 0; t < nthreads; t++) g += grads[t].C0;
        adam(net.C0, mC0, vC0, g);
      }
    }
    // validation (symmetry 0)
    double sse = 0;
#pragma omp parallel for reduction(+ : sse) schedule(static)
    for (long i = 0; i < (long)nval; i++) {
      Sample s;
      float hid[64];
      makeSample(data[ntrain + i], 0, s);
      float pred = forward(net, s, hid);
      sse += (double)(pred - s.y) * (pred - s.y);
    }
    printf("epoch %d lr %.5f train MSE %.4f val MSE %.4f\n", ep + 1, lr, trainLoss / stepsPerEpoch, sse / max<size_t>(1, nval));
    if (ep == epochs - 1) {
      double sb[10] = {0}, sn[10] = {0}, cnt[10] = {0};
      for (size_t i = 0; i < nval; i++) {
        Sample s;
        float hid[64];
        makeSample(data[ntrain + i], 0, s);
        float pred = forward(net, s, hid);
        int b = min(9, (int)data[ntrain + i].ply / 15);
        sb[b] += (s.mnow - s.y) * (s.mnow - s.y);
        sn[b] += (pred - s.y) * (pred - s.y);
        cnt[b]++;
      }
      printf("val MSE by ply bucket (15 plies each): baseline / net\n");
      for (int b = 0; b < 10; b++) printf("  %3d-%3d: %7.2f / %7.2f  (n=%.0f)\n", b * 15, b * 15 + 14, sb[b] / max(1.0, cnt[b]), sn[b] / max(1.0, cnt[b]), cnt[b]);
    }
    fflush(stdout);
    saveNet(net, outPath);
  }
  return 0;
}
