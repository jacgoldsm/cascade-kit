// Cascade engine (see RULES.md and PROTOCOL.md).
//
// Principal-variation alpha-beta search with iterative deepening and aspiration
// windows, a transposition table, killer and history ordering, late-move reductions,
// and a quiescence search over collapsing moves.
//
// The evaluation, from white's point of view, is the score margin (stacks controlled
// plus captures, less komi), plus a small term for the pieces each side has on the
// board and for the tall stacks each side controls. The margin alone decides finished
// games. Move generation and collapse detection use per-cell bitsets, so that each
// node costs little.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMaxCells = 469;  // side 13
constexpr int kMaxH = 11;       // collapse 12, so stacks hold at most 11 between moves
constexpr int kMaxMoves = kMaxCells * 6;
constexpr int kWords = (kMaxCells + 63) / 64;
constexpr int kMaxPly = 256;
constexpr int kMaxQ = 6;        // quiescence depth limit, in plies
constexpr int kScale = 8;       // evaluation units per point
constexpr int kWin = 1000000;
constexpr int kInf = 1 << 29;

// Search and evaluation settings, chosen by self-play (see the notes in the README
// of the submission). Evaluation units are 1/kScale of a point.
constexpr int kLmrFirst = 3;     // quiet moves from this index on are searched a ply shallower
constexpr int kLmrSecond = 8;    // ... and from this index on, two plies shallower
constexpr int kPieceWeight = 10; // per piece that white has on the board, minus black's
constexpr int kTallWeight = 4;   // per stack of height 3+ that white controls, minus black's
constexpr double kBudgetFraction = 0.94;  // share of movetime spent searching

const char *const kDirName[6] = {"E", "NE", "NW", "W", "SW", "SE"};
const int kDQ[6] = {1, 1, 0, -1, -1, 0};
const int kDR[6] = {0, -1, -1, 0, 1, 1};

// One cell. Bit i of b is the colour of the i-th piece from the bottom (1 = black).
struct Cell {
  uint32_t b;
  uint8_t h;
};

struct Pos {
  Cell c[kMaxCells];
  uint64_t ctrl[2][kWords];  // cells whose stack is controlled by white (0) or black (1)
  uint64_t hot[kWords];      // cells tall enough that one sow might collapse them
  uint64_t tall[kWords];     // cells holding a stack of height 3 or more
  int side;  // 0 = white to move, 1 = black
  int ply;
  int capW, capB;  // pieces captured by each side
  int ws, bs;      // stacks controlled by each side
  int pw, pb;      // pieces on the board, by colour
  uint64_t key;
};

// The rules in use, set by `newgame`. Everything is derived from side and collapse.
struct Board {
  int side = 5, collapse = 6, maxply = 150, maxH = 5, N = 0, komi = 4;
  int words = 1;   // 64-bit words in a cell set
  int maxOcc = 1;  // most times one cell can be sown by a single move
  int rowStart[32], rowLen[32], rows = 0;
  int nb[kMaxMoves];              // nb[c * 6 + d]: neighbour, or -1 off the board
  int antipode[kMaxCells];
  std::vector<int> walk;          // walk[m * maxH + i]: cell of the (i+1)-th sown piece of move m
  // pathMask[((m * (maxH + 1)) + h) * words + k]: cells among the first h sown cells of m
  std::vector<uint64_t> pathMask;
  std::vector<uint64_t> zob;      // zob[(c << (maxH + 1)) | code]: key of a stack on cell c
  std::vector<std::string> name;  // name[c], e.g. "e5"
  uint8_t dirs[kMaxCells];        // bit d set if the neighbour in direction d is on the board
};
Board B;

// ---- Zobrist keys ----------------------------------------------------------

