// Evaluation and alpha-beta search.
#pragma once
#include "nn.hpp"
#include <cmath>

namespace cz {

// Tunable parameters.
enum P {
  P_TEMPO,
  P_H1, P_H2, P_H3, P_H4, P_H5,       // controlled stack of height h (beyond the 1 point)
  P_OWNB, P_ENB,                       // own / enemy pieces buried in a controlled stack
  P_RING0, P_RING1, P_RING2, P_RING3, P_RING4,
  P_MOB,                               // per own move direction
  P_LMR, P_NULL, P_QS, P_FUT, P_NNSCALE, P_LMP, P_ASP, P_LMRDIV, P_LMPD, P_FFUT, P_QSG,
  P_COUNT
};

struct Params {
  int v[P_COUNT];
  Params() {
    int d[P_COUNT] = {
      30,
      0, 0, 0, 0, 0,
      0, 0,
      0, 0, 0, 0, 0,
      0,
      1, 0, 1, 60, 170, 4, 150, 200, 3, 0, 0,
    };
    memcpy(v, d, sizeof v);
  }
  int operator[](int i) const { return v[i]; }
};

static const char* PNAMES[P_COUNT] = {
  "tempo", "h1", "h2", "h3", "h4", "h5", "ownb", "enb",
  "ring0", "ring1", "ring2", "ring3", "ring4", "mob", "lmr", "null", "qs", "fut", "nnscale", "lmp", "asp", "lmrdiv", "lmpd", "ffut", "qsg",
};

constexpr int INF = 32000;
constexpr int WIN = 20000;

inline int evaluate(const Pos& p, const Params& pr) {
  const Tables& t = T();
  int s[2] = {0, 0};
  for (int i = 0; i < N; i++) {
    uint8_t code = p.c[i];
    if (code == EMPTY) continue;
    int x = t.TOP[code];
    int h = t.H[code];
    int own = t.CNT[code][x];
    s[x] += pr.v[P_H1 + h - 1] + pr.v[P_OWNB] * (own - 1) + pr.v[P_ENB] * (h - own)
          + pr.v[P_RING0 + t.ring[i]] + pr.v[P_MOB] * t.ndir[i];
  }
  int me = p.stm;
  int mat = 50 * p.marginHalf(me);
  return mat + s[me] - s[me ^ 1] + pr.v[P_TEMPO];
}

struct TTEntry {
  uint64_t key;
  int16_t value;
  int16_t move;
  int8_t depth;
  uint8_t flag;  // 1 lower, 2 upper, 3 exact
  uint8_t age;
  uint8_t pad;
};

enum { TT_LOWER = 1, TT_UPPER = 2, TT_EXACT = 3 };

struct Searcher {
  Params pr;
  const Net* net = nullptr;

  alignas(64) float accs[168][512];

  void makeChild(const Pos& p, Pos& q, Move m, int ply) {
    q = p;
    if (net) {
      Delta d;
      q.make(m, &d);
      net->accUpdate(accs[ply], accs[ply + 1], q, d);
    } else {
      q.make(m);
    }
  }

  int eval(const Pos& p, int ply) const {
    if (net) {
      float v = net->evalAcc(p, accs[ply]) * pr.v[P_NNSCALE];
      if (v > 15000) v = 15000;
      if (v < -15000) v = -15000;
      return (int)v;
    }
    return evaluate(p, pr);
  }
  TTEntry* tt = nullptr;
  uint64_t ttMask = 0;
  uint8_t age = 0;

  int killers[160][2];
  int lmrTable[64][64];

  void initLmr() {
    for (int d = 0; d < 64; d++)
      for (int i = 0; i < 64; i++)
        lmrTable[d][i] = (d && i) ? (int)(0.5 + std::log((double)d) * std::log((double)i) * 100.0 / pr.v[P_LMRDIV]) : 0;
  }
  int hist[2][N * 6];

  long long nodes = 0;
  long long nodeLimit = 0;
  bool stop = false;
  std::chrono::steady_clock::time_point t0;
  double hardMs = 1e18, softMs = 1e18;

  Move rootBest = -1;
  int rootScore = 0;
  int depthDone = 0;
  bool verbose = false;

  explicit Searcher(int ttBits = 22) {
    tt = (TTEntry*)calloc((size_t)1 << ttBits, sizeof(TTEntry));
    ttMask = ((uint64_t)1 << ttBits) - 1;
  }
  ~Searcher() { free(tt); }
  Searcher(const Searcher&) = delete;

