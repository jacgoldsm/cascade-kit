// Cascade engine (see RULES.md and PROTOCOL.md).
//
// Principal-variation alpha-beta search with iterative deepening, a transposition
// table, killer and history ordering, late-move reductions, and a quiescence search
// over collapsing moves. The evaluation is the score margin: stacks controlled plus
// captures, the difference between the sides, less komi.

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
constexpr int kMaxPly = 256;
constexpr int kMaxQ = 6;        // quiescence depth limit, in plies
constexpr int kScale = 8;       // evaluation units per point
constexpr int kWin = 1000000;
constexpr int kInf = 1 << 29;

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
  int side;  // 0 = white to move, 1 = black
  int ply;
  int capW, capB;  // pieces captured by each side
  int ws, bs;      // stacks controlled by each side
  uint64_t key;
};

// The rules in use, set by `newgame`. Everything is derived from side and collapse.
struct Board {
  int side = 5, collapse = 6, maxply = 150, maxH = 5, N = 0, komi = 4;
  int rowStart[32], rowLen[32], rows = 0;
  int nb[kMaxMoves];              // nb[c * 6 + d]: neighbour, or -1 off the board
  int antipode[kMaxCells];
  std::vector<int> walk;          // walk[m * maxH + i]: cell of the (i+1)-th sown piece of move m
  std::vector<uint8_t> dup;       // dup[m * (maxH + 1) + h]: the first h sown cells of m repeat
  std::vector<uint64_t> zob;      // zob[(c << (maxH + 1)) | code]: key of a stack on cell c
  std::vector<std::string> name;  // name[c], e.g. "e5"
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
  auto at = [&](int q, int r) -> int {
    if (std::abs(q) > R || std::abs(r) > R || std::abs(q + r) > R) return -1;
    return grid[(q + R) * S + (r + R)];
  };
  for (int i = 0; i < B.N; i++) {
    for (int d = 0; d < 6; d++) B.nb[i * 6 + d] = at(qs[i] + kDQ[d], rs[i] + kDR[d]);
    B.antipode[i] = at(-qs[i], -rs[i]);
  }
  const int H = B.maxH;
  B.walk.assign((size_t)B.N * 6 * H, -1);
  B.dup.assign((size_t)B.N * 6 * (H + 1), 0);
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
        bool rep = false;
        for (int a = 0; a < h; a++) {
          for (int b2 = a + 1; b2 < h; b2++) {
            rep = rep || B.walk[(size_t)m * H + a] == B.walk[(size_t)m * H + b2];
          }
        }
        B.dup[(size_t)m * (H + 1) + h] = rep;
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

// Recomputes the derived fields of a position from its cells.
void finishPos(Pos &p) {
  p.ws = p.bs = 0;
  for (int c = 0; c < B.N; c++) account(p.ws, p.bs, p.c[c], 1);
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

// Moves are cell * 6 + direction. Writes the legal ones and returns how many.
int genMoves(const Pos &p, int *out) {
  if (p.ply >= B.maxply) return 0;
  int n = 0;
  for (int c = 0; c < B.N; c++) {
    const Cell &x = p.c[c];
    if (!x.h || topColour(x) != p.side) continue;
    for (int d = 0; d < 6; d++) {
      if (B.nb[c * 6 + d] >= 0) out[n++] = c * 6 + d;
    }
  }
  return n;
}

// True if the move leaves some stack at or above the collapse height.
bool causesCollapse(const Pos &p, int m) {
  const int c = m / 6;
  const int h = p.c[c].h;
  const int *w = &B.walk[(size_t)m * B.maxH];
  const bool repeats = B.dup[(size_t)m * (B.maxH + 1) + h];
  for (int i = 0; i < h; i++) {
    const int t = w[i];
    const int base = (t == c) ? 0 : p.c[t].h;
    if (base + 1 < B.collapse && !repeats) continue;
    int occ = 0;
    for (int j = 0; j < h; j++) occ += (w[j] == t);
    if (base + occ >= B.collapse) return true;
  }
  return false;
}

// Plays a legal move: sows the stack, collapses tall stacks, and passes the turn.
void makeMove(const Pos &p, int m, Pos &q) {
  std::memcpy(q.c, p.c, sizeof(Cell) * B.N);
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
  int ws = p.ws, bs = p.bs;
  for (int j = 0; j < nt; j++) {
    key ^= cellKey(touched[j], q.c[touched[j]]);
    account(ws, bs, q.c[touched[j]], -1);
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
  }

  q.side = p.side ^ 1;
  q.ply = p.ply + 1;
  q.capW = capW;
  q.capB = capB;
  q.ws = ws;
  q.bs = bs;
  q.key = key ^ kSideKey;
}

// ---- evaluation ------------------------------------------------------------

// White's margin in units of 1/kScale point: white's score minus black's, less komi.
inline int evalWhite(const Pos &p) {
  return (p.ws + p.capW - p.bs - p.capB) * kScale - B.komi;
}

// The same from the side to move's point of view.
inline int evalStm(const Pos &p) {
  const int e = evalWhite(p);
  return p.side == 0 ? e : -e;
}

// Score from the side to move's view once the game has ended. The winner is decided
// by the margin, and the sooner the game ends, the larger the win.
int terminalScore(const Pos &p) {
  const int e = evalWhite(p);
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

  int moves[kMaxMoves];
  const int n = genMoves(p, moves);
  int best = stand;
  Pos child;
  for (int i = 0; i < n; i++) {
    if (!causesCollapse(p, moves[i])) continue;
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

  int moves[kMaxMoves], score[kMaxMoves];
  bool tactical[kMaxMoves];
  const int n = genMoves(p, moves);
  const int k0 = ply < kMaxPly ? S.killer[ply][0] : -1;
  const int k1 = ply < kMaxPly ? S.killer[ply][1] : -1;
  for (int i = 0; i < n; i++) {
    const int m = moves[i];
    tactical[i] = causesCollapse(p, m);
    if (m == ttMove) score[i] = 1 << 30;
    else if (tactical[i]) score[i] = (1 << 29);
    else if (m == k0) score[i] = (1 << 28);
    else if (m == k1) score[i] = (1 << 28) - 1;
    else score[i] = S.history[m];
  }

  int best = -kInf, bestMove = -1;
  Pos child;
  for (int i = 0; i < n; i++) {
    // Selection: bring the best remaining move to position i.
    int bi = i;
    for (int j = i + 1; j < n; j++) if (score[j] > score[bi]) bi = j;
    std::swap(moves[i], moves[bi]);
    std::swap(score[i], score[bi]);
    std::swap(tactical[i], tactical[bi]);
    const int m = moves[i];

    makeMove(p, m, child);
    int v;
    if (i == 0) {
      v = -negamax(child, depth - 1, -beta, -alpha, ply + 1);
    } else {
      // Late quiet moves are searched at reduced depth first; anything that beats
      // alpha is re-searched at full depth. Other moves get a null window.
      int reduce = 0;
      if (depth >= 3 && i >= 4 && !tactical[i] && m != k0 && m != k1) reduce = 1;
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
          if (!tactical[i]) {
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

// Searches every root move to the given depth, reordering them by score. Returns
// false if the search was stopped before it finished.
bool searchRoot(const Pos &root, int depth, std::vector<RootMove> &moves) {
  int alpha = -kInf;
  for (RootMove &rm : moves) {
    Pos child;
    makeMove(root, rm.m, child);
    const int v = -negamax(child, depth - 1, -kInf, -alpha, 1);
    if (S.stop) return false;
    rm.score = v;
    if (v > alpha) alpha = v;
  }
  std::stable_sort(moves.begin(), moves.end(),
                   [](const RootMove &a, const RootMove &b) { return a.score > b.score; });
  return true;
}

// Chooses a move for the position, searching for at most budgetMs (if positive),
// the node limit, or up to maxDepth. Depth 1 always completes.
int chooseMove(const Pos &root, int maxDepth, double budgetMs) {
  int moves[kMaxMoves];
  const int n = genMoves(root, moves);
  if (n == 0) return -1;
  if (n == 1) return moves[0];
  std::vector<RootMove> rootMoves;
  for (int i = 0; i < n; i++) rootMoves.push_back({moves[i], 0});

  S.nodes = 0;
  S.stop = false;
  S.timed = budgetMs > 0;
  S.deadline = Clock::now() + std::chrono::microseconds((long long)(budgetMs * 1000));
  int best = rootMoves[0].m;
  for (int depth = 1; depth <= maxDepth && depth < kMaxPly; depth++) {
    S.allowStop = depth > 1;
    if (!searchRoot(root, depth, rootMoves)) break;
    best = rootMoves[0].m;
    std::printf("info depth %d score %d nodes %llu\n", depth, rootMoves[0].score,
                (unsigned long long)S.nodes);
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
        if (tok[i] == "movetime") budgetMs = std::atof(tok[i + 1].c_str()) * 0.9;
        else if (tok[i] == "depth") maxDepth = std::atoi(tok[i + 1].c_str());
        else if (tok[i] == "nodes") nodes = std::atoll(tok[i + 1].c_str());
      }
      if (budgetMs <= 0 && nodes == 0 && maxDepth == 64) budgetMs = 225;
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