inline uint64_t mix64(uint64_t z) {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

const uint64_t kSideKey = mix64(0x51de51de51deULL);

// Key of a stack (one cell's contents). The code (1 << h) | b identifies the stack.
inline uint64_t cellKey(int c, const Cell &x) {
  if (!x.h) return 0;
  return B.zob[((size_t)c << (B.maxH + 1)) | ((1u << x.h) | x.b)];
}

uint64_t fullKey(const Pos &p) {
  uint64_t k = p.side ? kSideKey : 0;
  for (int c = 0; c < B.N; c++) k ^= cellKey(c, p.c[c]);
  return k;
}

// ---- board setup -----------------------------------------------------------

void setupBoard(int side, int collapse, int maxply, double komi) {
  B.side = side;
  B.collapse = collapse;
  B.maxply = maxply;
  B.maxH = collapse - 1;
  B.komi = (int)std::lround(komi * kScale);
  const int R = side - 1, S = 2 * R + 1;
  std::vector<int> grid(S * S, -1), qs, rs;
  B.name.clear();
  B.rows = S;
  int c = 0;
  for (int r = -R; r <= R; r++) {
    const int qmin = std::max(-R, -R - r), qmax = std::min(R, R - r);
    B.rowStart[r + R] = c;
    B.rowLen[r + R] = qmax - qmin + 1;
    for (int q = qmin; q <= qmax; q++) {
      grid[(q + R) * S + (r + R)] = c;
      B.name.push_back(std::string(1, (char)('a' + r + R)) + std::to_string(q - qmin + 1));
      qs.push_back(q);
      rs.push_back(r);
      c++;
    }
  }
  B.N = c;
  B.words = (B.N + 63) / 64;
  auto at = [&](int q, int r) -> int {
    if (std::abs(q) > R || std::abs(r) > R || std::abs(q + r) > R) return -1;
    return grid[(q + R) * S + (r + R)];
  };
  for (int i = 0; i < B.N; i++) {
    B.dirs[i] = 0;
    for (int d = 0; d < 6; d++) {
      B.nb[i * 6 + d] = at(qs[i] + kDQ[d], rs[i] + kDR[d]);
      if (B.nb[i * 6 + d] >= 0) B.dirs[i] |= (uint8_t)(1 << d);
    }
    B.antipode[i] = at(-qs[i], -rs[i]);
  }
  const int H = B.maxH, W = B.words;
  B.walk.assign((size_t)B.N * 6 * H, -1);
  B.pathMask.assign((size_t)B.N * 6 * (H + 1) * W, 0);
  B.maxOcc = 1;
  for (int i = 0; i < B.N; i++) {
    for (int d0 = 0; d0 < 6; d0++) {
      const int m = i * 6 + d0;
      if (B.nb[m] < 0) continue;
      int pos = i, d = d0;
      for (int k = 0; k < H; k++) {
        if (B.nb[pos * 6 + d] < 0) d = (d + 3) % 6;  // bounce off the edge
        pos = B.nb[pos * 6 + d];
        B.walk[(size_t)m * H + k] = pos;
      }
      for (int h = 1; h <= H; h++) {
        uint64_t *mask = &B.pathMask[((size_t)m * (H + 1) + h) * W];
        for (int k = 0; k < h; k++) {
          const int t = B.walk[(size_t)m * H + k];
          mask[t >> 6] |= 1ULL << (t & 63);
          int occ = 0;
          for (int j = 0; j < h; j++) occ += B.walk[(size_t)m * H + j] == t;
          B.maxOcc = std::max(B.maxOcc, occ);
        }
      }
    }
  }
  B.zob.assign((size_t)B.N << (H + 1), 0);
  for (int i = 0; i < B.N; i++) {
    for (int code = 1; code < (1 << (H + 1)); code++) {
      B.zob[((size_t)i << (H + 1)) | code] =
          mix64(((uint64_t)i << 32 | (uint64_t)code) * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL);
    }
  }
}

inline int topColour(const Cell &x) { return (x.b >> (x.h - 1)) & 1u; }

// Adds (sign = 1) or removes (sign = -1) a cell's contribution to the stack counts.
inline void account(int &ws, int &bs, const Cell &x, int sign) {
  if (!x.h) return;
  if (topColour(x)) bs += sign; else ws += sign;
}

// Sets the bitsets of one cell from its contents.
inline void setCellBits(Pos &p, int c) {
  const int w = c >> 6;
  const uint64_t bit = 1ULL << (c & 63);
  p.ctrl[0][w] &= ~bit;
  p.ctrl[1][w] &= ~bit;
  p.hot[w] &= ~bit;
  p.tall[w] &= ~bit;
  const Cell &x = p.c[c];
  if (!x.h) return;
  p.ctrl[topColour(x)][w] |= bit;
  if (x.h >= B.collapse - B.maxOcc) p.hot[w] |= bit;
  if (x.h >= 3) p.tall[w] |= bit;
}

// Recomputes the derived fields of a position from its cells.
void finishPos(Pos &p) {
  p.ws = p.bs = 0;
  std::memset(p.tall, 0, sizeof p.tall);
  p.pw = p.pb = 0;
  for (int c = 0; c < B.N; c++) {
    p.pb += __builtin_popcount(p.c[c].b);
    p.pw += p.c[c].h - __builtin_popcount(p.c[c].b);
  }
  std::memset(p.ctrl, 0, sizeof p.ctrl);
  std::memset(p.hot, 0, sizeof p.hot);
  for (int c = 0; c < B.N; c++) {
    account(p.ws, p.bs, p.c[c], 1);
    setCellBits(p, c);
  }
  p.key = fullKey(p);
}

// The opening for a seed (RULES.md section 3).
Pos initialPos(uint32_t seed) {
  Pos p;
  std::memset(&p, 0, sizeof p);
  uint32_t a = seed;
  auto draw = [&a]() {
    a += 0x9E3779B9u;
    uint32_t z = a;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
  };
  for (int c = 0; c < B.N; c++) {
    const int o = B.antipode[c];
    if (o <= c) continue;  // the centre, or a pair already assigned
    const uint32_t black = draw() & 1u;
    p.c[c].h = 1;
    p.c[c].b = black;
    p.c[o].h = 1;
    p.c[o].b = black ^ 1u;
  }
  p.side = 0;
  p.ply = 0;
  p.capW = p.capB = 0;
  finishPos(p);
  return p;
}

// ---- rules -----------------------------------------------------------------

inline bool gameOver(const Pos &p) {
  return p.ply >= B.maxply || (p.side == 0 ? p.ws == 0 : p.bs == 0);
}

// True if the move leaves some stack at or above the collapse height.
bool causesCollapse(const Pos &p, int m) {
  const int c = m / 6;
  const int h = p.c[c].h;
  const int *w = &B.walk[(size_t)m * B.maxH];
  for (int i = 0; i < h; i++) {
    const int t = w[i];
    const int base = (t == c) ? 0 : p.c[t].h;
    if (base + B.maxOcc < B.collapse) continue;
    int occ = 0;
    for (int j = 0; j < h; j++) occ += (w[j] == t);
    if (base + occ >= B.collapse) return true;
  }
  return false;
}

// Writes the legal moves of the side to move (cell * 6 + direction) to out, and sets
// tac[i] when the move causes a collapse. With tacticalOnly, only collapsing moves are
// written. Returns the count.
int genMoves(const Pos &p, int *out, bool *tac, bool tacticalOnly) {
  if (p.ply >= B.maxply) return 0;
  int n = 0;
  if (B.words == 1) {
    uint64_t bits = p.ctrl[p.side][0];
    const uint64_t hot = p.hot[0];
    while (bits) {
      const int c = __builtin_ctzll(bits);
      bits &= bits - 1;
      const int h = p.c[c].h;
      const uint64_t *pm = &B.pathMask[(size_t)c * 6 * (B.maxH + 1)];
      for (int d = 0; d < 6; d++) {
        const int m = c * 6 + d;
        if (!(B.dirs[c] & (1 << d))) continue;
        const bool t = (pm[d * (B.maxH + 1) + h] & hot) && causesCollapse(p, m);
        if (tacticalOnly && !t) continue;
        out[n] = m;
        if (tac) tac[n] = t;
        n++;
      }
    }
    return n;
  }
  const int W = B.words;
  for (int k = 0; k < W; k++) {
    uint64_t bits = p.ctrl[p.side][k];
    while (bits) {
      const int c = k * 64 + __builtin_ctzll(bits);
      bits &= bits - 1;
      const int h = p.c[c].h;
      for (int d = 0; d < 6; d++) {
        if (!(B.dirs[c] & (1 << d))) continue;
        const int m = c * 6 + d;
        const uint64_t *mask = &B.pathMask[((size_t)m * (B.maxH + 1) + h) * W];
        bool maybe = false;
        for (int j = 0; j < W; j++) maybe = maybe || (mask[j] & p.hot[j]);
        const bool t = maybe && causesCollapse(p, m);
        if (tacticalOnly && !t) continue;
        out[n] = m;
        if (tac) tac[n] = t;
        n++;
      }
    }
  }
  return n;
}

// Plays a legal move: sows the stack, collapses tall stacks, and passes the turn.
void makeMove(const Pos &p, int m, Pos &q) {
  std::memcpy(q.c, p.c, sizeof(Cell) * B.N);
  std::memcpy(q.ctrl, p.ctrl, sizeof q.ctrl);
  std::memcpy(q.hot, p.hot, sizeof q.hot);
  std::memcpy(q.tall, p.tall, sizeof q.tall);
  const int c = m / 6;
  const Cell src = p.c[c];
  const int *w = &B.walk[(size_t)m * B.maxH];

  // Cells whose contents change, each listed once.
  int touched[kMaxH + 1];
  int nt = 0;
  touched[nt++] = c;
  for (int i = 0; i < src.h; i++) {
    bool seen = false;
    for (int j = 0; j < nt; j++) seen = seen || touched[j] == w[i];
    if (!seen) touched[nt++] = w[i];
  }
  uint64_t key = p.key;
  int ws = p.ws, bs = p.bs, pw = p.pw, pb = p.pb;
  for (int j = 0; j < nt; j++) {
    key ^= cellKey(touched[j], q.c[touched[j]]);
    account(ws, bs, q.c[touched[j]], -1);
    pb -= __builtin_popcount(q.c[touched[j]].b);
    pw -= q.c[touched[j]].h - __builtin_popcount(q.c[touched[j]].b);
  }

  q.c[c].h = 0;
  q.c[c].b = 0;
  // Bottom piece first, so the top piece lands last.
  for (int i = 0; i < src.h; i++) {
    Cell &x = q.c[w[i]];
    x.b |= ((src.b >> i) & 1u) << x.h;
    x.h++;
  }
  int capW = p.capW, capB = p.capB;
  for (int i = 0; i < src.h; i++) {
    Cell &x = q.c[w[i]];
    if (x.h >= B.collapse) {
      const int blacks = __builtin_popcount(x.b);
      if (p.side == 0) capW += blacks; else capB += x.h - blacks;
      x.h = 0;
      x.b = 0;
    }
  }
  for (int j = 0; j < nt; j++) {
    key ^= cellKey(touched[j], q.c[touched[j]]);
    account(ws, bs, q.c[touched[j]], +1);
    pb += __builtin_popcount(q.c[touched[j]].b);
    pw += q.c[touched[j]].h - __builtin_popcount(q.c[touched[j]].b);
  }

  for (int j = 0; j < nt; j++) setCellBits(q, touched[j]);
  q.side = p.side ^ 1;
  q.ply = p.ply + 1;
  q.capW = capW;
  q.capB = capB;
  q.ws = ws;
  q.bs = bs;
  q.pw = pw;
  q.pb = pb;
  q.key = key ^ kSideKey;
}

// ---- evaluation ------------------------------------------------------------

// White's margin in units of 1/kScale point: white's score minus black's, less komi.
inline int marginWhite(const Pos &p) {
  return (p.ws + p.capW - p.bs - p.capB) * kScale - B.komi;
}

// The evaluation of a position that goes on: the margin, plus a small term for the
// pieces each side has on the board, which predict later captures.
inline int evalWhite(const Pos &p) {
  int tallDiff = 0;
  for (int k = 0; k < B.words; k++) {
    tallDiff += __builtin_popcountll(p.ctrl[0][k] & p.tall[k]) - __builtin_popcountll(p.ctrl[1][k] & p.tall[k]);
  }
  return marginWhite(p) + kPieceWeight * (p.pw - p.pb) + kTallWeight * tallDiff;
}

// The same from the side to move's point of view.
inline int evalStm(const Pos &p) {
  const int e = evalWhite(p);
  return p.side == 0 ? e : -e;
}

// Score from the side to move's view once the game has ended. The winner is decided
// by the margin alone, and the sooner the game ends, the larger the win.
int terminalScore(const Pos &p) {
  const int e = marginWhite(p);
  const int v = kWin - p.ply;
  const bool whiteWins = e > 0;
  return (whiteWins == (p.side == 0)) ? v : -v;
}

// ---- transposition table ---------------------------------------------------

enum : uint8_t { kExact = 0, kLower = 1, kUpper = 2 };

struct TTEntry {
  uint64_t key;
  int32_t score;
  int16_t move;
  int8_t depth;
  uint8_t flag;
};

std::vector<TTEntry> tt;
uint64_t ttMask = 0;

void ttResize(int log2entries) {
  tt.assign((size_t)1 << log2entries, TTEntry{0, 0, -1, -1, kExact});
  ttMask = ((uint64_t)1 << log2entries) - 1;
}

void ttClear() { std::fill(tt.begin(), tt.end(), TTEntry{0, 0, -1, -1, kExact}); }

// ---- search ----------------------------------------------------------------

struct Search {
  uint64_t nodes = 0;
  uint64_t nodeLimit = 0;  // 0 = none
  bool timed = false;
  bool stop = false;
  bool allowStop = false;
  Clock::time_point deadline;
  int killer[kMaxPly][2];
  int history[kMaxMoves];
};
Search S;

inline void poll() {
  ++S.nodes;
  if (S.allowStop) {
    if (S.nodeLimit && S.nodes >= S.nodeLimit) S.stop = true;
    if (S.timed && (S.nodes & 511) == 0 && Clock::now() >= S.deadline) S.stop = true;
  }
}

int qsearch(const Pos &p, int alpha, int beta, int qd) {
  poll();
  if (S.stop) return 0;
  if (gameOver(p)) return terminalScore(p);
  const int stand = evalStm(p);
  if (stand >= beta) return stand;
  if (stand > alpha) alpha = stand;
  if (qd >= kMaxQ) return stand;
  // No cell can collapse, so no capture exists.
  bool anyHot = false;
  for (int k = 0; k < B.words; k++) anyHot = anyHot || p.hot[k];
  if (!anyHot) return stand;

  int moves[kMaxMoves];
  const int n = genMoves(p, moves, nullptr, true);
  int best = stand;
  Pos child;
  for (int i = 0; i < n; i++) {
    makeMove(p, moves[i], child);
    const int v = -qsearch(child, -beta, -alpha, qd + 1);
    if (S.stop) return 0;
    if (v > best) {
      best = v;
      if (v > alpha) {
        alpha = v;
        if (alpha >= beta) break;
      }
    }
  }
  return best;
}

int negamax(const Pos &p, int depth, int alpha, int beta, int ply) {
  poll();
  if (S.stop) return 0;
  if (gameOver(p)) return terminalScore(p);
  if (depth <= 0) return qsearch(p, alpha, beta, 0);

  const int alpha0 = alpha;
  int ttMove = -1;
  TTEntry &e = tt[p.key & ttMask];
  if (e.key == p.key && e.move >= 0) {
    ttMove = e.move;
    if (ply > 0 && e.depth >= depth) {
      if (e.flag == kExact) return e.score;
      if (e.flag == kLower && e.score >= beta) return e.score;
      if (e.flag == kUpper && e.score <= alpha) return e.score;
    }
  }

  // Each move is packed as (score << 20) | (tactical << 16) | move, so that ordering
  // needs one comparison and one swap per entry.
  int moves[kMaxMoves];
  bool tactical[kMaxMoves];
  const int n = genMoves(p, moves, tactical, false);
  const int k0 = ply < kMaxPly ? S.killer[ply][0] : -1;
  const int k1 = ply < kMaxPly ? S.killer[ply][1] : -1;
  uint64_t order[kMaxMoves];
  for (int i = 0; i < n; i++) {
    const int m = moves[i];
    uint64_t sc;
    if (m == ttMove) sc = 1ULL << 30;
    else if (tactical[i]) sc = 1ULL << 29;
    else if (m == k0) sc = 1ULL << 28;
    else if (m == k1) sc = (1ULL << 28) - 1;
    else sc = (uint64_t)S.history[m];
    order[i] = (sc << 20) | ((uint64_t)tactical[i] << 16) | (uint64_t)m;
  }

  int best = -kInf, bestMove = -1;
  Pos child;
  for (int i = 0; i < n; i++) {
    // Selection: bring the best remaining move to position i.
    int bi = i;
    for (int j = i + 1; j < n; j++) if (order[j] > order[bi]) bi = j;
    std::swap(order[i], order[bi]);
    const int m = (int)(order[i] & 0xFFFF);
    const bool tacticalMove = (order[i] >> 16) & 1;

    makeMove(p, m, child);
    int v;
    if (i == 0) {
      v = -negamax(child, depth - 1, -beta, -alpha, ply + 1);
    } else {
      // Late quiet moves are searched at reduced depth first; anything that beats
      // alpha is re-searched at full depth. Other moves get a null window.
      int reduce = 0;
      if (depth >= 3 && !tacticalMove && m != k0 && m != k1) {
        reduce = i >= kLmrSecond ? 2 : i >= kLmrFirst ? 1 : 0;
        reduce = std::min(reduce, depth - 2);
      }
      v = -negamax(child, depth - 1 - reduce, -alpha - 1, -alpha, ply + 1);
      if (v > alpha && (reduce || v < beta)) v = -negamax(child, depth - 1, -beta, -alpha, ply + 1);
    }
    if (S.stop) return 0;
    if (v > best) {
      best = v;
      bestMove = m;
      if (v > alpha) {
        alpha = v;
        if (alpha >= beta) {
          if (!tacticalMove) {
            if (ply < kMaxPly && m != k0) {
              S.killer[ply][1] = S.killer[ply][0];
              S.killer[ply][0] = m;
            }
            S.history[m] += depth * depth;
          }
          break;
        }
      }
    }
  }

  e.key = p.key;
  e.score = best;
  e.move = (int16_t)bestMove;
  e.depth = (int8_t)depth;
  e.flag = best <= alpha0 ? kUpper : best >= beta ? kLower : kExact;
  return best;
}

struct RootMove {
  int m;
  int score;
};

// Searches the root moves to the given depth within (alpha, beta), reordering them by
// score. Stops early on a fail high, leaving the rest with their old scores. Returns
// false if the search was stopped before it finished.
bool searchRoot(const Pos &root, int depth, std::vector<RootMove> &moves, int alpha, int beta) {
  for (RootMove &rm : moves) {
    Pos child;
    makeMove(root, rm.m, child);
    const int v = -negamax(child, depth - 1, -beta, -alpha, 1);
    if (S.stop) return false;
    rm.score = v;
    if (v > alpha) alpha = v;
    if (alpha >= beta) break;
  }
  std::stable_sort(moves.begin(), moves.end(),
                   [](const RootMove &a, const RootMove &b) { return a.score > b.score; });
  return true;
}

// Chooses a move for the position, searching for at most budgetMs (if positive), the
// node limit, or up to maxDepth. Depth 1 always completes. From depth 4 each iteration
// searches a narrow window around the previous score, widening it when the score falls
// outside the window.
int chooseMove(const Pos &root, int maxDepth, double budgetMs) {
  int moves[kMaxMoves];
  const int n = genMoves(root, moves, nullptr, false);
  if (n == 0) return -1;
  if (n == 1) return moves[0];
  std::vector<RootMove> rootMoves;
  for (int i = 0; i < n; i++) rootMoves.push_back({moves[i], 0});

  S.nodes = 0;
  S.stop = false;
  S.timed = budgetMs > 0;
  S.deadline = Clock::now() + std::chrono::microseconds((long long)(budgetMs * 1000));
  int best = rootMoves[0].m;
  int prev = 0;
  for (int depth = 1; depth <= maxDepth && depth < kMaxPly; depth++) {
    S.allowStop = depth > 1;
    int delta = 40;
    int alpha = -kInf, beta = kInf;
    if (depth >= 4) {
      alpha = prev - delta;
      beta = prev + delta;
    }
    const std::vector<RootMove> saved = rootMoves;
    bool done = false;
    for (;;) {
      if (!searchRoot(root, depth, rootMoves, alpha, beta)) break;
      const int score = rootMoves[0].score;
      if (alpha > -kInf && score <= alpha) {
        alpha = prev - (delta *= 4) < -kInf / 2 ? -kInf : prev - delta;
      } else if (beta < kInf && score >= beta) {
        beta = prev + (delta *= 4) > kInf / 2 ? kInf : prev + delta;
      } else {
        done = true;
        break;
      }
      rootMoves = saved;
    }
    if (!done) break;
    best = rootMoves[0].m;
    prev = rootMoves[0].score;
    std::printf("info depth %d score %d nodes %llu\n", depth, prev, (unsigned long long)S.nodes);
    std::fflush(stdout);
  }
  return best;
}

// ---- protocol --------------------------------------------------------------

std::string moveName(int m) { return B.name[m / 6] + kDirName[m % 6]; }

std::vector<std::string> split(const std::string &text, char sep) {
  std::vector<std::string> out(1);
  for (char ch : text) {
    if (ch == sep) out.emplace_back();
    else out.back() += ch;
  }
  return out;
}

// Parses a position in Cascade State Notation (RULES.md section 7).
bool parseCSN(const std::string &text, Pos &p) {
  std::istringstream is(text);
  std::string board, side;
  int ply = 0, capW = 0, capB = 0;
  if (!(is >> board >> side >> ply >> capW >> capB)) return false;
  const std::vector<std::string> rows = split(board, '/');
  if ((int)rows.size() != B.rows) return false;
  std::memset(&p, 0, sizeof p);
  for (int r = 0; r < B.rows; r++) {
    const std::vector<std::string> cells = split(rows[r], ',');
    if ((int)cells.size() != B.rowLen[r]) return false;
    for (int k = 0; k < B.rowLen[r]; k++) {
      const int c = B.rowStart[r] + k;
      const std::string &cell = cells[k];
      if (cell == "-") continue;
      if ((int)cell.size() >= B.collapse) return false;
      for (int i = 0; i < (int)cell.size(); i++) {
        if (cell[i] != 'w' && cell[i] != 'b') return false;
        if (cell[i] == 'b') p.c[c].b |= 1u << i;
      }
      p.c[c].h = (uint8_t)cell.size();
    }
  }
  p.side = side == "b" ? 1 : 0;
  p.ply = ply;
  p.capW = capW;
  p.capB = capB;
  finishPos(p);
  return true;
}

// Sets the rules from key=value options (`newgame`). Unknown keys are ignored.
void parseNewGame(const std::vector<std::string> &args) {
  int side = 5, collapse = 6, maxply = 150;
  double komi = 0.5;
  for (const std::string &a : args) {
    const size_t eq = a.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = a.substr(0, eq);
    const double v = std::atof(a.substr(eq + 1).c_str());
    if (k == "side") side = (int)v;
    else if (k == "collapse") collapse = (int)v;
    else if (k == "maxply") maxply = (int)v;
    else if (k == "komi") komi = v;
  }
  setupBoard(side, collapse, maxply, komi);
  ttClear();
  std::memset(S.killer, 0xff, sizeof S.killer);
  std::memset(S.history, 0, sizeof S.history);
}

std::vector<std::string> tokens(const std::string &line) {
  std::istringstream is(line);
  std::vector<std::string> out;
  std::string tok;
  while (is >> tok) out.push_back(tok);
  return out;
}

}  // namespace