  void clearTT() { memset(tt, 0, (ttMask + 1) * sizeof(TTEntry)); }

  double elapsedMs() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }

  void checkTime() {
    if (nodeLimit && nodes >= nodeLimit) stop = true;
    if ((nodes & 1023) == 0 && elapsedMs() > hardMs) stop = true;
  }

  int terminalValue(const Pos& p) const {
    int m = p.marginHalf(p.stm);
    return m > 0 ? WIN + 50 * m : -WIN + 50 * m;
  }

  // Static gain of a move for ordering: material change for the mover, in half points.
  // Sets *capt to the number of pieces captured.
  static int gain(const Pos& p, Move m, int* capt = nullptr) {
    const Tables& t = T();
    int cell = m / 6, d = m % 6, me = p.stm;
    uint8_t code = p.c[cell];
    int h = t.H[code];
    int bits = code ^ (1 << h);
    int tc[MAXH + 1];
    uint8_t orig[MAXH + 1], cur[MAXH + 1];
    int nt = 1;
    tc[0] = cell; orig[0] = code; cur[0] = EMPTY;
    const int8_t* w = t.walk[cell][d];
    for (int i = 0; i < h; i++) {
      int x = w[i], k = 0;
      while (k < nt && tc[k] != x) k++;
      if (k == nt) { tc[nt] = x; orig[nt] = cur[nt] = p.c[x]; nt++; }
      uint8_t o = cur[k];
      cur[k] = o + (uint8_t)((1 + ((bits >> i) & 1)) << t.H[o]);
    }
    int g = 0, cp = 0;
    for (int k = 0; k < nt; k++) {
      uint8_t o = orig[k], f = cur[k];
      if (t.H[f] >= COLLAPSE) { cp += t.CNT[f][me ^ 1]; f = EMPTY; }
      if (o != EMPTY) g -= t.TOP[o] == me ? 1 : -1;
      if (f != EMPTY) g += t.TOP[f] == me ? 1 : -1;
    }
    if (capt) *capt = cp;
    return 2 * (g + cp);
  }

  int qsearch(const Pos& p, int alpha, int beta, int ply, int qply = 0) {
    nodes++;
    checkTime();
    if (stop) return 0;
    if (p.terminal()) return terminalValue(p);
    int stand = eval(p, ply);
    if (stand >= beta) return stand;
    if (stand > alpha) alpha = stand;
    // Only moves that cause a collapse. A collapse needs a stack of height >= 4 on the
    // sowing path (a cell receives at most two pieces from one move).
    const Tables& t = T();
    bool tall[N];
    int ntall = 0;
    for (int i = 0; i < N; i++) { tall[i] = t.H[p.c[i]] >= 4; ntall += tall[i]; }
    if (!ntall && !pr.v[P_QSG]) return stand;
    Move ms[400];
    int n = p.genMoves(ms);
    Move cm[64];
    int sc[64];
    int k = 0;
    for (int i = 0; i < n && k < 64; i++) {
      if (!pr.v[P_QSG]) {
        int c0 = ms[i] / 6, h0 = t.H[p.c[c0]];
        const int8_t* w = t.walk[c0][ms[i] % 6];
        bool any = false;
        for (int j = 0; j < h0; j++) any |= tall[w[j]];
        if (!any) continue;
      }
      int cp;
      int g = gain(p, ms[i], &cp);
      if (cp == 0 && !(pr.v[P_QSG] && qply < 2 && g >= pr.v[P_QSG])) continue;
      cm[k] = ms[i];
      sc[k] = g;
      k++;
    }
    int best = stand;
    for (int i = 0; i < k; i++) {
      int bi = i;
      for (int j = i + 1; j < k; j++) if (sc[j] > sc[bi]) bi = j;
      if (bi != i) { std::swap(cm[i], cm[bi]); std::swap(sc[i], sc[bi]); }
      Pos q;
      makeChild(p, q, cm[i], ply);
      int v = -qsearch(q, -beta, -alpha, ply + 1, qply + 1);
      if (stop) return 0;
      if (v > best) {
        best = v;
        if (v > alpha) {
          alpha = v;
          if (v >= beta) break;
        }
      }
    }
    return best;
  }

