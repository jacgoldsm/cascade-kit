// Cascade engine: iterative-deepening alpha-beta (principal variation search) with a
// transposition table, quiescence search over collapses, late-move reduction and
// one-ply move ordering. The rules follow RULES.md and cascade/rules.js. The board is
// rebuilt from CSN on every `position` command, so the engine keeps no game history.
//
// `engine perft N [CSN]` counts move sequences for checking the rules (RULES.md §8).
// `engine check` reads CSN positions and checks the incremental evaluation and
// collapse detection against a full move application.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int MAXN = 469;     // cells on the largest board (side 13)
constexpr int MAXH = 12;      // sow length: collapse - 1 is at most 11
constexpr int MAXMOVES = MAXN * 6;
constexpr int MAXPLY = 160;   // deepest search, in plies from the root
constexpr int WIN = 1000000;  // score of a proven win now; a win in k plies is WIN - k
constexpr int INF = 2000000;
constexpr int TT_BITS = 20;
constexpr uint64_t TT_MASK = (1ULL << TT_BITS) - 1;

// Static evaluation of a position that is not over, in doubled-margin units (a stack or
// a captured piece is worth 2 here, as in the exact margin). Pieces still on the board
// can yet become stacks or captures, so each net white piece counts MAT_W, and each
// controlled stack counts STACK_W. Finished games are scored by the exact margin.
// Both weights were chosen by self-play matches.
constexpr int MAT_W = 2;
constexpr int STACK_W = 1;

const char* const DIR_NAMES[6] = {"E", "NE", "NW", "W", "SW", "SE"};
const int DIR_DQ[6] = {1, 1, 0, -1, -1, 0};
const int DIR_DR[6] = {0, -1, -1, 0, 1, 1};

// ---- geometry -----------------------------------------------------------------

struct Geometry {
  int side = 5, N = 0, maxH = 4, collapse = 6, maxply = 150, komi2 = 1;
  std::string cellName[MAXN];
  int nb[MAXN][6];                 // neighbour in each direction, or -1 off the board
  int antipode[MAXN];
  int16_t walk[MAXN][6][MAXH];     // cell receiving the i-th sown piece, per start and direction
};

Geometry G;

// Builds the board for the given rules. komi2 is twice the komi (1 for komi 0.5).
void buildGeometry(int side, int collapse, int maxply, int komi2) {
  G.side = side;
  G.collapse = collapse;
  G.maxH = collapse - 1;
  G.maxply = maxply;
  G.komi2 = komi2;
  const int R = side - 1;
  static int grid[25][25];
  for (auto& row : grid) std::fill(std::begin(row), std::end(row), -1);
  int cq[MAXN], cr[MAXN];
  int n = 0;
  for (int r = -R; r <= R; r++) {
    const int qmin = std::max(-R, -R - r), qmax = std::min(R, R - r);
    for (int q = qmin; q <= qmax; q++) {
      grid[q + R][r + R] = n;
      cq[n] = q;
      cr[n] = r;
      G.cellName[n] = std::string(1, char('a' + r + R)) + std::to_string(q - qmin + 1);
      n++;
    }
  }
  G.N = n;
  for (int c = 0; c < n; c++) {
    for (int d = 0; d < 6; d++) {
      const int q = cq[c] + DIR_DQ[d], r = cr[c] + DIR_DR[d];
      const bool on = std::abs(q) <= R && std::abs(r) <= R && std::abs(q + r) <= R;
      G.nb[c][d] = on ? grid[q + R][r + R] : -1;
    }
    G.antipode[c] = grid[-cq[c] + R][-cr[c] + R];
  }
  // Sowing walk, as in cascade/rules.js: reverse direction whenever the next step is off the board.
  for (int c = 0; c < n; c++) {
    for (int d0 = 0; d0 < 6; d0++) {
      for (int i = 0; i < MAXH; i++) G.walk[c][d0][i] = -1;
      if (G.nb[c][d0] < 0) continue;
      int pos = c, d = d0;
      for (int i = 0; i < G.maxH; i++) {
        if (G.nb[pos][d] < 0) d = (d + 3) % 6;
        pos = G.nb[pos][d];
        G.walk[c][d0][i] = int16_t(pos);
      }
    }
  }
}

