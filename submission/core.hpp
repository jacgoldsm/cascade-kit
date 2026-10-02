// Cascade engine core: board, move generation, evaluation and search.
// Fixed to the benchmark rules: side=5 collapse=6 maxply=150 komi=0.5.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

namespace cz {

constexpr int N = 61;
constexpr int R = 4;
constexpr int MAXH = 5;
constexpr int COLLAPSE = 6;
constexpr int MAXPLY = 150;
constexpr int EMPTY = 1;  // stack code of an empty cell

// A stack is encoded as code = (1 << h) | bits, bit i = colour of the i-th piece from
// the bottom (0 white, 1 black). Heights up to 7 occur transiently while sowing.
struct Tables {
  int8_t nb[N][6];
  int8_t walk[N][6][MAXH];
  int8_t q[N], r[N];
  int8_t ring[N];
  char names[N][4];
  uint8_t H[256];      // height
  int8_t TOP[256];     // controller: 0 white, 1 black, -1 empty
  uint8_t CNT[256][2]; // pieces of each colour
  uint64_t Z[N][64];
  uint64_t Zside;
  uint64_t Zply[MAXPLY + 2];
  uint64_t Zcap[2][64];
  int ndir[N];

  Tables() {
    int idx = 0;
    int cellAt[9][9];
    for (int rr = -R; rr <= R; rr++) {
      int qmin = std::max(-R, -R - rr), qmax = std::min(R, R - rr);
      for (int qq = qmin; qq <= qmax; qq++) {
        q[idx] = qq; r[idx] = rr;
        cellAt[qq + R][rr + R] = idx;
        snprintf(names[idx], 4, "%c%d", 'a' + rr + R, qq - qmin + 1);
        idx++;
      }
    }
    static const int DQ[6] = {1, 1, 0, -1, -1, 0};
    static const int DR[6] = {0, -1, -1, 0, 1, 1};
    for (int c = 0; c < N; c++) {
      int s = -q[c] - r[c];
      ring[c] = std::max({std::abs(q[c]), std::abs(r[c]), std::abs(s)});
      ndir[c] = 0;
      for (int d = 0; d < 6; d++) {
        int nq = q[c] + DQ[d], nr = r[c] + DR[d];
        if (std::abs(nq) <= R && std::abs(nr) <= R && std::abs(nq + nr) <= R) {
          nb[c][d] = cellAt[nq + R][nr + R];
          ndir[c]++;
        } else nb[c][d] = -1;
      }
    }
    for (int c = 0; c < N; c++)
      for (int d0 = 0; d0 < 6; d0++) {
        if (nb[c][d0] < 0) { for (int i = 0; i < MAXH; i++) walk[c][d0][i] = -1; continue; }
        int pos = c, d = d0;
        for (int i = 0; i < MAXH; i++) {
          if (nb[pos][d] < 0) d = (d + 3) % 6;
          pos = nb[pos][d];
          walk[c][d0][i] = pos;
        }
      }
    for (int code = 0; code < 256; code++) {
      int h = 0;
      while (h < 8 && (code >> (h + 1))) h++;
      if (code == 0) h = 0;
      H[code] = h;
      int bits = code & ((1 << h) - 1);
      TOP[code] = h == 0 ? -1 : (bits >> (h - 1)) & 1;
      int b = __builtin_popcount(bits);
      CNT[code][1] = b;
      CNT[code][0] = h - b;
    }
    uint64_t s = 0x123456789abcdefULL;
    auto rnd = [&]() {
      s += 0x9E3779B97F4A7C15ULL;
      uint64_t z = s;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
      return z ^ (z >> 31);
    };
    for (int c = 0; c < N; c++)
      for (int k = 0; k < 64; k++) Z[c][k] = (k == EMPTY) ? 0 : rnd();
    Zside = rnd();
    for (int i = 0; i < MAXPLY + 2; i++) Zply[i] = rnd();
    for (int i = 0; i < 2; i++)
      for (int k = 0; k < 64; k++) Zcap[i][k] = k == 0 ? 0 : rnd();
  }
};

inline const Tables& T() {
  static Tables t;
  return t;
}

typedef int Move;  // cell * 6 + dir

struct Pos {
  uint8_t c[N];
  uint8_t stm;
  uint8_t ply;
  uint8_t cap[2];
  uint8_t nstk[2];
  uint64_t key;

  void computeKey() {
    const Tables& t = T();
    key = 0;
    nstk[0] = nstk[1] = 0;
    for (int i = 0; i < N; i++) {
      key ^= t.Z[i][c[i]];
      if (c[i] != EMPTY) nstk[t.TOP[c[i]]]++;
    }
    if (stm) key ^= t.Zside;
    key ^= t.Zply[ply];
    key ^= t.Zcap[0][cap[0]] ^ t.Zcap[1][cap[1]];
  }

  // Score difference from `side`'s point of view in half points, including komi.
  int marginHalf(int side) const {
    int w = nstk[0] + cap[0], b = nstk[1] + cap[1];
    int m = 2 * (w - b) - 1;  // komi 0.5 for black
    return side == 0 ? m : -m;
  }