  int search(const Pos& p, int depth, int alpha, int beta, int ply, bool pvNode, bool allowNull) {
    if (p.terminal()) { nodes++; return terminalValue(p); }
    if (depth <= 0) {
      if (pr.v[P_QS]) return qsearch(p, alpha, beta, ply);
      nodes++;
      checkTime();
      return eval(p, ply);
    }
    nodes++;
    checkTime();
    if (stop) return 0;

    TTEntry& e = tt[p.key & ttMask];
    Move ttMove = -1;
    if (e.key == p.key) {
      ttMove = e.move;
      if (!pvNode && e.depth >= depth) {
        int v = e.value;
        if (e.flag == TT_EXACT) return v;
        if (e.flag == TT_LOWER && v >= beta) return v;
        if (e.flag == TT_UPPER && v <= alpha) return v;
      }
    }

    int staticEval = -INF;
    if (!pvNode && (pr.v[P_NULL] || pr.v[P_FUT] || pr.v[P_FFUT])) staticEval = eval(p, ply);

    // Reverse futility pruning: far above beta near the leaves.
    if (pr.v[P_FUT] && !pvNode && depth <= 3 && staticEval - pr.v[P_FUT] * depth >= beta && staticEval < WIN / 2)
      return staticEval;

    // Null move pruning.
    if (pr.v[P_NULL] && allowNull && !pvNode && depth >= 3 && staticEval >= beta && p.ply + 1 < MAXPLY) {
      Pos q = p;
      if (net) memcpy(accs[ply + 1], accs[ply], sizeof accs[0]);
      q.stm ^= 1;
      q.key ^= T().Zside ^ T().Zply[q.ply] ^ T().Zply[q.ply + 1];
      q.ply++;
      if (q.hasMoves()) {
        int Rn = 2 + depth / 4;
        int v = -search(q, depth - 1 - Rn, -beta, -beta + 1, ply + 1, false, false);
        if (stop) return 0;
        if (v >= beta && v < WIN) return v;
      }
    }

    int best = -INF;
    Move bestMove = -1;
    int origAlpha = alpha;
    int gain0 = 0;
    const Tables& t = T();

    // Searches move m as the i-th move. Returns 1 on a beta cutoff, 2 if time ran out.
    auto tryMove = [&](Move m, int i, int g) -> int {
      if (i == 0) gain0 = g;
      if (pr.v[P_LMP] && !pvNode && depth <= pr.v[P_LMPD] && i >= pr.v[P_LMP] * depth && best > -WIN / 2 &&
          g <= 0 && m != killers[ply][0] && m != killers[ply][1])
        return 0;
      if (pr.v[P_FFUT] && !pvNode && depth == 1 && i > 0 && g <= 0 && best > -WIN / 2 &&
          staticEval + pr.v[P_FFUT] <= alpha)
        return 0;
      Pos q;
      makeChild(p, q, m, ply);
      int v;
      if (i == 0) {
        v = -search(q, depth - 1, -beta, -alpha, ply + 1, pvNode, true);
      } else {
        int red = 0;
        if (pr.v[P_LMR] && depth >= 3 && i >= 3 && m != killers[ply][0] && m != killers[ply][1]) {
          if (pr.v[P_LMR] >= 2) {
            red = lmrTable[std::min(depth, 63)][std::min(i, 63)];
            if (pvNode && red > 0) red--;
          } else {
            red = 1;
            if (i >= 10 && depth >= 4) red = 2;
            if (g < gain0 - 2 && depth >= 5) red++;
          }
          if (red > depth - 2) red = depth - 2;
          if (red < 0) red = 0;
        }
        v = -search(q, depth - 1 - red, -alpha - 1, -alpha, ply + 1, false, true);
        if (!stop && v > alpha && red)
          v = -search(q, depth - 1, -alpha - 1, -alpha, ply + 1, false, true);
        if (!stop && v > alpha && v < beta && pvNode)
          v = -search(q, depth - 1, -beta, -alpha, ply + 1, true, true);
      }
      if (stop) return 2;
      if (v > best) {
        best = v;
        bestMove = m;
        if (ply == 0) { rootBest = m; rootScore = v; }
        if (v > alpha) {
          alpha = v;
          if (v >= beta) {
            if (killers[ply][0] != m) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
            hist[p.stm][m] += depth * depth;
            if (hist[p.stm][m] > 30000) for (int k = 0; k < N * 6; k++) { hist[0][k] /= 2; hist[1][k] /= 2; }
            return 1;
          }
        }
      }
      return 0;
    };

    int idx = 0;
    int r = 0;
    bool ttLegal = ttMove >= 0 && ttMove < N * 6 && t.TOP[p.c[ttMove / 6]] == p.stm && t.nb[ttMove / 6][ttMove % 6] >= 0;
    if (ttLegal) {
      r = tryMove(ttMove, idx++, gain(p, ttMove));
      if (r == 2) return 0;
    } else {
      ttMove = -1;
    }
    if (r == 0) {
      Move ms[400];
      int n0 = p.genMoves(ms);
      int64_t keys[400];
      int gains[400];
      Move mv[400];
      int n = 0;
      const int* h = hist[p.stm];
      for (int i = 0; i < n0; i++) {
        Move m = ms[i];
        if (m == ttMove) continue;
        int g = gain(p, m);
        int sc = g * 100000;
        if (m == killers[ply][0]) sc += 50000;
        else if (m == killers[ply][1]) sc += 40000;
        sc += h[m];
        gains[n] = g;
        mv[n] = m;
        keys[n] = (int64_t)sc * 65536 + n;
        n++;
      }
      const int PICK = 3;
      for (int i = 0; i < n; i++) {
        if (i < PICK) {
          int bi = i;
          for (int j = i + 1; j < n; j++) if (keys[j] > keys[bi]) bi = j;
          std::swap(keys[i], keys[bi]);
        } else if (i == PICK) {
          std::sort(keys + PICK, keys + n, [](int64_t a, int64_t b) { return a > b; });
        }
        int k = (int)(keys[i] & 0xffff);
        r = tryMove(mv[k], idx++, gains[k]);
        if (r) break;
      }
      if (r == 2) return 0;
    }

    int flag = best >= beta ? TT_LOWER : (best > origAlpha ? TT_EXACT : TT_UPPER);
    if (e.key != p.key || depth >= e.depth || e.age != age || flag == TT_EXACT) {
      e.key = p.key;
      e.value = (int16_t)best;
      e.move = (int16_t)bestMove;
      e.depth = (int8_t)depth;
      e.flag = (uint8_t)flag;
      e.age = age;
    }
    return best;
  }