// ---- positions ----------------------------------------------------------------

struct Board {
  uint8_t h[MAXN];       // stack height
  uint16_t bits[MAXN];   // bit i is 1 when the i-th piece from the bottom is black
  uint8_t side;          // side to move: 0 white, 1 black
  uint8_t nStacks[2];    // stacks controlled by white, by black
  int16_t mat;           // white pieces on the board minus black pieces
  uint16_t ply;
  uint16_t cap[2];       // pieces captured by white, by black
  uint64_t hash;
};

inline void copyBoard(Board& dst, const Board& src) {
  std::memcpy(dst.h, src.h, G.N);
  std::memcpy(dst.bits, src.bits, sizeof(uint16_t) * G.N);
  dst.side = src.side;
  dst.nStacks[0] = src.nStacks[0];
  dst.nStacks[1] = src.nStacks[1];
  dst.mat = src.mat;
  dst.ply = src.ply;
  dst.cap[0] = src.cap[0];
  dst.cap[1] = src.cap[1];
  dst.hash = src.hash;
}

inline uint64_t mix64(uint64_t z) {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}
inline uint64_t cellKey(int c, int h, int bits) {
  return mix64(((uint64_t(c) * 64 + uint64_t(h)) << 16) + uint64_t(bits) + 0x51ed27ULL);
}
const uint64_t SIDE_KEY = mix64(0xdeadbeefcafeULL);

// Colour of the top piece of cell c: 0 white, 1 black. Only meaningful for h > 0.
inline int topColour(const Board& s, int c) { return (s.bits[c] >> (s.h[c] - 1)) & 1; }

// White pieces minus black pieces in a stack of height h with black pieces given by bits.
inline int matOf(int h, int bits) { return h - 2 * __builtin_popcount(bits); }

inline void setCell(Board& s, int c, int nh, int nbits) {
  s.hash ^= cellKey(c, s.h[c], s.bits[c]) ^ cellKey(c, nh, nbits);
  s.mat = int16_t(s.mat - matOf(s.h[c], s.bits[c]) + matOf(nh, nbits));
  if (s.h[c]) s.nStacks[topColour(s, c)]--;
  s.h[c] = uint8_t(nh);
  s.bits[c] = uint16_t(nbits);
  if (nh) s.nStacks[topColour(s, c)]++;
}

uint64_t fullHash(const Board& s) {
  uint64_t x = s.side ? SIDE_KEY : 0;
  for (int c = 0; c < G.N; c++) x ^= cellKey(c, s.h[c], s.bits[c]);
  return x;
}

// Parses a CSN position (RULES.md §7). Returns false if it is malformed.
bool parsePosition(const std::string& csn, Board& s) {
  size_t i = 0;
  const size_t len = csn.size();
  int c = 0;
  for (;;) {
    if (c >= G.N) return false;
    int hh = 0;
    uint16_t bb = 0;
    if (i < len && csn[i] == '-') {
      i++;
    } else {
      while (i < len && (csn[i] == 'w' || csn[i] == 'b')) {
        if (csn[i] == 'b') bb |= uint16_t(1u << hh);
        hh++;
        i++;
      }
    }
    if (hh >= 16) return false;
    s.h[c] = uint8_t(hh);
    s.bits[c] = bb;
    c++;
    if (i >= len) return false;
    if (csn[i] == ',' || csn[i] == '/') {
      i++;
      continue;
    }
    if (csn[i] == ' ') {
      i++;
      break;
    }
    return false;
  }
  if (c != G.N) return false;
  std::istringstream rest(csn.substr(i));
  std::string sideTok;
  int ply = 0, capW = 0, capB = 0;
  if (!(rest >> sideTok >> ply >> capW >> capB)) return false;
  s.side = sideTok == "b" ? 1 : 0;
  s.ply = uint16_t(ply);
  s.cap[0] = uint16_t(capW);
  s.cap[1] = uint16_t(capB);
  s.nStacks[0] = s.nStacks[1] = 0;
  s.mat = 0;
  for (int k = 0; k < G.N; k++) {
    if (s.h[k]) s.nStacks[topColour(s, k)]++;
    s.mat = int16_t(s.mat + matOf(s.h[k], s.bits[k]));
  }
  s.hash = fullHash(s);
  return true;
}

