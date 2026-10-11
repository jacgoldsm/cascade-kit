// Cascade engine: alpha-beta (PVS) search over a compact incremental board.
//
// Stack encoding: each cell holds a "sentinel code": a leading 1 bit followed by the
// stack's pieces from bottom to top (1 = black). Empty = 1. The top piece is code & 1,
// the height is bitlength(code) - 1. Adding a piece on top: (code << 1) | colour.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace std;

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int16_t i16;
typedef int8_t i8;

static const int MAXSIDE = 13;
static const int MAXN = 3 * MAXSIDE * (MAXSIDE - 1) + 1;  // 469
static const int MAXMOVES = 6 * MAXN;
static const int MAXPLY = 160;
static const int MAXH = 12;

// ---------------------------------------------------------------- geometry

static const int DQ[6] = {1, 1, 0, -1, -1, 0};
static const int DR[6] = {0, -1, -1, 0, 1, 1};
static const char* DNAME[6] = {"E", "NE", "NW", "W", "SW", "SE"};

struct Geo {
  int side = 0, collapse = 0, maxply = 0, N = 0, maxH = 0;
  int komi100 = 50;
  double komi = 0.5;
  int q[MAXN], r[MAXN];
  int nb[MAXN * 6];
  int ndir[MAXN];
  int dirs[MAXN][6];
  int cls[MAXN];
  int antipode[MAXN];
  string name[MAXN];
  vector<i16> walk;
  int rowStart[2 * MAXSIDE], rowLen[2 * MAXSIDE], rows = 0;
  int collapseCode = 64;
  int ncls = 0;
};
static Geo G;

static void initGeo(int side, int collapse, int maxply, double komi) {
  G.side = side;
  G.collapse = collapse;
  G.maxply = maxply;
  G.komi = komi;
  G.komi100 = (int)llround(komi * 100);
  G.maxH = collapse - 1;
  G.collapseCode = 1 << collapse;
  int R = side - 1;
  vector<int> idx((2 * R + 1) * (2 * R + 1), -1);
  auto at = [&](int q, int r) -> int& { return idx[(r + R) * (2 * R + 1) + (q + R)]; };
  int n = 0;
  G.rows = 0;
  for (int r = -R; r <= R; r++) {
    int qmin = max(-R, -R - r), qmax = min(R, R - r);
    G.rowStart[G.rows] = n;
    G.rowLen[G.rows] = qmax - qmin + 1;
    G.rows++;
    for (int q = qmin; q <= qmax; q++) {
      at(q, r) = n;
      G.q[n] = q;
      G.r[n] = r;
      G.name[n] = string(1, (char)('a' + r + R)) + to_string(q - qmin + 1);
      n++;
    }
  }
  G.N = n;
  for (int c = 0; c < n; c++) {
    G.ndir[c] = 0;
    for (int d = 0; d < 6; d++) {
      int q = G.q[c] + DQ[d], r = G.r[c] + DR[d];
      int t = -1;
      if (abs(q) <= R && abs(r) <= R && abs(q + r) <= R) t = at(q, r);
      G.nb[c * 6 + d] = t;
      if (t >= 0) G.dirs[c][G.ndir[c]++] = d;
    }
    G.antipode[c] = at(-G.q[c], -G.r[c]);
  }
  G.walk.assign((size_t)n * 6 * G.maxH, -1);
  for (int c = 0; c < n; c++)
    for (int d0 = 0; d0 < 6; d0++) {
      if (G.nb[c * 6 + d0] < 0) continue;
      int pos = c, d = d0;
      for (int i = 0; i < G.maxH; i++) {
        if (G.nb[pos * 6 + d] < 0) d = (d + 3) % 6;
        pos = G.nb[pos * 6 + d];
        G.walk[((size_t)c * 6 + d0) * G.maxH + i] = (i16)pos;
      }
    }
  // Cell classes: orbits under the 12 symmetries of the hexagon.
  vector<int> keys;
  for (int c = 0; c < n; c++) {
    int x = G.q[c], z = G.r[c], y = -x - z;
    int a[3] = {abs(x), abs(y), abs(z)};
    sort(a, a + 3);
    int key = a[0] * 10000 + a[1] * 100 + a[2];
    int k = -1;
    for (size_t i = 0; i < keys.size(); i++)
      if (keys[i] == key) k = (int)i;
    if (k < 0) {
      k = (int)keys.size();
      keys.push_back(key);
    }
    G.cls[c] = k;
  }
  G.ncls = (int)keys.size();
}

// ---------------------------------------------------------------- hashing