  // Returns the best move. movetimeMs <= 0 means no time limit (use depth/nodes).
  Move think(const Pos& root, double softMsIn, double hardMsIn, int maxDepth = 64, long long nodeLim = 0) {
    t0 = std::chrono::steady_clock::now();
    softMs = softMsIn; hardMs = hardMsIn;
    nodeLimit = nodeLim;
    nodes = 0;
    stop = false;
    age++;
    memset(killers, -1, sizeof killers);
    initLmr();
    if (net) net->accFull(root, accs[0]);
    for (int k = 0; k < N * 6; k++) { hist[0][k] /= 8; hist[1][k] /= 8; }
    Move ms[400];
    int n = root.genMoves(ms);
    Move best = n ? ms[0] : -1;
    if (n <= 1) return best;
    depthDone = 0;
    int prevScore = 0;
    for (int d = 1; d <= maxDepth; d++) {
      rootBest = -1;
      int v;
      if (d >= 4) {
        int win = pr.v[P_ASP];
        int a = prevScore - win, b = prevScore + win;
        for (;;) {
          v = search(root, d, a, b, 0, true, false);
          if (stop) break;
          if (v <= a) { a = std::max(-INF, a - win * 4); win *= 4; }
          else if (v >= b) { b = std::min(INF, b + win * 4); win *= 4; }
          else break;
          if (rootBest >= 0) best = rootBest;
        }
      } else {
        v = search(root, d, -INF, INF, 0, true, false);
      }
      if (stop) {
        if (rootBest >= 0) best = rootBest;  // partial iteration: first move searched is the TT move
        break;
      }
      best = rootBest >= 0 ? rootBest : best;
      prevScore = v;
      depthDone = d;
      if (verbose)
        fprintf(stderr, "depth %d score %d nodes %lld time %.0f best %s\n", d, v, nodes, elapsedMs(),
                root.moveStr(best).c_str());
      if (std::abs(v) >= WIN) {
        // Proven result; deeper search cannot change it unless the line is longer.
        if (d >= MAXPLY - root.ply) break;
      }
      if (elapsedMs() > softMs) break;
      if (nodeLimit && nodes >= nodeLimit) break;
    }
    rootScore = prevScore;
    return best;
  }
};

}  // namespace cz