// Moves are cell * 6 + direction. Returns the number of legal moves written to out.
int genMoves(const Board& s, int* out) {
  int n = 0;
  for (int c = 0; c < G.N; c++) {
    if (!s.h[c] || topColour(s, c) != s.side) continue;
    for (int d = 0; d < 6; d++)
      if (G.nb[c][d] >= 0) out[n++] = c * 6 + d;
  }
  return n;
}

// True if moving m would make some stack reach the collapse height. Uses the heights
// before the move, counting how often the sow visits each cell.
bool collapses(const Board& s, int m) {
  const int c = m / 6, d = m % 6;
  const int hgt = s.h[c];
  const int16_t* w = G.walk[c][d];
  // Cheap reject: no cell on the path is tall enough, even counting every sown piece.
  int tallest = 0;
  for (int i = 0; i < hgt; i++) {
    const int t = w[i];
    const int base = t == c ? 0 : s.h[t];
    if (base > tallest) tallest = base;
  }
  if (tallest + hgt < G.collapse) return false;
  for (int i = 0; i < hgt; i++) {
    const int t = w[i];
    int hits = 0;
    for (int j = 0; j < hgt; j++) hits += w[j] == t;
    const int base = t == c ? 0 : s.h[t];
    if (base + hits >= G.collapse) return true;
  }
  return false;
}

// Applies a legal move in place (RULES.md §4). Returns the number of stacks collapsed.
int applyMove(Board& s, int m) {
  const int c = m / 6, d = m % 6;
  const int hgt = s.h[c];
  const uint32_t bits = s.bits[c];
  const int mover = s.side;
  const int16_t* w = G.walk[c][d];
  setCell(s, c, 0, 0);
  // Sow bottom piece first; the mover's top piece lands last, on the farthest cell.
  for (int i = 0; i < hgt; i++) {
    const int t = w[i];
    const int th = s.h[t];
    const int nbits = s.bits[t] | int(((bits >> i) & 1u) << th);
    setCell(s, t, th + 1, nbits);
  }
  // Collapse: each stack at the collapse height is removed; the mover captures the
  // opponent's pieces in it.
  int removed = 0;
  for (int i = 0; i < hgt; i++) {
    const int t = w[i];
    const int th = s.h[t];
    if (th >= G.collapse) {
      const int blacks = __builtin_popcount(s.bits[t]);
      s.cap[mover] = uint16_t(s.cap[mover] + (mover == 0 ? blacks : th - blacks));
      setCell(s, t, 0, 0);
      removed++;
    }
  }
  s.side = uint8_t(mover ^ 1);
  s.hash ^= SIDE_KEY;
  s.ply++;
  return removed;
}

// ---- evaluation ---------------------------------------------------------------

// Twice (white score - black score - komi). Komi is x.5, so this is never zero.
// This is the exact result, used when the game is over.
int marginWhite2(const Board& s) {
  return 2 * (s.nStacks[0] + s.cap[0] - s.nStacks[1] - s.cap[1]) - G.komi2;
}

// Heuristic value for a position that is not over: the exact margin, with the stack
// and material terms re-weighted as described above.
int heuristicWhite2(int margin2, int stackDiff, int mat) {
  return margin2 + (STACK_W - 2) * stackDiff + MAT_W * mat;
}

// Static value for the side to move.
int evalMover(const Board& s) {
  const int m = heuristicWhite2(marginWhite2(s), s.nStacks[0] - s.nStacks[1], s.mat);
  return s.side == 0 ? m : -m;
}

bool isTerminal(const Board& s) { return s.ply >= G.maxply || s.nStacks[s.side] == 0; }

// The game is decided by the sign of the margin, never by a draw (komi x.5).
int terminalValue(const Board& s, int depth) {
  const int v = s.side == 0 ? marginWhite2(s) : -marginWhite2(s);
  return v > 0 ? WIN - depth : -(WIN - depth);
}