static u64 splitmix64(u64& s) {
  u64 z = (s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}
static inline u64 mixZ(int c, u32 code) {
  u64 s = ((u64)c << 32) ^ code ^ 0x1234567ULL;
  return splitmix64(s);
}
static const u32 ZLIM = 512;
static vector<u64> ztab;
static u64 zPly[256], zCap[2][1024], zSide;
static void initZobrist() {
  ztab.assign((size_t)MAXN * ZLIM, 0);
  for (int c = 0; c < MAXN; c++)
    for (u32 k = 2; k < ZLIM; k++) ztab[(size_t)c * ZLIM + k] = mixZ(c, k);
  u64 s = 0xC0FFEE;
  for (auto& x : zPly) x = splitmix64(s);
  for (auto& a : zCap)
    for (auto& x : a) x = splitmix64(s);
  zSide = splitmix64(s);
}
static inline u64 Z(int c, u32 code) {
  if (code == 1) return 0;
  return code < ZLIM ? ztab[(size_t)c * ZLIM + code] : mixZ(c, code);
}

// ---------------------------------------------------------------- evaluation
// Two evaluators. Margin: stacks + captures difference (always available). Net: a small
// sparse-input network predicting the final white-minus-black margin as
//   margin_now + residual(position),
// with first-layer accumulators updated incrementally while moves are made.

#ifndef HC_VAL
#define HC_VAL 32
#endif
static const int HC = HC_VAL;  // hidden width (compile-time so loops vectorise)
static const int HMAX = HC;
static const int KC = 64;  // codes per cell in the net's input space (collapse == 6)
static bool netOn = false;
static int NH = 0;
// Quantised network: accumulators and first-layer weights are int16 in units of 1/QA; the
// output weights are int16 in units of 1/QV.
static const int QA = 256, QV = 1024;
static vector<int16_t> netW1q;  // [(cell * KC + code) * HC + j]
static int16_t netB1q[HMAX], netVq[HMAX];
static int16_t tabCd[128][HMAX], tabSd[128][HMAX], tabPly[256][HMAX];  // dense-input contributions
static float netC0 = 0;
static int netC0cp = 0;
static u8 FLP[KC];  // colour-flipped stack code (for the second perspective)

static inline int16_t quant(float x, int scale) {
  float v = nearbyintf(x * (float)scale);
  if (v > 32767.f) v = 32767.f;
  if (v < -32768.f) v = -32768.f;
  return (int16_t)v;
}

static bool loadNetBuf(const unsigned char* buf, size_t len) {
  if (len < 8 || memcmp(buf, "CNN1", 4) != 0) return false;
  int H;
  memcpy(&H, buf + 4, 4);
  if (H != HC) return false;
  int N = 61;
  size_t need = 8 + sizeof(float) * ((size_t)N * KC * H + H + 3 * H + H + 1);
  if (len != need) return false;
  const float* f = (const float*)(buf + 8);
  NH = H;
  netW1q.resize((size_t)N * KC * H);
  for (size_t i = 0; i < netW1q.size(); i++) netW1q[i] = quant(f[i], QA);
  f += (size_t)N * KC * H;
  for (int j = 0; j < H; j++) netB1q[j] = quant(f[j], QA);
  f += H;
  const float* wd[3];
  for (int k = 0; k < 3; k++) {
    wd[k] = f;
    f += H;
  }
  for (int j = 0; j < H; j++) netVq[j] = quant(f[j], QV);
  f += H;
  netC0 = *f;
  netC0cp = (int)lrintf(netC0 * 100.f);
  for (int i = 0; i < 128; i++)
    for (int j = 0; j < H; j++) {
      tabCd[i][j] = quant((float)(i - 64) * 0.1f * wd[0][j], QA);
      tabSd[i][j] = quant((float)(i - 64) * 0.1f * wd[2][j], QA);
    }
  for (int i = 0; i < 256; i++)
    for (int j = 0; j < H; j++) tabPly[i][j] = quant((float)i * (1.0f / 150.0f) * wd[1][j], QA);
  return true;
}

static bool loadNetFile(const char* path) {
  FILE* fp = fopen(path, "rb");
  if (!fp) return false;
  fseek(fp, 0, SEEK_END);
  long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  vector<unsigned char> buf(len);
  size_t got = fread(buf.data(), 1, len, fp);
  fclose(fp);
  return got == (size_t)len && loadNetBuf(buf.data(), len);
}

static void initEval() {
  for (int c = 2; c < KC; c++) FLP[c] = (u8)(c ^ ((1 << (31 - __builtin_clz(c))) - 1));
  FLP[0] = 0;
  FLP[1] = 1;
  // The net is only valid for the default board; other variants use the margin evaluator.
  netOn = NH > 0 && G.side == 5 && G.collapse == 6 && getenv("CASCADE_NONET") == nullptr;
}

// ---------------------------------------------------------------- position

struct Pos {
  u32 code[MAXN];
  int stk[2], cap[2];
  int side, ply;
  u64 hash;
  alignas(16) int16_t acc[2][HMAX];  // [0]: white's view, [1]: black's view (colours swapped)
};

struct Undo {
  int n;
  u16 cell[2 * MAXH + 4];
  u32 old[2 * MAXH + 4];
  int stk[2], cap[2];
  u64 hash;
  alignas(16) int16_t acc[2][HMAX];
};

static inline int hgt(u32 code) { return 31 - __builtin_clz(code); }

static inline void setCell(Pos& P, Undo& U, int c, u32 nc) {
  u32 oc = P.code[c];
  U.cell[U.n] = (u16)c;
  U.old[U.n] = oc;
  U.n++;
  P.code[c] = nc;
  P.hash ^= Z(c, oc) ^ Z(c, nc);
  if (oc > 1) P.stk[oc & 1]--;
  if (nc > 1) P.stk[nc & 1]++;
  if (netOn) {
    const int H = HC;
    const bool fo = oc > 1 && oc < KC, fn = nc > 1 && nc < KC;
    for (int v = 0; v < 2; v++) {
      int16_t* acc = P.acc[v];
      const int ov = v ? FLP[oc & (KC - 1)] : oc, nv = v ? FLP[nc & (KC - 1)] : nc;
      if (fo && fn) {
        const int16_t* wo = &netW1q[(size_t)(c * KC + ov) * H];
        const int16_t* wn = &netW1q[(size_t)(c * KC + nv) * H];
        for (int j = 0; j < H; j++) acc[j] = (int16_t)(acc[j] + wn[j] - wo[j]);
      } else if (fn) {
        const int16_t* wn = &netW1q[(size_t)(c * KC + nv) * H];
        for (int j = 0; j < H; j++) acc[j] = (int16_t)(acc[j] + wn[j]);
      } else if (fo) {
        const int16_t* wo = &netW1q[(size_t)(c * KC + ov) * H];
        for (int j = 0; j < H; j++) acc[j] = (int16_t)(acc[j] - wo[j]);
      }
    }
  }
}

static inline void makeMove(Pos& P, int m, Undo& U) {
  U.n = 0;
  U.stk[0] = P.stk[0];
  U.stk[1] = P.stk[1];
  U.cap[0] = P.cap[0];
  U.cap[1] = P.cap[1];
  U.hash = P.hash;
  if (netOn) memcpy(U.acc, P.acc, sizeof(P.acc));
  int c = m / 6;
  u32 code = P.code[c];
  int h = hgt(code);
  int mover = P.side;
  setCell(P, U, c, 1);
  const i16* w = &G.walk[(size_t)m * G.maxH];
  for (int i = 0; i < h; i++) {
    int t = w[i];
    u32 col = (code >> (h - 1 - i)) & 1;
    setCell(P, U, t, (u32)((P.code[t] << 1) | col));
  }
  for (int i = 0; i < h; i++) {
    int t = w[i];
    u32 ct = P.code[t];
    if (ct >= (u32)G.collapseCode) {
      int ht = hgt(ct);
      int blacks = __builtin_popcount(ct) - 1;
      P.cap[mover] += mover == 0 ? blacks : ht - blacks;
      setCell(P, U, t, 1);
    }
  }
  P.side ^= 1;
  P.ply++;
}

static inline void unmakeMove(Pos& P, const Undo& U) {
  for (int i = U.n - 1; i >= 0; i--) P.code[U.cell[i]] = U.old[i];
  P.stk[0] = U.stk[0];
  P.stk[1] = U.stk[1];
  P.cap[0] = U.cap[0];
  P.cap[1] = U.cap[1];
  P.hash = U.hash;
  if (netOn) memcpy(P.acc, U.acc, sizeof(P.acc));
  P.side ^= 1;
  P.ply--;
}

static inline int genMoves(const Pos& P, int* mv) {
  int n = 0, s = P.side;
  for (int c = 0; c < G.N; c++) {
    u32 cd = P.code[c];
    if (cd > 1 && (int)(cd & 1) == s) {
      int nd = G.ndir[c];
      for (int k = 0; k < nd; k++) mv[n++] = c * 6 + G.dirs[c][k];
    }
  }
  return n;
}

static inline u64 ttKey(const Pos& P) {
  return P.hash ^ zPly[P.ply & 255] ^ zCap[0][P.cap[0] & 1023] ^ zCap[1][P.cap[1] & 1023] ^ (P.side ? zSide : 0);
}

static void recompute(Pos& P) {
  P.stk[0] = P.stk[1] = 0;
  P.hash = 0;
  if (netOn) {
    for (int v = 0; v < 2; v++)
      for (int j = 0; j < HC; j++) P.acc[v][j] = netB1q[j];
  }
  for (int c = 0; c < G.N; c++) {
    u32 cd = P.code[c];
    if (cd > 1) P.stk[cd & 1]++;
    P.hash ^= Z(c, cd);
    if (netOn && cd > 1 && cd < (u32)KC) {
      for (int v = 0; v < 2; v++) {
        const int16_t* w = &netW1q[(size_t)(c * KC + (v ? FLP[cd] : cd)) * HC];
        for (int j = 0; j < HC; j++) P.acc[v][j] = (int16_t)(P.acc[v][j] + w[j]);
      }
    }
  }
}

static string cellStr(const Pos& P, int c) {
  u32 cd = P.code[c];
  if (cd == 1) return "-";
  int h = hgt(cd);
  string s;
  for (int i = 0; i < h; i++) s += ((cd >> (h - 1 - i)) & 1) ? 'b' : 'w';
  return s;
}

static string toCSN(const Pos& P) {
  string out;
  for (int r = 0; r < G.rows; r++) {
    if (r) out += '/';
    for (int k = 0; k < G.rowLen[r]; k++) {
      if (k) out += ',';
      out += cellStr(P, G.rowStart[r] + k);
    }
  }
  out += P.side ? " b " : " w ";
  out += to_string(P.ply) + " " + to_string(P.cap[0]) + " " + to_string(P.cap[1]);
  return out;
}

static bool fromCSN(const string& str, Pos& P) {
  char board[8192];
  char sd[8];
  int ply, cw, cb;
  if (sscanf(str.c_str(), "%8191s %7s %d %d %d", board, sd, &ply, &cw, &cb) != 5) return false;
  memset(P.code, 0, sizeof(P.code));
  int r = 0, k = 0, c = G.rowStart[0];
  u32 cur = 1;
  bool any = false;
  auto flush = [&]() {
    if (k >= G.rowLen[r]) return false;
    P.code[G.rowStart[r] + k] = (u32)(any ? cur : 1);
    k++;
    cur = 1;
    any = false;
    return true;
  };
  (void)c;
  for (char* p = board;; p++) {
    char ch = *p;
    if (ch == 'w' || ch == 'b') {
      cur = (cur << 1) | (ch == 'b');
      any = true;
    } else if (ch == '-') {
      any = false;
    } else if (ch == ',') {
      if (!flush()) return false;
    } else if (ch == '/') {
      if (!flush()) return false;
      if (k != G.rowLen[r]) return false;
      r++;
      k = 0;
      if (r >= G.rows) return false;
    } else if (ch == 0) {
      if (!flush()) return false;
      break;
    } else
      return false;
  }
  if (r != G.rows - 1 || k != G.rowLen[r]) return false;
  P.side = sd[0] == 'b';
  P.ply = ply;
  P.cap[0] = cw;
  P.cap[1] = cb;
  recompute(P);
  return true;
}

static string moveStr(int m) { return G.name[m / 6] + DNAME[m % 6]; }
static int parseMove(const string& s) {
  size_t i = 1;
  if (s.empty()) return -1;
  while (i < s.size() && isdigit((unsigned char)s[i])) i++;
  string cell = s.substr(0, i), dir = s.substr(i);
  for (auto& ch : cell) ch = (char)tolower(ch);
  for (auto& ch : dir) ch = (char)toupper(ch);
  int c = -1, d = -1;
  for (int k = 0; k < G.N; k++)
    if (G.name[k] == cell) c = k;
  for (int k = 0; k < 6; k++)
    if (dir == DNAME[k]) d = k;
  if (c < 0 || d < 0) return -1;
  return c * 6 + d;
}

// splitmix32-based opening, as in RULES.md
static void initialPos(Pos& P, u32 seed) {
  u32 a = seed;
  auto next = [&]() {
    a += 0x9E3779B9u;
    u32 z = a;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
  };
  memset(P.code, 0, sizeof(P.code));
  for (int c = 0; c < G.N; c++) {
    int an = G.antipode[c];
    if (an == c) {
      P.code[c] = 1;
      continue;
    }
    if (an < c) continue;
    u32 x = next();
    int blackHere = x & 1;
    P.code[c] = (u32)(2 | blackHere);
    P.code[an] = (u32)(2 | (blackHere ^ 1));
  }
  P.side = 0;
  P.ply = 0;
  P.cap[0] = P.cap[1] = 0;
  recompute(P);
}

// ---------------------------------------------------------------- terminal / eval

static const int WIN = 20000;
static const int MATE_BOUND = WIN - 400;

static inline bool isTerminal(const Pos& P) { return P.ply >= G.maxply || P.stk[P.side] == 0; }

// Terminal value from the side to move's perspective, distance from root = dist
static inline int terminalValue(const Pos& P, int dist) {
  int wm = (P.stk[0] + P.cap[0]) * 100 - (P.stk[1] + P.cap[1]) * 100 - G.komi100;
  int v = WIN - dist;
  int forWhite = wm > 0 ? v : -v;
  return P.side == 0 ? forWhite : -forWhite;
}

// Evaluation from the side to move's view. acc: that side's accumulator (net only).
static inline int evalCore(const int16_t* acc, int stm, int sd, int cd, int ply) {
  int mnow = sd + cd;
  int komiStm = stm == 0 ? G.komi100 : -G.komi100;
  if (!netOn) return mnow * 100 - komiStm;
  const int H = HC;
  const int16_t* tc = tabCd[(cd < -64 ? -64 : (cd > 63 ? 63 : cd)) + 64];
  const int16_t* ts = tabSd[(sd < -64 ? -64 : (sd > 63 ? 63 : sd)) + 64];
  const int16_t* tp = tabPly[ply > 255 ? 255 : ply];
  int sum = 0;
  for (int j = 0; j < H; j++) {
    int pre = (int16_t)(acc[j] + tc[j] + ts[j] + tp[j]);
    sum += (pre > 0 ? pre : 0) * (int)netVq[j];
  }
  int cp = mnow * 100 + netC0cp + (int)lrintf((float)sum * (100.0f / (float)(QA * QV))) - komiStm;
  return cp > 9000 ? 9000 : (cp < -9000 ? -9000 : cp);
}

static inline int evalStatic(const Pos& P) {
  int stm = P.side;
  return evalCore(P.acc[stm], stm, P.stk[stm] - P.stk[stm ^ 1], P.cap[stm] - P.cap[stm ^ 1], P.ply);
}

// ---------------------------------------------------------------- transposition table

struct TTEntry {
  u64 key;
  i16 score;
  u16 move;
  i8 depth;
  u8 flag;  // low 2 bits bound, upper 6 bits age
  i16 pad;
};
enum { B_LOWER = 1, B_UPPER = 2, B_EXACT = 3 };
static TTEntry* tt = nullptr;
static size_t ttMask = 0;  // number of buckets - 1
static u8 ttAge = 0;

static void initTT(size_t mb) {
  size_t entries = mb * 1024 * 1024 / sizeof(TTEntry);
  size_t buckets = 1;
  while (buckets * 2 * 4 <= entries) buckets *= 2;
  ttMask = buckets - 1;
  tt = (TTEntry*)calloc(buckets * 4, sizeof(TTEntry));
}

static inline int scoreToTT(int s, int dist) {
  if (s > MATE_BOUND) return s + dist;
  if (s < -MATE_BOUND) return s - dist;
  return s;
}
static inline int scoreFromTT(int s, int dist) {
  if (s > MATE_BOUND) return s - dist;
  if (s < -MATE_BOUND) return s + dist;
  return s;
}

static inline TTEntry* ttProbe(u64 key, bool& found) {
  TTEntry* b = &tt[(key & ttMask) * 4];
  for (int i = 0; i < 4; i++)
    if (b[i].key == key) {
      found = true;
      return &b[i];
    }
  found = false;
  return nullptr;
}

static inline void ttStore(u64 key, int score, int move, int depth, int bound, int dist) {
  TTEntry* b = &tt[(key & ttMask) * 4];
  TTEntry* rep = nullptr;
  int worst = 1 << 30;
  for (int i = 0; i < 4; i++) {
    if (b[i].key == key) {
      rep = &b[i];
      if (!move) move = b[i].move;
      if (depth < b[i].depth && bound != B_EXACT && (b[i].flag >> 2) == ttAge) return;
      goto write;
    }
    int age = (b[i].flag >> 2);
    int pr = b[i].key == 0 ? -1000000 : b[i].depth - 8 * ((ttAge - age) & 63);
    if (pr < worst) {
      worst = pr;
      rep = &b[i];
    }
  }
write:
  rep->key = key;
  rep->score = (i16)scoreToTT(score, dist);
  rep->move = (u16)move;
  rep->depth = (i8)depth;
  rep->flag = (u8)(bound | (ttAge << 2));
}

// ---------------------------------------------------------------- search

using Clock = chrono::steady_clock;
static Clock::time_point tStart;
static double hardMs = 0;
static u64 nodes = 0, nodeLimit = 0;
static bool stopped = false;
static int rootPly = 0;
static bool verbose = false;

static int moveBuf[MAXPLY + 4][MAXMOVES];
static int scoreBuf[MAXPLY + 4][MAXMOVES];
static int statBuf[MAXPLY + 4][MAXMOVES];
static int killer[MAXPLY + 4][2];
static int hist[2][MAXN * 6];

// tunable search parameters
static int P_lmrMinDepth = 2;  // late-move reductions
static int P_lmrAfter = 2;
static int P_lmrAfter2 = 6;
static int P_lmrAfter3 = 99;
static int P_lmrGap = 100;
static int P_nmpMin = 2;
static int P_nmpR = 2;
static int P_pcMin = 99;   // ProbCut min depth (99 = off)
static int P_pcMargin = 200;
static int P_pcR = 3;
static int P_pcGate = 100;
static int P_nmpMargin = 0;  // null move requires static eval >= beta + margin
static int P_nmpDeep = 99;   // reduction grows by one at this depth and above
static int P_nmpEnd = 0;   // no null move when this few plies remain
static int P_nmpStk = 0;   // no null move when the mover controls fewer stacks
static int P_asp = 0;  // root aspiration half-window in cp (0 = full window)
static int P_fut = 99999;
static int P_sel1 = 0;
static int P_hshift = 4;
static int P_killerBonus = 50;
static int P_hbonus1 = 2;

static inline double elapsedMs() { return chrono::duration<double, milli>(Clock::now() - tStart).count(); }

static int checkCounter = 0;
static inline void checkStop() {
  if (--checkCounter > 0) return;
  checkCounter = 64;
  if (nodeLimit && nodes >= nodeLimit) stopped = true;
  else if (hardMs > 0 && elapsedMs() >= hardMs) stopped = true;
}

static inline int leafValue(const Pos& P, int dist) {
  return isTerminal(P) ? terminalValue(P, dist) : evalStatic(P);
}

static inline int leafAfterMove(Pos& P, int m, int dist, int* gainOut = nullptr);

#ifdef STATS
static u64 stFut = 0, stInterior = 0, stCut[8], stNoCut = 0, stTTcut = 0, stTTmove = 0, stTTmoveFirstCut = 0, stCutTotal = 0;
#define STAT(x) x
#else
#define STAT(x)
#endif
// Value (from the opponent's view, i.e. the side to move after the move) of the position after
// playing m, computed without a full make: only the needed accumulator view is updated, on a
// local copy, and P.code is restored. Equivalent to makeMove + leafValue + unmakeMove.
static inline int leafAfterMove(Pos& P, int m, int dist, int* gainOut) {
  const int mover = P.side, opp = mover ^ 1;
  int stk[2] = {P.stk[0], P.stk[1]}, cap[2] = {P.cap[0], P.cap[1]};
  alignas(16) int16_t acc[HC];
  if (netOn) memcpy(acc, P.acc[opp], sizeof(acc));
  u16 savedCell[2 * MAXH + 4]; u32 savedOld[2 * MAXH + 4];
  int ns = 0;
  auto setc = [&](int t, u32 nc) {
    u32 oc = P.code[t];
    savedCell[ns] = (u16)t;
    savedOld[ns] = oc;
    ns++;
    P.code[t] = nc;
    if (oc > 1) stk[oc & 1]--;
    if (nc > 1) stk[nc & 1]++;
    if (netOn) {
      const bool fo = oc > 1 && oc < KC, fn = nc > 1 && nc < KC;
      const int ov = opp ? FLP[oc & (KC - 1)] : oc, nv = opp ? FLP[nc & (KC - 1)] : nc;
      if (fo && fn) {
        const int16_t* wo = &netW1q[(size_t)(t * KC + ov) * HC];
        const int16_t* wn = &netW1q[(size_t)(t * KC + nv) * HC];
        for (int j = 0; j < HC; j++) acc[j] = (int16_t)(acc[j] + wn[j] - wo[j]);
      } else if (fn) {
        const int16_t* wn = &netW1q[(size_t)(t * KC + nv) * HC];
        for (int j = 0; j < HC; j++) acc[j] = (int16_t)(acc[j] + wn[j]);
      } else if (fo) {
        const int16_t* wo = &netW1q[(size_t)(t * KC + ov) * HC];
        for (int j = 0; j < HC; j++) acc[j] = (int16_t)(acc[j] - wo[j]);
      }
    }
  };
  int c = m / 6;
  u32 code = P.code[c];
  int h = hgt(code);
  setc(c, 1);
  const i16* w = &G.walk[(size_t)m * G.maxH];
  for (int i = 0; i < h; i++) {
    int t = w[i];
    u32 col = (code >> (h - 1 - i)) & 1;
    setc(t, (u32)((P.code[t] << 1) | col));
  }
  for (int i = 0; i < h; i++) {
    int t = w[i];
    u32 ct = P.code[t];
    if (ct >= (u32)G.collapseCode) {
      int ht = hgt(ct);
      int blacks = __builtin_popcount(ct) - 1;
      cap[mover] += mover == 0 ? blacks : ht - blacks;
      setc(t, 1);
    }
  }
  for (int i = ns - 1; i >= 0; i--) P.code[savedCell[i]] = savedOld[i];
  int ply = P.ply + 1;
  if (ply >= G.maxply || stk[opp] == 0) {
    int wm = (stk[0] + cap[0]) * 100 - (stk[1] + cap[1]) * 100 - G.komi100;
    int v = WIN - dist;
    int forWhite = wm > 0 ? v : -v;
    if (gainOut) *gainOut = 1;
    return opp == 0 ? forWhite : -forWhite;
  }
  if (gainOut) *gainOut = (stk[mover] - stk[opp] + cap[mover] - cap[opp]) - (P.stk[mover] - P.stk[opp] + P.cap[mover] - P.cap[opp]);
  return evalCore(acc, opp, stk[opp] - stk[mover], cap[opp] - cap[mover], ply);
}

static int search(Pos& P, int depth, int alpha, int beta, bool pv, bool allowNull = true) {
  nodes++;
  checkStop();
  if (stopped) return 0;
  int dist = P.ply - rootPly;
  if (isTerminal(P)) return terminalValue(P, dist);
  if (depth <= 0) return evalStatic(P);

  u64 key_tt = ttKey(P);
  u64 key = key_tt;
  int ttMove = 0;
  bool found;
  TTEntry* e = ttProbe(key, found);
  if (found) {
    ttMove = e->move;
    if (!pv && e->depth >= depth) {
      int s = scoreFromTT(e->score, dist);
      int b = e->flag & 3;
      if (b == B_EXACT || (b == B_LOWER && s >= beta) || (b == B_UPPER && s <= alpha)) {
        STAT(stTTcut++;)
        return s;
      }
    }
  }

  int* mv = moveBuf[dist];
  int* sc = scoreBuf[dist];
  Undo U;
  int side = P.side;

  if (!pv && allowNull && depth >= P_nmpMin && beta < MATE_BOUND && beta > -MATE_BOUND && G.maxply - P.ply > P_nmpEnd &&
      P.stk[side] >= P_nmpStk && evalStatic(P) >= beta + P_nmpMargin) {
    P.side ^= 1;
    P.ply++;
    int v = -search(P, depth - 1 - P_nmpR - (depth >= P_nmpDeep ? 1 : 0), -beta, -beta + 1, false, false);
    P.side ^= 1;
    P.ply--;
    if (stopped) return 0;
    if (v >= beta) return v > MATE_BOUND ? beta : v;
  }
  if (!pv && depth >= P_pcMin && abs(beta) < MATE_BOUND && evalStatic(P) >= beta + P_pcMargin - P_pcGate) {
    int rb = beta + P_pcMargin;
    int v = search(P, depth - P_pcR, rb - 1, rb, false, false);
    if (stopped) return 0;
    if (v >= rb) return beta;
  }
  int n = genMoves(P, mv);

  if (depth == 1) {
    if (!pv && P_fut < 90000) {
      int e0 = evalStatic(P) + P_fut;
      if (e0 <= alpha) {
        STAT(stFut++;)
        return e0;
      }
    }
    // All children are leaves. Order cheaply (TT move, killers, history) so that cut-nodes
    // stop after a few evaluations, then evaluate in that order.
    int best = -WIN - 10, bestMove = 0;
    int* key = scoreBuf[dist];
    for (int i = 0; i < n; i++) {
      int m = mv[i];
      key[i] = hist[side][m];
      if (m == ttMove) key[i] += 1 << 28;
      else if (m == killer[dist][0]) key[i] += 1 << 27;
      else if (m == killer[dist][1]) key[i] += 1 << 26;
    }
    const int SEL = P_sel1;
    int a0 = alpha;
    for (int i = 0; i < n; i++) {
      if (i < SEL) {
        int bi = i;
        for (int j = i + 1; j < n; j++)
          if (key[j] > key[bi]) bi = j;
        if (bi != i) {
          swap(key[i], key[bi]);
          swap(mv[i], mv[bi]);
        }
      }
      int m = mv[i];
      nodes++;
      int v = -leafAfterMove(P, m, dist + 1);
      if (v > best) {
        best = v;
        bestMove = m;
        if (v > a0) a0 = v;
        if (v >= beta) {
          if (killer[dist][0] != m) {
            killer[dist][1] = killer[dist][0];
            killer[dist][0] = m;
          }
          hist[side][m] += P_hbonus1;
          break;
        }
      }
    }
    ttStore(key_tt, best, bestMove, 1, best >= beta ? B_LOWER : B_EXACT, dist);
    return best;
  }

  // Order: TT move first, then by one-ply evaluation of the child plus history.
  int* st = statBuf[dist];
  int maxStat = -WIN - 10;
  for (int i = 0; i < n; i++) {
    int m = mv[i];
    int v = -leafAfterMove(P, m, dist + 1);
    st[i] = v;
    if (v > maxStat) maxStat = v;
    if (m == ttMove) {
      sc[i] = 1 << 28;
      continue;
    }
    sc[i] = v + (hist[side][m] >> P_hshift);
    if (m == killer[dist][0] || m == killer[dist][1]) sc[i] += P_killerBonus;
  }
  int best = -WIN - 10, bestMove = 0;
  int origAlpha = alpha;
  for (int i = 0; i < n; i++) {
    int bi = i;
    for (int j = i + 1; j < n; j++)
      if (sc[j] > sc[bi]) bi = j;
    if (bi != i) {
      swap(sc[i], sc[bi]);
      swap(st[i], st[bi]);
      swap(mv[i], mv[bi]);
    }
    int m = mv[i];
    makeMove(P, m, U);
    int v;
    if (i == 0) v = -search(P, depth - 1, -beta, -alpha, pv);
    else {
      int red = 0;
      if (depth >= P_lmrMinDepth && i >= P_lmrAfter && !pv && maxStat - st[i] >= P_lmrGap) {
        red = 1;
        if (i >= P_lmrAfter2 && depth >= P_lmrMinDepth + 1) red = 2;
        if (i >= P_lmrAfter3 && depth >= P_lmrMinDepth + 3) red = 3;
      }
      v = -search(P, depth - 1 - red, -alpha - 1, -alpha, false);
      if (v > alpha && red && !stopped) v = -search(P, depth - 1, -alpha - 1, -alpha, false);
      if (v > alpha && v < beta && !stopped) v = -search(P, depth - 1, -beta, -alpha, true);
    }
    unmakeMove(P, U);
    if (stopped) return 0;
    if (v > best) {
      best = v;
      bestMove = m;
      if (v > alpha) {
        alpha = v;
        if (alpha >= beta) {
          STAT(stCutTotal++; stCut[i < 7 ? i : 7]++;)
          if (killer[dist][0] != m) {
            killer[dist][1] = killer[dist][0];
            killer[dist][0] = m;
          }
          int bonus = depth * depth;
          hist[side][m] += bonus;
          for (int j = 0; j < i; j++) hist[side][mv[j]] -= bonus;
          break;
        }
      }
    }
  }
  STAT(stInterior++;)
  int bound = best >= beta ? B_LOWER : (best > origAlpha ? B_EXACT : B_UPPER);
  ttStore(key, best, bestMove, depth, bound, dist);
  return best;
}

struct SearchResult {
  int move = 0, score = 0, depth = 0;
  u64 nodes = 0;
};

static SearchResult think(Pos& P, double budgetMs, int maxDepth, u64 nodeCap) {
  tStart = Clock::now();
  hardMs = budgetMs;
  nodeLimit = nodeCap;
  nodes = 0;
  stopped = false;
  rootPly = P.ply;
  ttAge = (ttAge + 1) & 63;
  memset(killer, 0, sizeof(killer));
  for (auto& h : hist)
    for (auto& x : h) x >>= 2;

  SearchResult res;
  static int rootMoves[MAXMOVES], rootKey[MAXMOVES];
  int n = genMoves(P, rootMoves);
  if (n == 0) return res;
  res.move = rootMoves[0];
  if (n == 1) return res;
  Undo U;
  for (int i = 0; i < n; i++) {
    rootKey[i] = -leafAfterMove(P, rootMoves[i], 1);
  }
  // initial ordering: best one-ply eval first
  {
    vector<int> order(n);
    for (int i = 0; i < n; i++) order[i] = i;
    stable_sort(order.begin(), order.end(), [&](int a, int b) { return rootKey[a] > rootKey[b]; });
    vector<int> tmp(n);
    for (int i = 0; i < n; i++) tmp[i] = rootMoves[order[i]];
    for (int i = 0; i < n; i++) rootMoves[i] = tmp[i];
  }
  res.move = rootMoves[0];
  int prevScore = 0;
  for (int depth = 1; depth <= maxDepth; depth++) {
    int alphaW = -WIN - 10, betaW = WIN + 10;
    int delta = P_asp;
    if (P_asp > 0 && depth >= 3 && abs(prevScore) < MATE_BOUND) {
      alphaW = prevScore - delta;
      betaW = prevScore + delta;
    }
    bool complete = true, done = false;
    int bestV = -WIN - 10, bestIdx = -1;
    while (!done) {
      int alpha = alphaW, beta = betaW;
      bestV = -WIN - 10;
      bestIdx = -1;
      complete = true;
      for (int i = 0; i < n; i++) {
        makeMove(P, rootMoves[i], U);
        int v;
        if (i == 0) v = -search(P, depth - 1, -beta, -alpha, true);
        else {
          v = -search(P, depth - 1, -alpha - 1, -alpha, false);
          if (v > alpha && v < beta && !stopped) v = -search(P, depth - 1, -beta, -alpha, true);
        }
        unmakeMove(P, U);
        if (stopped) {
          complete = false;
          break;
        }
        if (v > bestV) {
          bestV = v;
          bestIdx = i;
          if (v > alpha) alpha = v;
          if (alpha >= beta) break;  // fail high
        }
      }
      if (!complete) break;
      if (bestV <= alphaW && alphaW > -WIN - 10) {  // fail low: widen downwards
        delta *= 4;
        alphaW = (delta > 2000) ? -WIN - 10 : max(-WIN - 10, prevScore - delta);
      } else if (bestV >= betaW && betaW < WIN + 10) {  // fail high: widen upwards
        delta *= 4;
        betaW = (delta > 2000) ? WIN + 10 : min(WIN + 10, prevScore + delta);
        // keep the move that failed high first for the re-search
        if (bestIdx > 0) {
          int bm = rootMoves[bestIdx];
          for (int i = bestIdx; i > 0; i--) rootMoves[i] = rootMoves[i - 1];
          rootMoves[0] = bm;
        }
      } else done = true;
    }
    // an interrupted iteration may still have produced a trustworthy better move
    if (bestIdx >= 0 && (complete || bestV > alphaW)) {
      res.move = rootMoves[bestIdx];
      res.score = bestV;
      prevScore = bestV;
      int bm = rootMoves[bestIdx];
      for (int i = bestIdx; i > 0; i--) rootMoves[i] = rootMoves[i - 1];
      rootMoves[0] = bm;
    }
    if (complete) res.depth = depth;
    if (!complete) break;
    if (verbose)
      fprintf(stderr, "info depth %d score %d nodes %llu ms %.0f move %s\n", depth, bestV, (unsigned long long)nodes,
              elapsedMs(), moveStr(res.move).c_str());
    if (abs(bestV) > MATE_BOUND) break;  // forced result
  }
  res.nodes = nodes;
  return res;
}

// ---------------------------------------------------------------- protocol / modes

static void setupGame(int side, int collapse, int maxply, double komi) {
  initGeo(side, collapse, maxply, komi);
  initEval();
}

#ifdef EMBED_NET
#include "netdata.h"
#endif

int main(int argc, char** argv) {
  initZobrist();
#ifdef EMBED_NET
  loadNetBuf(NET_DATA, sizeof(NET_DATA));
#endif
  setupGame(5, 6, 150, 0.5);
  initTT(64);
  Pos P;
  initialPos(P, 0);
  char line[16384];
  setvbuf(stdout, nullptr, _IOLBF, 0);
  while (fgets(line, sizeof(line), stdin)) {
    string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (s.empty()) continue;
    size_t sp = s.find(' ');
    string cmd = s.substr(0, sp), rest = sp == string::npos ? "" : s.substr(sp + 1);
    if (cmd == "cascade") {
      printf("name cascade-ab\nready\n");
      fflush(stdout);
    } else if (cmd == "newgame") {
      int side = 5, collapse = 6, maxply = 150;
      double komi = 0.5;
      size_t pos = 0;
      while (pos < rest.size()) {
        size_t e = rest.find(' ', pos);
        if (e == string::npos) e = rest.size();
        string tok = rest.substr(pos, e - pos);
        size_t eq = tok.find('=');
        if (eq != string::npos) {
          string k = tok.substr(0, eq);
          double v = atof(tok.substr(eq + 1).c_str());
          if (k == "side") side = (int)v;
          else if (k == "collapse") collapse = (int)v;
          else if (k == "maxply") maxply = (int)v;
          else if (k == "komi") komi = v;
        }
        pos = e + 1;
      }
      setupGame(side, collapse, maxply, komi);
    } else if (cmd == "position") {
      fromCSN(rest, P);
    } else if (cmd == "go") {
      double movetime = 0;
      u64 nodeCap = 0;
      int maxDepth = 60;
      char key[32];
      long long val;
      const char* p = rest.c_str();
      int off;
      while (sscanf(p, "%31s %lld%n", key, &val, &off) == 2) {
        if (!strcmp(key, "movetime")) movetime = (double)val;
        else if (!strcmp(key, "nodes")) nodeCap = (u64)val;
        else if (!strcmp(key, "depth")) maxDepth = (int)min<long long>(val, 100);
        p += off;
      }
      double budget = movetime > 0 ? max(5.0, movetime - 12.0) : 0;
      SearchResult r = think(P, budget, maxDepth, nodeCap);
      printf("bestmove %s\n", moveStr(r.move).c_str());
      fflush(stdout);
    } else if (cmd == "quit") {
      break;
    }
  }
  return 0;
}