  bool hasMoves() const { return nstk[stm] > 0; }
  bool terminal() const { return ply >= MAXPLY || nstk[stm] == 0; }

  int genMoves(Move* out) const {
    const Tables& t = T();
    int n = 0;
    for (int i = 0; i < N; i++) {
      if (t.TOP[c[i]] != stm) continue;
      for (int d = 0; d < 6; d++)
        if (t.nb[i][d] >= 0) out[n++] = i * 6 + d;
    }
    return n;
  }

  // Applies move m in place.
  void make(Move m) {
    const Tables& t = T();
    int cell = m / 6, d = m % 6;
    int mover = stm;
    uint8_t code = c[cell];
    int h = t.H[code];
    int bits = code ^ (1 << h);
    int touched[MAXH + 1];
    uint8_t orig[MAXH + 1];
    int nt = 0;
    touched[nt] = cell; orig[nt] = code; nt++;
    c[cell] = EMPTY;
    const int8_t* w = t.walk[cell][d];
    for (int i = 0; i < h; i++) {
      int x = w[i];
      bool seen = false;
      for (int k = 0; k < nt; k++) if (touched[k] == x) { seen = true; break; }
      if (!seen) { touched[nt] = x; orig[nt] = c[x]; nt++; }
      uint8_t o = c[x];
      c[x] = o + (uint8_t)((1 + ((bits >> i) & 1)) << t.H[o]);
    }
    uint8_t cap0 = cap[mover];
    for (int k = 0; k < nt; k++) {
      int x = touched[k];
      uint8_t o = orig[k], f = c[x];
      if (t.H[f] >= COLLAPSE) {
        cap[mover] += t.CNT[f][mover ^ 1];
        f = EMPTY;
        c[x] = EMPTY;
      }
      if (o != EMPTY) nstk[t.TOP[o]]--;
      if (f != EMPTY) nstk[t.TOP[f]]++;
      key ^= t.Z[x][o] ^ t.Z[x][f];
    }
    if (cap[mover] != cap0) key ^= t.Zcap[mover][cap0] ^ t.Zcap[mover][cap[mover] & 63];
    stm ^= 1;
    key ^= t.Zside;
    key ^= t.Zply[ply] ^ t.Zply[ply + 1];
    ply++;
  }

  std::string moveStr(Move m) const {
    static const char* DN[6] = {"E", "NE", "NW", "W", "SW", "SE"};
    return std::string(T().names[m / 6]) + DN[m % 6];
  }

  static int parseMove(const std::string& s) {
    static const char* DN[6] = {"E", "NE", "NW", "W", "SW", "SE"};
    const Tables& t = T();
    for (int c = 0; c < N; c++) {
      size_t L = strlen(t.names[c]);
      if (s.size() > L && s.compare(0, L, t.names[c]) == 0) {
        std::string d = s.substr(L);
        for (int k = 0; k < 6; k++) if (d == DN[k]) return c * 6 + k;
      }
    }
    return -1;
  }

  bool fromCSN(const std::string& str) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < str.size()) {
      while (i < str.size() && str[i] == ' ') i++;
      size_t j = i;
      while (j < str.size() && str[j] != ' ') j++;
      if (j > i) parts.push_back(str.substr(i, j - i));
      i = j;
    }
    if (parts.size() != 5) return false;
    int cell = 0;
    std::string cur;
    const std::string& b = parts[0];
    for (size_t k = 0; k <= b.size(); k++) {
      char ch = k < b.size() ? b[k] : ',';
      if (ch == ',' || ch == '/') {
        if (cell >= N) return false;
        uint8_t code = 1;
        if (cur != "-") {
          int h = 0, bits = 0;
          for (char x : cur) { if (x == 'b') bits |= 1 << h; h++; }
          code = (uint8_t)((1 << h) | bits);
        }
        c[cell++] = code;
        cur.clear();
      } else cur += ch;
    }
    if (cell != N) return false;
    stm = parts[1] == "b" ? 1 : 0;
    ply = (uint8_t)std::min(255, atoi(parts[2].c_str()));
    cap[0] = (uint8_t)atoi(parts[3].c_str());
    cap[1] = (uint8_t)atoi(parts[4].c_str());
    computeKey();
    return true;
  }

  void initial(uint32_t seed) {
    const Tables& t = T();
    uint32_t a = seed;
    auto next = [&]() {
      a += 0x9E3779B9u;
      uint32_t z = a;
      z = (z ^ (z >> 16)) * 0x21F0AAADu;
      z = (z ^ (z >> 15)) * 0x735A2D97u;
      return z ^ (z >> 15);
    };
    for (int i = 0; i < N; i++) c[i] = EMPTY;
    for (int i = 0; i < N; i++) {
      // antipode
      int aq = -t.q[i], ar = -t.r[i];
      int ap = -1;
      for (int j = 0; j < N; j++) if (t.q[j] == aq && t.r[j] == ar) ap = j;
      if (ap <= i) continue;
      uint32_t x = next();
      int black = x & 1;
      c[i] = (uint8_t)(2 | black);
      c[ap] = (uint8_t)(2 | (black ^ 1));
    }
    stm = 0; ply = 0; cap[0] = cap[1] = 0;
    computeKey();
  }
};

}  // namespace cz