// The mover's static value after move m, from the mover's side, without changing s.
// Equal to -evalMover(child) for the child that applyMove(child, m) would give.
// Sets terminal when the game ends after the move. Used to order and prune moves
// before paying for a full copy and apply.
int quickAfter(const Board& s, int m, bool& terminal) {
  const int c = m / 6, d = m % 6;
  const int hgt = s.h[c];
  const uint32_t bits = s.bits[c];
  const int mover = s.side;
  const int16_t* w = G.walk[c][d];
  // The cells the sow touches, with their simulated heights and contents.
  int ids[MAXH + 1];
  int hs[MAXH + 1];
  uint32_t bs[MAXH + 1];
  int n = 0;
  auto slot = [&](int t) {
    for (int k = 0; k < n; k++)
      if (ids[k] == t) return k;
    ids[n] = t;
    hs[n] = s.h[t];
    bs[n] = s.bits[t];
    return n++;
  };
  hs[slot(c)] = 0;
  bs[slot(c)] = 0;
  for (int i = 0; i < hgt; i++) {
    const int k = slot(w[i]);
    bs[k] |= ((bits >> i) & 1u) << hs[k];
    hs[k]++;
  }
  int capGain = 0;
  for (int i = 0; i < hgt; i++) {
    const int k = slot(w[i]);
    if (hs[k] >= G.collapse) {
      const int blacks = __builtin_popcount(bs[k]);
      capGain += mover == 0 ? blacks : hs[k] - blacks;
      hs[k] = 0;
      bs[k] = 0;
    }
  }
  int nS[2] = {s.nStacks[0], s.nStacks[1]};
  int mat = s.mat;
  for (int k = 0; k < n; k++) {
    const int t = ids[k];
    if (s.h[t]) nS[(s.bits[t] >> (s.h[t] - 1)) & 1]--;
    if (hs[k]) nS[(bs[k] >> (hs[k] - 1)) & 1]++;
    mat += matOf(hs[k], bs[k]) - matOf(s.h[t], s.bits[t]);
  }
  const int capW = s.cap[0] + (mover == 0 ? capGain : 0);
  const int capB = s.cap[1] + (mover == 1 ? capGain : 0);
  terminal = s.ply + 1 >= G.maxply || nS[mover ^ 1] == 0;
  const int m2 = 2 * (nS[0] + capW - nS[1] - capB) - G.komi2;
  const int v = heuristicWhite2(m2, nS[0] - nS[1], mat);
  return mover == 0 ? v : -v;
}

// ---- search -------------------------------------------------------------------

struct TTEntry {
  uint64_t key;
  int32_t score;
  int16_t move;
  int8_t depth;
  uint8_t flag;
};
enum { EXACT = 0, LOWER = 1, UPPER = 2 };

std::vector<TTEntry> tt(1ULL << TT_BITS);

using Clock = std::chrono::steady_clock;

struct Searcher {
  Board boards[MAXPLY + 2];
  int moves[MAXPLY + 2][MAXMOVES];
  int scores[MAXPLY + 2][MAXMOVES];
  uint64_t nodes = 0;
  uint64_t nodeLimit = UINT64_MAX;
  bool stopped = false;
  bool useTime = true;
  Clock::time_point deadline;

  void checkTime() {
    if (nodes >= nodeLimit || (useTime && Clock::now() >= deadline)) stopped = true;
  }

  // Quiescence: at the horizon, keep searching moves that collapse a stack, so that
  // the static evaluation is not taken in the middle of a capture sequence.
  int qsearch(int alpha, int beta, int ply) {
    Board& s = boards[ply];
    if ((++nodes & 1023) == 0) checkTime();
    if (stopped) return 0;
    if (isTerminal(s)) return terminalValue(s, ply);
    const int stand = evalMover(s);
    if (stand >= beta || ply >= MAXPLY) return stand;
    if (stand > alpha) alpha = stand;

    Board& child = boards[ply + 1];
    int best = stand;
    for (int c = 0; c < G.N; c++) {
      if (!s.h[c] || topColour(s, c) != s.side) continue;
      for (int d = 0; d < 6; d++) {
        const int m = c * 6 + d;
        if (G.nb[c][d] < 0 || !collapses(s, m)) continue;
        copyBoard(child, s);
        applyMove(child, m);
        const int v = -qsearch(-beta, -alpha, ply + 1);
        if (stopped) return 0;
        if (v > best) {
          best = v;
          if (v > alpha) {
            alpha = v;
            if (alpha >= beta) return best;
          }
        }
      }
    }
    return best;
  }