#ifndef CASCADE_LIB
int main() {
  setupBoard(5, 6, 150, 0.5);
  ttResize(22);
  std::memset(S.killer, 0xff, sizeof S.killer);
  Pos game = initialPos(0);

  std::string line;
  while (std::getline(std::cin, line)) {
    const std::vector<std::string> tok = tokens(line);
    if (tok.empty()) continue;
    const std::string &cmd = tok[0];
    if (cmd == "cascade") {
      std::printf("name cascade-ab\nready\n");
      std::fflush(stdout);
    } else if (cmd == "newgame") {
      parseNewGame(std::vector<std::string>(tok.begin() + 1, tok.end()));
      game = initialPos(0);
    } else if (cmd == "position") {
      Pos p;
      if (parseCSN(line.substr(line.find("position") + 8), p)) game = p;
    } else if (cmd == "go") {
      int maxDepth = 64;
      double budgetMs = 0;
      long long nodes = 0;
      for (size_t i = 1; i + 1 < tok.size(); i += 2) {
        if (tok[i] == "movetime") budgetMs = std::atof(tok[i + 1].c_str()) * kBudgetFraction;
        else if (tok[i] == "depth") maxDepth = std::atoi(tok[i + 1].c_str());
        else if (tok[i] == "nodes") nodes = std::atoll(tok[i + 1].c_str());
      }
      if (budgetMs <= 0 && nodes == 0 && maxDepth == 64) budgetMs = 250 * kBudgetFraction;
      S.nodeLimit = (uint64_t)nodes;
      const int m = chooseMove(game, maxDepth, budgetMs);
      std::printf("bestmove %s\n", m < 0 ? "a1E" : moveName(m).c_str());
      std::fflush(stdout);
    } else if (cmd == "quit") {
      break;
    }
  }
  return 0;
}
#endif