  int negamax(int depth, int alpha, int beta, int ply) {
    Board& s = boards[ply];
    if ((++nodes & 1023) == 0) checkTime();
    if (stopped) return 0;
    if (isTerminal(s)) return terminalValue(s, ply);
    if (depth <= 0) return qsearch(alpha, beta, ply);

    const uint64_t key = s.hash;
    TTEntry& e = tt[key & TT_MASK];
    int ttMove = -1;
    if (e.key == key) {
      ttMove = e.move;
      if (e.depth >= depth) {
        if (e.flag == EXACT) return e.score;
        if (e.flag == LOWER && e.score >= beta) return e.score;
        if (e.flag == UPPER && e.score <= alpha) return e.score;
      }
    }

    int* mv = moves[ply];
    int* sc = scores[ply];
    const int n = genMoves(s, mv);
    Board& child = boards[ply + 1];
    const int alphaOrig = alpha;
    int best = -INF, bestMove = -1;

    if (depth == 1) {
      // Children are horizon nodes: quiescence search them directly, without ordering.
      // A child whose static value is already at or below alpha needs no full apply:
      // its quiescence search would stand pat there anyway.
      for (int i = 0; i < n; i++) {
        bool term = false;
        const int quick = quickAfter(s, mv[i], term);
        if (!term && quick <= alpha) {
          // The child's quiescence search would stand pat at this value, so it is exact enough.
          if (quick > best) {
            best = quick;
            bestMove = mv[i];
          }
          continue;
        }
        copyBoard(child, s);
        applyMove(child, mv[i]);
        const int v = -qsearch(-beta, -alpha, ply + 1);
        if (stopped) return 0;
        if (v > best) {
          best = v;
          bestMove = mv[i];
          if (v > alpha) {
            alpha = v;
            if (alpha >= beta) break;
          }
        }
      }
    } else {
      for (int i = 0; i < n; i++) {
        bool term = false;
        sc[i] = mv[i] == ttMove ? INF : quickAfter(s, mv[i], term);
      }
      sortMoves(mv, sc, n);
      for (int i = 0; i < n; i++) {
        copyBoard(child, s);
        applyMove(child, mv[i]);
        int v;
        if (i == 0) {
          v = -negamax(depth - 1, -beta, -alpha, ply + 1);
        } else {
          // Late quiet moves are searched shallower first; they are re-searched in full
          // only if they beat alpha.
          int r = 0;
          if (depth >= 3 && i >= 4 && !collapses(s, mv[i])) r = i >= 10 ? 2 : 1;
          v = -negamax(depth - 1 - r, -alpha - 1, -alpha, ply + 1);
          if (!stopped && r > 0 && v > alpha) v = -negamax(depth - 1, -alpha - 1, -alpha, ply + 1);
          if (!stopped && v > alpha && v < beta) v = -negamax(depth - 1, -beta, -alpha, ply + 1);
        }
        if (stopped) return 0;
        if (v > best) {
          best = v;
          bestMove = mv[i];
          if (v > alpha) {
            alpha = v;
            if (alpha >= beta) break;
          }
        }
      }
    }
    if (stopped) return 0;
    const uint8_t flag = best <= alphaOrig ? UPPER : (best >= beta ? LOWER : EXACT);
    e.key = key;
    e.score = best;
    e.move = int16_t(bestMove);
    e.depth = int8_t(depth);
    e.flag = flag;
    return best;
  }

  // Puts the best moves first, sorted by score; the rest keep an arbitrary order. Late
  // moves are searched at reduced depth, so only the front of the list needs ordering.
  // Score and move are packed into one key.
  static void sortMoves(int* mv, int* sc, int n) {
    constexpr int SORTED = 12;
    int64_t key[MAXMOVES];
    for (int i = 0; i < n; i++) key[i] = int64_t(sc[i]) * 65536 + mv[i];
    const int k = std::min(n, SORTED);
    std::partial_sort(key, key + k, key + n, std::greater<int64_t>());
    for (int i = 0; i < n; i++) {
      mv[i] = int(key[i] & 0xffff);
      sc[i] = int(key[i] >> 16);
    }
  }
};

Searcher searcher;

// Searches the root position to the given depth within the window (alpha, beta). Returns
// the best move, or -1 if the search was stopped before it finished. If the value lies
// outside the window the caller must search again.
int searchRoot(const Board& root, int depth, int alpha, int beta, int& value) {
  Searcher& S = searcher;
  copyBoard(S.boards[0], root);
  Board& s = S.boards[0];
  int* mv = S.moves[0];
  int* sc = S.scores[0];
  const int n = genMoves(s, mv);
  Board& child = S.boards[1];
  int best = -INF, bestMove = -1;
  for (int i = 0; i < n; i++) {
    copyBoard(child, s);
    applyMove(child, mv[i]);
    int v;
    if (i == 0) {
      v = -S.negamax(depth - 1, -beta, -alpha, 1);
    } else {
      v = -S.negamax(depth - 1, -alpha - 1, -alpha, 1);
      if (!S.stopped && v > alpha && v < beta) v = -S.negamax(depth - 1, -beta, -alpha, 1);
    }
    if (S.stopped) return -1;
    sc[i] = v;
    if (v > best) {
      best = v;
      bestMove = mv[i];
      if (v > alpha) alpha = v;
      if (alpha >= beta) break;  // fails high: the caller searches again
    }
  }
  // Order the next iteration by this one's scores.
  Searcher::sortMoves(mv, sc, n);
  value = best;
  return bestMove;
}

// ---- protocol -----------------------------------------------------------------

struct Options {
  int side = 5, collapse = 6, maxply = 150, komi2 = 1;
};

std::string moveName(int m) { return G.cellName[m / 6] + DIR_NAMES[m % 6]; }

void reply(const std::string& line) {
  std::cout << line << '\n' << std::flush;
}

void tryParseNewgame(const std::vector<std::string>& args) {
  Options o;
  for (const auto& tok : args) {
    const auto eq = tok.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = tok.substr(0, eq);
    const double v = std::atof(tok.c_str() + eq + 1);
    if (k == "side") o.side = int(v);
    else if (k == "collapse") o.collapse = int(v);
    else if (k == "maxply") o.maxply = int(v);
    else if (k == "komi") o.komi2 = int(v * 2 + (v < 0 ? -0.5 : 0.5));
  }
  if (o.side < 3 || o.side > 13) o.side = 5;
  if (o.collapse < 3 || o.collapse > 12) o.collapse = 6;
  buildGeometry(o.side, o.collapse, o.maxply, o.komi2);
  std::fill(tt.begin(), tt.end(), TTEntry{0, 0, -1, 0, 0});
}

// Think for the given limits and return the chosen move.
int think(const Board& root, double movetimeMs, int maxDepth, uint64_t maxNodes) {
  Searcher& S = searcher;
  S.nodes = 0;
  S.stopped = false;
  S.nodeLimit = maxNodes ? maxNodes : UINT64_MAX;
  S.useTime = movetimeMs > 0;
  const Clock::time_point started = Clock::now();
  S.deadline = started + std::chrono::microseconds(int64_t(movetimeMs * 1000));

  Board& s = S.boards[0];
  copyBoard(s, root);
  int* mv = S.moves[0];
  const int n = genMoves(s, mv);
  if (n == 0) return -1;
  int bestMove = mv[0];
  if (n == 1) return bestMove;

  // From depth 4 each iteration first searches a window around the last score; a result
  // outside it is searched again with the full window.
  constexpr int ASPIRATION = 24;
  int prev = 0;
  for (int depth = 1; depth <= MAXPLY - 2 && (maxDepth == 0 || depth <= maxDepth); depth++) {
    int value = 0;
    const bool window = depth >= 4;
    int m = searchRoot(root, depth, window ? prev - ASPIRATION : -INF, window ? prev + ASPIRATION : INF, value);
    if (m >= 0 && window && (value <= prev - ASPIRATION || value >= prev + ASPIRATION)) {
      m = searchRoot(root, depth, -INF, INF, value);
    }
    if (m < 0) break;  // stopped: keep the previous iteration's move
    bestMove = m;
    prev = value;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    reply("info depth " + std::to_string(depth) + " score " + std::to_string(value) +
          " nodes " + std::to_string(S.nodes) + " ms " + std::to_string(int(ms)) +
          " bestmove " + moveName(m));
    if (value >= WIN - MAXPLY || value <= -(WIN - MAXPLY)) break;  // result is forced
  }
  return bestMove;
}

// Counts move sequences of the given length (perft, RULES.md §8).
uint64_t perft(const Board& s, int depth, int ply) {
  if (depth == 0 || isTerminal(s)) return depth == 0 ? 1 : 0;
  int* mv = searcher.moves[ply];
  const int n = genMoves(s, mv);
  if (depth == 1) return uint64_t(n);
  Board& child = searcher.boards[ply + 1];
  uint64_t total = 0;
  for (int i = 0; i < n; i++) {
    copyBoard(child, s);
    applyMove(child, mv[i]);
    total += perft(child, depth - 1, ply + 1);
  }
  return total;
}

}  // namespace

int main(int argc, char** argv) {
  buildGeometry(5, 6, 150, 1);
  Board root{};

  if (argc >= 3 && std::string(argv[1]) == "perft") {
    const std::string csn = argc >= 4 ? argv[3]
                                      : "w,w,b,b,w/w,w,b,b,w,b/b,w,w,b,w,b,w/w,w,b,b,b,b,w,b/b,w,w,w,-,b,b,b,w/"
                                        "w,b,w,w,w,w,b,b/b,w,b,w,b,b,w/w,b,w,w,b,b/b,w,w,b,b w 0 0 0";
    if (!parsePosition(csn, root)) {
      std::fprintf(stderr, "bad position\n");
      return 1;
    }
    std::printf("%llu\n", static_cast<unsigned long long>(perft(root, std::atoi(argv[2]), 0)));
    return 0;
  }

  if (argc >= 2 && std::string(argv[1]) == "check") {
    // Self-test: quickAfter and collapses must agree with a full apply, move by move.
    Board s{};
    std::string csn;
    long long moves = 0, bad = 0;
    while (std::getline(std::cin, csn)) {
      if (!parsePosition(csn, s)) continue;
      int* mv = searcher.moves[0];
      const int n = genMoves(s, mv);
      for (int i = 0; i < n; i++) {
        Board child{};
        copyBoard(child, s);
        const int removed = applyMove(child, mv[i]);
        bool term = false;
        const int quick = quickAfter(s, mv[i], term);
        const bool ok = quick == -evalMover(child) && term == isTerminal(child) &&
                        collapses(s, mv[i]) == (removed > 0) && child.hash == fullHash(child);
        bad += !ok;
        moves++;
      }
    }
    std::printf("moves %lld mismatches %lld\n", moves, bad);
    return bad ? 1 : 0;
  }

  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "cascade") {
      reply("name cascade-search");
      reply("ready");
    } else if (cmd == "newgame") {
      std::vector<std::string> args;
      for (std::string tok; in >> tok;) args.push_back(tok);
      tryParseNewgame(args);
    } else if (cmd == "position") {
      std::string rest;
      std::getline(in, rest);
      const size_t start = rest.find_first_not_of(" \t");
      if (start == std::string::npos || !parsePosition(rest.substr(start), root)) {
        reply("info error bad position");
      }
    } else if (cmd == "go") {
      double movetime = 0;
      int depth = 0;
      uint64_t nodes = 0;
      for (std::string key, val; in >> key >> val;) {
        if (key == "movetime") movetime = std::atof(val.c_str());
        else if (key == "depth") depth = std::atoi(val.c_str());
        else if (key == "nodes") nodes = std::strtoull(val.c_str(), nullptr, 10);
      }
      // Use most of the allowance; the referee's clock also covers process overhead.
      if (movetime > 0) movetime = movetime * 0.94;
      if (movetime <= 0 && depth == 0 && nodes == 0) movetime = 250 * 0.94;
      const int m = think(root, movetime, depth, nodes);
      reply(m < 0 ? "bestmove 0" : "bestmove " + moveName(m));
    } else if (cmd == "quit") {
      break;
    }
  }
  return 0;
}
