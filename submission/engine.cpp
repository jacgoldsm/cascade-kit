// Cascade engine: alpha-beta search with an incremental, per-cell evaluation.
//
// Board/rules follow RULES.md and cascade/rules.js.  A cell's stack is stored as a
// single integer "code": starting from 1 (empty), pushing a piece of colour x gives
// code = code*2 + x.  So the code is a sentinel 1 bit followed by the stack's
// colours from bottom to top, the top colour is code & 1, the height is
// bit_width(code) - 1 and the number of black pieces is popcount(code) - 1.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef int32_t i32;

static const int MAXN = 469;        // side 13
static const int MAXW = 8;          // 64-bit words needed for MAXN cells
static const int MAXDEPTH = 80;
static const i32 INFSC = 1 << 30;
static const i32 WINBASE = 1 << 21; // terminal (exact) results dominate heuristics
static const i32 EVCLAMP = 1 << 19;

static const int DQ[6] = { 1, 1, 0, -1, -1, 0 };
static const int DR[6] = { 0, -1, -1, 0, 1, 1 };
static const char* DIRN[6] = { "E", "NE", "NW", "W", "SW", "SE" };

static inline int bitwidth(u32 x) { return x ? 32 - __builtin_clz(x) : 0; }

// ---------------------------------------------------------------- geometry ----

struct Geo {
    int side = 0, collapse = 0, maxply = 0;
    double komi = 0.5;
    int komi1000 = 500;
    int N = 0, maxH = 0, nWords = 1, codes = 0, maxCode = 0;
    std::vector<int> cq, cr, rowStart, rowLen;
    std::vector<std::string> names;
    std::vector<int16_t> nbr;        // N*6, -1 off board
    std::vector<int16_t> walkT;      // (N*6)*maxH
    std::vector<uint8_t> deg, cen;
    std::vector<uint8_t> htab, ptab; // height / popcount by code, size maxCode
    std::vector<u16> prog;           // sow programs
    std::vector<i32> progStart;      // [(m*maxH) + h-1]
    std::vector<int16_t> cellMv;     // N*6
    std::vector<uint8_t> cellNMv;
    std::vector<uint8_t> distUpto;   // N*6: longest sow with all drops on distinct cells

    void init(int side_, int collapse_, int maxply_, double komi_) {
        side = side_; collapse = collapse_; maxply = maxply_; komi = komi_;
        komi1000 = (int)llround(komi * 1000.0);
        int R = side - 1;
        maxH = collapse - 1;
        cq.clear(); cr.clear(); names.clear(); rowStart.clear(); rowLen.clear();
        std::vector<std::vector<int> > idx(2 * R + 1, std::vector<int>(2 * R + 1, -1));
        for (int r = -R; r <= R; r++) {
            int qmin = std::max(-R, -R - r), qmax = std::min(R, R - r);
            rowStart.push_back((int)cq.size());
            rowLen.push_back(qmax - qmin + 1);
            for (int q = qmin; q <= qmax; q++) {
                idx[r + R][q + R] = (int)cq.size();
                char buf[24];
                snprintf(buf, sizeof buf, "%c%d", 'a' + (r + R), q - qmin + 1);
                names.push_back(std::string(buf));
                cq.push_back(q); cr.push_back(r);
            }
        }
        N = (int)cq.size();
        nWords = (N + 63) / 64;
        nbr.assign((size_t)N * 6, -1);
        deg.assign(N, 0); cen.assign(N, 0);
        for (int c = 0; c < N; c++) {
            for (int d = 0; d < 6; d++) {
                int nq = cq[c] + DQ[d], nr = cr[c] + DR[d];
                if (nq < -R || nq > R || nr < -R || nr > R) continue;
                int t = idx[nr + R][nq + R];
                if (t >= 0) { nbr[(size_t)c * 6 + d] = (int16_t)t; deg[c]++; }
            }
            int ad = std::max(std::max(std::abs(cq[c]), std::abs(cr[c])), std::abs(cq[c] + cr[c]));
            cen[c] = (uint8_t)(R - ad);
        }
        // sowing walks (bounce at the edge)
        walkT.assign((size_t)N * 6 * maxH, -1);
        for (int c = 0; c < N; c++) for (int d0 = 0; d0 < 6; d0++) {
            if (nbr[(size_t)c * 6 + d0] < 0) continue;
            int pos = c, d = d0;
            for (int i = 0; i < maxH; i++) {
                if (nbr[(size_t)pos * 6 + d] < 0) d = (d + 3) % 6;
                pos = nbr[(size_t)pos * 6 + d];
                walkT[((size_t)(c * 6 + d0)) * maxH + i] = (int16_t)pos;
            }
        }
        // per (move, height) program: distinct target cells, each with the bit shifts
        // of the pieces dropped on it, in drop order.
        progStart.assign((size_t)N * 6 * maxH, -1);
        prog.clear();
        int maxDrops = 1;
        std::vector<int> ts; std::vector<std::vector<int> > sh;
        for (int m = 0; m < N * 6; m++) {
            if (nbr[m] < 0) continue;
            for (int h = 1; h <= maxH; h++) {
                ts.clear(); sh.clear();
                for (int i = 0; i < h; i++) {
                    int t = walkT[(size_t)m * maxH + i];
                    int e = -1;
                    for (size_t k = 0; k < ts.size(); k++) if (ts[k] == t) { e = (int)k; break; }
                    if (e < 0) { ts.push_back(t); sh.push_back(std::vector<int>()); e = (int)ts.size() - 1; }
                    sh[e].push_back(h - 1 - i);
                }
                progStart[(size_t)m * maxH + h - 1] = (i32)prog.size();
                prog.push_back((u16)ts.size());
                for (size_t e = 0; e < ts.size(); e++) {
                    prog.push_back((u16)ts[e]);
                    prog.push_back((u16)sh[e].size());
                    maxDrops = std::max(maxDrops, (int)sh[e].size());
                    for (size_t k = 0; k < sh[e].size(); k++) prog.push_back((u16)sh[e][k]);
                }
            }
        }
        distUpto.assign((size_t)N * 6, 0);
        for (int m = 0; m < N * 6; m++) {
            if (nbr[m] < 0) continue;
            int k = 0;
            for (int h = 1; h <= maxH; h++) {
                int t = walkT[(size_t)m * maxH + h - 1];
                bool dup = false;
                for (int j = 0; j < h - 1; j++) if (walkT[(size_t)m * maxH + j] == t) { dup = true; break; }
                if (dup) break;
                k = h;
            }
            distUpto[m] = (uint8_t)k;
        }
        codes = 1 << collapse;
        maxCode = 1 << std::min(30, collapse + maxDrops);
        htab.assign(maxCode, 0); ptab.assign(maxCode, 0);
        for (int c = 1; c < maxCode; c++) {
            htab[c] = (uint8_t)(bitwidth((u32)c) - 1);
            ptab[c] = (uint8_t)__builtin_popcount((unsigned)c);
        }
        cellMv.assign((size_t)N * 6, -1); cellNMv.assign(N, 0);
        for (int c = 0; c < N; c++) {
            int k = 0;
            for (int d = 0; d < 6; d++) if (nbr[(size_t)c * 6 + d] >= 0) cellMv[(size_t)c * 6 + k++] = (int16_t)(c * 6 + d);
            cellNMv[c] = (uint8_t)k;
        }
    }

    int cellOf(const std::string& s) const {
        for (int i = 0; i < N; i++) if (names[i] == s) return i;
        return -1;
    }
    std::string moveStr(int m) const { return names[m / 6] + DIRN[m % 6]; }
    int parseMove(const std::string& in) const {
        std::string s;
        for (size_t i = 0; i < in.size(); i++) if (!isspace((unsigned char)in[i])) s += in[i];
        if (s.size() < 3) return -1;
        std::string lower = s;
        for (size_t i = 0; i < lower.size(); i++) lower[i] = (char)tolower((unsigned char)lower[i]);
        for (int d = 0; d < 6; d++) {
            std::string dn = DIRN[d];
            std::string dl = dn;
            for (size_t i = 0; i < dl.size(); i++) dl[i] = (char)tolower((unsigned char)dl[i]);
            if (lower.size() <= dl.size()) continue;
            if (lower.compare(lower.size() - dl.size(), dl.size(), dl) != 0) continue;
            std::string cellName = lower.substr(0, lower.size() - dl.size());
            // "e5SW" must not also match direction "W" with cell "e5S"
            int c = cellOf(cellName);
            if (c < 0) continue;
            return c * 6 + d;
        }
        return -1;
    }
};

// ----------------------------------------------------------------- weights ----

struct Weights {
    int ctrl, mob, pos, wcap, tempo, mobh, bias, mobA, posA;
    int pure[14];   // bonus for a stack whose pieces are all one colour
    int ctlh[14];   // control value adjustment by stack height
    int pv[14];     // value per net own piece by stack height
    int lmp[12];    // move-count pruning limit by depth
    int fut[5];     // futility margin by depth
    int dlim;       // 0 = unlimited (used for fixed-depth reference players)
    int asp;        // aspiration window
    int rbase, rm1, rm2, rm3;
    int nmp, nmpR, nmpD, rfp, rfpM;
    int phl;                   // plies before the end over which positional terms fade
    int rlog, rl0, rl1;        // logarithmic late-move reductions
    int pw[14][14];            // value of the piece i places below the top of a
    bool pwSet[14][14];        // height-h stack, when explicitly set
    int iir, hmal, cmb;
    Weights() {
        ctrl = 1000; mob = 17; pos = -41; wcap = 651; tempo = 400; mobh = 15; bias = -269;
        mobA = -5; posA = 38;
        memset(pure, 0, sizeof pure);
        pure[2] = 1681; pure[3] = 703;   // a stack of one colour is worth extra
        dlim = 0; asp = 1200;
        rbase = 1; rm1 = 5; rm2 = 14; rm3 = 30;
        nmp = 1; nmpR = 2; nmpD = 2; rfp = 1; rfpM = 2200;
        phl = 0; rlog = 0; rl0 = 20; rl1 = 55;
        iir = 1; hmal = 60; cmb = 0;
        static const int dl[12] = { 0, 0, 8, 14, 22, 32, 48, 70, 110, 170, 280, 450 };
        memcpy(lmp, dl, sizeof lmp);
        fut[0] = 0; fut[1] = 1100; fut[2] = 2600; fut[3] = 5200; fut[4] = 0;
        memset(pw, 0, sizeof pw);
        memset(pwSet, 0, sizeof pwSet);
        // Piece values by stack height and distance below the top, fitted to the
        // results of self-play games by logistic regression.
        static const int dpw[6][5] = {
            { 0, 0, 0, 0, 0 },
            { 1000, 0, 0, 0, 0 },
            { 251, 14, 0, 0, 0 },
            { 927, 745, 648, 0, 0 },
            { 1242, 765, 704, 663, 0 },
            { 971, 830, 530, 427, 513 },
        };
        for (int h = 1; h <= 5; h++) for (int ix = 0; ix < h; ix++) { pw[h][ix] = dpw[h][ix]; pwSet[h][ix] = true; }
        // Only used for stack heights the piece table below does not cover (that is,
        // for collapse heights other than the default 6).
        static const int dc[14] = { 0, 0, -350, -350, -350, -350, -350, -350, -350, -350, -350, -350, -350, -350 };
        static const int dp[14] = { 0, 0, 650, 650, 650, 650, 650, 650, 650, 650, 650, 650, 650, 650 };
        memcpy(ctlh, dc, sizeof ctlh);
        memcpy(pv, dp, sizeof pv);
    }
    bool set(const std::string& k, int v) {
        if (k == "ctrl") { ctrl = v; return true; }
        if (k == "tempo") { tempo = v; return true; }
        if (k == "dlim") { dlim = v; return true; }
        if (k == "asp") { asp = v; return true; }
        if (k == "rbase") { rbase = v; return true; }
        if (k == "rm1") { rm1 = v; return true; }
        if (k == "rm2") { rm2 = v; return true; }
        if (k == "rm3") { rm3 = v; return true; }
        if (k == "nmp") { nmp = v; return true; }
        if (k == "nmpR") { nmpR = v; return true; }
        if (k == "nmpD") { nmpD = v; return true; }
        if (k == "rfp") { rfp = v; return true; }
        if (k == "rfpM") { rfpM = v; return true; }
        if (k == "phl") { phl = v; return true; }
        if (k == "rlog") { rlog = v; return true; }
        if (k == "rl0") { rl0 = v; return true; }
        if (k == "rl1") { rl1 = v; return true; }
        if (k == "iir") { iir = v; return true; }
        if (k == "hmal") { hmal = v; return true; }
        if (k == "cmb") { cmb = v; return true; }
        if (k.size() > 3 && k[0] == 'p' && isdigit((unsigned char)k[1])) {
            size_t us = k.find('_');
            if (us != std::string::npos) {
                int h = atoi(k.c_str() + 1), ix = atoi(k.c_str() + us + 1);
                if (h >= 1 && h <= 13 && ix >= 0 && ix < h) { pw[h][ix] = v; pwSet[h][ix] = true; return true; }
            }
        }
        if (k.size() > 3 && k.compare(0, 3, "lmp") == 0) { int h = atoi(k.c_str() + 3); if (h >= 2 && h <= 11) { lmp[h] = v; return true; } }
        if (k.size() > 3 && k.compare(0, 3, "fut") == 0) { int h = atoi(k.c_str() + 3); if (h >= 1 && h <= 3) { fut[h] = v; return true; } }
        if (k == "mob") { mob = v; return true; }
        if (k == "mobh") { mobh = v; return true; }
        if (k == "bias") { bias = v; return true; }
        if (k == "mobA") { mobA = v; return true; }
        if (k == "posA") { posA = v; return true; }
        if (k.size() > 4 && k.compare(0, 4, "pure") == 0) { int h = atoi(k.c_str() + 4); if (h >= 1 && h <= 13) { pure[h] = v; return true; } }
        if (k == "pos") { pos = v; return true; }
        if (k == "wcap") { wcap = v; return true; }
        if (k.size() > 4 && k.compare(0, 4, "ctlh") == 0) { int h = atoi(k.c_str() + 4); if (h >= 1 && h <= 13) { ctlh[h] = v; return true; } }
        if (k.size() > 2 && k.compare(0, 2, "pv") == 0) { int h = atoi(k.c_str() + 2); if (h >= 1 && h <= 13) { pv[h] = v; return true; } }
        return false;
    }
    // A copy with the positional terms faded towards zero (phase 256 = unchanged,
    // phase 0 = plain control + captures, which is exactly the game's score).
    Weights scaled(int phase) const {
        Weights r = *this;
        if (phase >= 256) return r;
        r.mob = mob * phase / 256;
        r.mobh = mobh * phase / 256;
        r.bias = bias * phase / 256;
        r.mobA = mobA * phase / 256;
        r.posA = posA * phase / 256;
        for (int h = 0; h < 14; h++) r.pure[h] = pure[h] * phase / 256;
        r.pos = pos * phase / 256;
        r.tempo = tempo * phase / 256;
        r.wcap = 1000 + (wcap - 1000) * phase / 256;
        for (int h = 0; h < 14; h++) {
            r.ctlh[h] = ctlh[h] * phase / 256;
            r.pv[h] = pv[h] * phase / 256;
        }
        r.ctrl = 1000 + (ctrl - 1000) * phase / 256;
        for (int h = 1; h <= 13; h++) for (int ix = 0; ix < h; ix++) if (pwSet[h][ix])
            r.pw[h][ix] = ix == 0 ? 1000 + (pw[h][0] - 1000) * phase / 256 : pw[h][ix] * phase / 256;
        return r;
    }

    void parse(const std::string& s) {
        size_t i = 0;
        while (i < s.size()) {
            size_t j = s.find_first_of(",;", i);
            if (j == std::string::npos) j = s.size();
            std::string tok = s.substr(i, j - i);
            size_t eq = tok.find('=');
            if (eq != std::string::npos) set(tok.substr(0, eq), atoi(tok.c_str() + eq + 1));
            i = j + 1;
        }
    }
};

// ---------------------------------------------------------------- transposition ----

struct TTEntry { u64 key; i32 score; int16_t move; int8_t depth; uint8_t flag; };
static const uint8_t TT_NONE = 0, TT_EXACT = 1, TT_LOWER = 2, TT_UPPER = 3;

// ---------------------------------------------------------------- engine ----

struct Undo {
    u64 key;
    i32 ev;
    int16_t cell[16], wc, bc, cw, cb;
    u16 old[16];
    int8_t n;
};

struct Engine {
    Geo g;
    Weights w;   // configured weights (search knobs are read from here)
    Weights we;  // phase-scaled weights used by the evaluation tables
    std::vector<i32> valC;   // nCls*codes, white's point of view, milli-points
    std::vector<i32> addT;   // ((mover*2+bit)*nCls + cls)*codes: value of one drop
    std::vector<i32> clsBase;// N: cls*codes
    std::vector<uint8_t> clsOf;
    int nCls = 1, addStride = 0;
    std::vector<u64> zob;    // N*codes
    std::vector<u64> zply, zcw, zcb;
    u64 zside = 0;

    // position
    u16 code[MAXN];
    u64 ctl[2][MAXW];
    int wCells = 0, bCells = 0, capW = 0, capB = 0, side = 0, ply = 0;
    i32 evalAcc = 0;
    u64 key = 0;

    std::vector<TTEntry> tt;
    size_t ttMask = 0;
    std::vector<i32> hist[2];
    std::vector<int16_t> cmove;   // (N*6) -> reply that refuted it
    int killer[MAXDEPTH][2];
    Undo undo[MAXDEPTH];

    u64 nodes = 0;   // work units: node visits plus scored moves
    u64 visits = 0;
    bool stopped = false;
    std::chrono::steady_clock::time_point t0;
    double budgetMs = 1e18;
    u64 nodeLimit = 0;
    int depthLimit = 64;
    int selDepth = 0;
    bool verbose = false;
    int seldepthReached = 0;

    Engine() { setTTSize(21); }

    void setTTSize(int bits) {
        size_t n = (size_t)1 << bits;
        tt.assign(n, TTEntry());
        for (size_t i = 0; i < n; i++) { tt[i].key = 0; tt[i].flag = TT_NONE; }
        ttMask = n - 1;
    }

    void setRules(int side_, int collapse_, int maxply_, double komi_) {
        g.init(side_, collapse_, maxply_, komi_);
        we = w;
        buildTables();
        reset(0);
    }

    static u64 sm64(u64& x) {
        x += 0x9E3779B97F4A7C15ull;
        u64 z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    void buildTables() { buildEval(); buildZob(); }

    void buildEval() {
        int N = g.N, C = g.codes;
        // cells only enter the evaluation through (degree, centrality), so group them
        std::vector<int> key(N);
        std::vector<int> uniq;
        clsOf.assign(N, 0);
        for (int c = 0; c < N; c++) {
            key[c] = g.deg[c] * 32 + g.cen[c];
            size_t j = 0;
            for (; j < uniq.size(); j++) if (uniq[j] == key[c]) break;
            if (j == uniq.size()) uniq.push_back(key[c]);
            clsOf[c] = (uint8_t)j;
        }
        nCls = (int)uniq.size();
        addStride = nCls * C;
        clsBase.assign(N, 0);
        for (int c = 0; c < N; c++) clsBase[c] = clsOf[c] * C;
        // Value of the piece `ix` places below the top of a height-h stack.  Derived
        // from (ctrl, ctlh, pv) unless a weight sets it explicitly.
        int P[14][14];
        for (int h = 1; h <= 13; h++) for (int ix = 0; ix < h; ix++) {
            P[h][ix] = ix == 0 ? (we.ctrl + we.ctlh[h] + we.pv[h]) : we.pv[h];
            if (we.pwSet[h][ix]) P[h][ix] = we.pw[h][ix];
        }
        valC.assign((size_t)nCls * C, 0);
        for (int k = 0; k < nCls; k++) {
            int dg = uniq[k] / 32, cn = uniq[k] % 32;
            for (int cd = 1; cd < C; cd++) {
                int h = g.htab[cd];
                if (h == 0) continue;
                int hh = h < 14 ? h : 13;
                int sg = (cd & 1) ? -1 : 1;
                i32 v = sg * ((we.mob + we.mobh * (h - 1)) * dg + we.pos * cn);
                int net = 0;
                for (int ix = 0; ix < h; ix++) {
                    int bit = (cd >> ix) & 1;     // ix places below the top
                    int si = bit ? -1 : 1;
                    net += si;
                    v += si * P[hh][ix < 13 ? ix : 13];
                }
                v += net * (we.mobA * dg + we.posA * cn);
                if (net == h || net == -h) v += sg * we.pure[hh];
                valC[(size_t)k * C + cd] = v;
            }
        }
        addT.assign((size_t)4 * addStride, 0);
        for (int mover = 0; mover < 2; mover++) for (int bit = 0; bit < 2; bit++)
            for (int k = 0; k < nCls; k++) for (int oc = 1; oc < C; oc++) {
                int nc = (oc << 1) | bit;
                int nh = g.htab[nc];
                i32 v;
                if (nh >= g.collapse) {
                    int blacks = g.ptab[nc] - 1;
                    int caps = mover == 0 ? blacks : (nh - blacks);
                    v = -valC[(size_t)k * C + oc] + (mover == 0 ? we.wcap * caps : -we.wcap * caps);
                } else {
                    v = valC[(size_t)k * C + nc] - valC[(size_t)k * C + oc];
                }
                addT[((size_t)(mover * 2 + bit) * nCls + k) * C + oc] = v;
            }
    }

    void buildZob() {
        int N = g.N, C = g.codes;
        zob.assign((size_t)N * C, 0);
        u64 s = 0x1234567890ABCDEFull;
        for (size_t i = 0; i < zob.size(); i++) zob[i] = sm64(s);
        for (int c = 0; c < N; c++) zob[(size_t)c * C + 1] = 0; // empty contributes nothing
        zside = sm64(s);
        zply.assign(g.maxply + 2, 0);
        for (int i = 0; i <= g.maxply + 1; i++) zply[i] = sm64(s);
        zcw.assign(N + 2, 0); zcb.assign(N + 2, 0);
        for (int i = 0; i < N + 2; i++) zcw[i] = sm64(s);
        for (int i = 0; i < N + 2; i++) zcb[i] = sm64(s);
        hist[0].assign((size_t)N * 6, 0);
        hist[1].assign((size_t)N * 6, 0);
        cmove.assign((size_t)N * 6, -1);
    }

    // ---- position ----

    void recompute() {
        int N = g.N, C = g.codes;
        wCells = bCells = 0; evalAcc = 0; key = 0;
        for (int i = 0; i < 2; i++) for (int j = 0; j < MAXW; j++) ctl[i][j] = 0;
        for (int c = 0; c < N; c++) {
            u16 cd = code[c];
            evalAcc += valC[(size_t)clsBase[c] + cd];
            key ^= zob[(size_t)c * C + cd];
            if (cd > 1) {
                int t = cd & 1;
                ctl[t][c >> 6] |= 1ull << (c & 63);
                if (t) bCells++; else wCells++;
            }
        }
        evalAcc += we.wcap * (capW - capB);
        key ^= zcw[capW] ^ zcb[capB] ^ zply[ply];
        if (side) key ^= zside;
    }

    void reset(u32 seed) {
        int N = g.N;
        for (int c = 0; c < N; c++) code[c] = 1;
        // splitmix32 opening, exactly as RULES.md section 3
        u32 a = seed;
        std::vector<int> anti(N);
        for (int c = 0; c < N; c++) {
            int tq = -g.cq[c], tr = -g.cr[c];
            int R = g.side - 1;
            int qmin = std::max(-R, -R - tr);
            anti[c] = g.rowStart[tr + R] + (tq - qmin);
        }
        for (int c = 0; c < N; c++) {
            int ac = anti[c];
            if (ac <= c) continue;
            a = (u32)(a + 0x9e3779b9u);
            u32 z = a;
            z = (u32)((z ^ (z >> 16)) * 0x21f0aaadu);
            z = (u32)((z ^ (z >> 15)) * 0x735a2d97u);
            z = z ^ (z >> 15);
            int blackHere = (int)(z & 1u);
            code[c] = (u16)(2 + blackHere);
            code[ac] = (u16)(2 + (blackHere ^ 1));
        }
        side = 0; ply = 0; capW = 0; capB = 0;
        recompute();
    }

    inline void setCell(int c, u16 oc, u16 nc) {
        int C = g.codes;
        key ^= zob[(size_t)c * C + oc] ^ zob[(size_t)c * C + nc];
        evalAcc += valC[(size_t)clsBase[c] + nc] - valC[(size_t)clsBase[c] + oc];
        u64 bit = 1ull << (c & 63);
        int wd = c >> 6;
        if (oc > 1) { int t = oc & 1; ctl[t][wd] &= ~bit; if (t) bCells--; else wCells--; }
        if (nc > 1) { int t = nc & 1; ctl[t][wd] |= bit; if (t) bCells++; else wCells++; }
        code[c] = nc;
    }

    inline int genMoves(int* out) const {
        int n = 0;
        for (int wd = 0; wd < g.nWords; wd++) {
            u64 bb = ctl[side][wd];
            while (bb) {
                int c = wd * 64 + __builtin_ctzll(bb);
                bb &= bb - 1;
                int k = g.cellNMv[c];
                const int16_t* cm = &g.cellMv[(size_t)c * 6];
                for (int j = 0; j < k; j++) out[n++] = cm[j];
            }
        }
        return n;
    }

    inline bool isLegal(int m) const {
        if (m < 0 || m >= g.N * 6) return false;
        if (ply >= g.maxply) return false;
        int c = m / 6;
        u16 cd = code[c];
        if (cd <= 1) return false;
        if ((cd & 1) != side) return false;
        return g.nbr[m] >= 0;
    }

    void make(int m, Undo& u) {
        u.key = key; u.ev = evalAcc;
        u.wc = (int16_t)wCells; u.bc = (int16_t)bCells; u.cw = (int16_t)capW; u.cb = (int16_t)capB;
        u.n = 0;
        int c = m / 6;
        u16 code0 = code[c];
        int h = g.htab[code0];
        u.cell[u.n] = (int16_t)c; u.old[u.n] = code0; u.n++;
        setCell(c, code0, 1);
        const u16* p = &g.prog[g.progStart[(size_t)m * g.maxH + h - 1]];
        int ne = *p++;
        int caps = 0;
        for (int e = 0; e < ne; e++) {
            int t = *p++;
            int cnt = *p++;
            u16 oc = code[t], nc = oc;
            for (int k = 0; k < cnt; k++) nc = (u16)((nc << 1) | ((code0 >> *p++) & 1));
            int nh = g.htab[nc];
            if (nh >= g.collapse) {
                int blacks = g.ptab[nc] - 1;
                caps += side == 0 ? blacks : (nh - blacks);
                nc = 1;
            }
            u.cell[u.n] = (int16_t)t; u.old[u.n] = oc; u.n++;
            setCell(t, oc, nc);
        }
        if (caps) {
            if (side == 0) { key ^= zcw[capW] ^ zcw[capW + caps]; capW += caps; evalAcc += we.wcap * caps; }
            else { key ^= zcb[capB] ^ zcb[capB + caps]; capB += caps; evalAcc -= we.wcap * caps; }
        }
        key ^= zply[ply] ^ zply[ply + 1];
        ply++;
        side ^= 1;
        key ^= zside;
    }

    inline void makeNull() { side ^= 1; key ^= zside; }
    inline void unmakeNull() { side ^= 1; key ^= zside; }

    void unmake(const Undo& u) {
        for (int i = u.n - 1; i >= 0; i--) code[u.cell[i]] = u.old[i];
        for (int i = 0; i < u.n; i++) {
            int c = u.cell[i];
            u64 bit = 1ull << (c & 63);
            int wd = c >> 6;
            ctl[0][wd] &= ~bit; ctl[1][wd] &= ~bit;
            u16 cd = code[c];
            if (cd > 1) ctl[cd & 1][wd] |= bit;
        }
        key = u.key; evalAcc = u.ev;
        wCells = u.wc; bCells = u.bc; capW = u.cw; capB = u.cb;
        ply--; side ^= 1;
    }

    // White-perspective change in the static evaluation if `m` is played.  Used where
    // a single move is scored in isolation (the root); the search uses scan().
    i32 staticDelta(int m) {
        int c = m / 6;
        u16 code0 = code[c];
        int h = g.htab[code0];
        code[c] = 1;
        i32 d = -valC[clsBase[c] + code0] + deltaOf(m, code0, h, side);
        code[c] = code0;
        return d;
    }

    // Change in the evaluation from the drops of move `m`, assuming the origin cell
    // has already been emptied.  `mover` decides which pieces a collapse captures.
    inline i32 deltaOf(int m, u16 code0, int h, int mover) const {
        const i32* ap0 = &addT[(size_t)(mover * 2) * addStride];
        const i32* ap1 = ap0 + addStride;
        i32 d = 0;
        if (h <= g.distUpto[m]) {           // every piece lands on a different cell
            const int16_t* wp = &g.walkT[(size_t)m * g.maxH];
            u16 bits = code0;
            for (int i = h - 1; i >= 0; i--) {
                int t = wp[i];
                const i32* ap = (bits & 1) ? ap1 : ap0;
                bits >>= 1;
                d += ap[clsBase[t] + code[t]];
            }
            return d;
        }
        const u16* p = &g.prog[g.progStart[(size_t)m * g.maxH + h - 1]];
        int ne = *p++;
        {
            int caps = 0;
            for (int e = 0; e < ne; e++) {
                int t = *p++;
                int cnt = *p++;
                u16 oc = code[t], nc = oc;
                for (int q = 0; q < cnt; q++) nc = (u16)((nc << 1) | ((code0 >> *p++) & 1));
                int nh = g.htab[nc];
                if (nh >= g.collapse) {
                    int blacks = g.ptab[nc] - 1;
                    caps += mover == 0 ? blacks : (nh - blacks);
                    nc = 1;
                }
                d += valC[clsBase[t] + nc] - valC[clsBase[t] + oc];
            }
            if (caps) d += mover == 0 ? we.wcap * caps : -we.wcap * caps;
        }
        return d;
    }

    // Generates the moves of the side to move and scores each by the resulting change
    // in the static evaluation.  With FRONT it instead returns the best child score
    // from the mover's point of view, stopping early once beta is beaten.
    template<bool FRONT>
    i32 scan(int* mv, i32* dv, int* nOut, i32 fbase, i32 beta) {
        int n = 0;
        int mover = side;
        int sgn = mover == 0 ? 1 : -1;
        i32 best = -INFSC;
        for (int wd = 0; wd < g.nWords; wd++) {
            u64 bb = ctl[mover][wd];
            while (bb) {
                int c = wd * 64 + __builtin_ctzll(bb);
                bb &= bb - 1;
                u16 code0 = code[c];
                int h = g.htab[code0];
                i32 base = -valC[clsBase[c] + code0];
                code[c] = 1;
                int k = g.cellNMv[c];
                const int16_t* cm = &g.cellMv[(size_t)c * 6];
                for (int j = 0; j < k; j++) {
                    int m = cm[j];
                    i32 d = base + deltaOf(m, code0, h, mover);
                    if (FRONT) {
                        i32 v = fbase + sgn * d;
                        if (v > best) best = v;
                    } else {
                        mv[n] = m; dv[n] = d; n++;
                    }
                }
                nodes += (u64)k;
                code[c] = code0;
                if (FRONT && best >= beta) return best;
            }
        }
        if (nOut) *nOut = n;
        return best;
    }

    // ---- evaluation ----

    inline bool terminal() const {
        if (ply >= g.maxply) return true;
        return (side == 0 ? wCells : bCells) == 0;
    }

    inline i32 exactScoreWhite() const {
        i32 m1000 = (i32)((wCells + capW - bCells - capB) * 1000 - g.komi1000);
        return (m1000 > 0 ? WINBASE : -WINBASE) + m1000;
    }

    inline i32 terminalScore() const {
        i32 s = exactScoreWhite();
        return side == 0 ? s : -s;
    }

    inline i32 evalW() const {
        i32 e = evalAcc - g.komi1000 + we.bias;
        if (e > EVCLAMP) e = EVCLAMP; else if (e < -EVCLAMP) e = -EVCLAMP;
        return e;
    }
    // static score from the mover's point of view, including a tempo bonus
    inline i32 evalStm() const { i32 e = evalW(); return (side == 0 ? e : -e) + we.tempo; }
    // static score of any child of this node, from this node's point of view, with
    // the child's tempo bonus (which belongs to the opponent) subtracted.
    inline i32 childBaseScore() const { i32 e = evalW(); return (side == 0 ? e : -e) - we.tempo; }

    inline void checkTime() {
        if (nodeLimit) { if (nodes >= nodeLimit) stopped = true; return; }
        std::chrono::duration<double, std::milli> el = std::chrono::steady_clock::now() - t0;
        if (el.count() >= budgetMs) stopped = true;
    }

    // Depth-1 node: the best child by static evaluation, computed without making moves.
    i32 frontier(i32 alpha, i32 beta) {
        (void)alpha;
        return scan<true>(0, 0, 0, childBaseScore(), beta);
    }

    i32 search(int depth, i32 alpha, i32 beta, int pfr, bool canNull = true, int prevMove = -1) {
        nodes++;
        if (((++visits) & 255) == 0) checkTime();
        if (stopped) return 0;
        if (pfr > seldepthReached) seldepthReached = pfr;
        if (ply >= g.maxply || (side == 0 ? wCells : bCells) == 0) return terminalScore();
        if (depth <= 0 || pfr >= MAXDEPTH - 3) return evalStm();
        if (depth == 1 && ply + 1 < g.maxply) return frontier(alpha, beta);

        TTEntry& te = tt[key & ttMask];
        int ttMove = -1;
        if (te.flag != TT_NONE && te.key == key) {
            ttMove = te.move;
            if (te.depth >= depth) {
                if (te.flag == TT_EXACT) return te.score;
                if (te.flag == TT_LOWER && te.score >= beta) return te.score;
                if (te.flag == TT_UPPER && te.score <= alpha) return te.score;
            }
        }

        int sgn = side == 0 ? 1 : -1;
        i32 childBase = childBaseScore();
        bool pvNode = beta > alpha + 1;
        if (w.iir && ttMove < 0 && depth >= 4) depth -= w.iir;

        // reverse futility: a big static lead at low depth is rarely thrown away
        if (w.rfp && !pvNode && depth <= 3) {
            i32 st = evalStm();
            if (st - w.rfpM * depth >= beta && st < WINBASE / 2) return st - w.rfpM * depth;
        }
        // null move: hand the opponent a free move; if we still beat beta, cut
        if (w.nmp && !pvNode && canNull && depth >= w.nmpD && evalStm() >= beta
            && (side == 0 ? bCells : wCells) > 0 && beta < WINBASE / 2 && beta > -WINBASE / 2) {
            int R = w.nmpR + depth / 5;
            makeNull();
            i32 sc = -search(depth - 1 - R, -beta, -beta + 1, pfr + 1, false, -1);
            unmakeNull();
            if (stopped) return 0;
            if (sc >= beta) return sc < WINBASE / 2 ? sc : beta;
        }

        int* mv = &mvbuf[(size_t)pfr * mvStride];
        i32* ord = &ordbuf[(size_t)pfr * mvStride];
        i32* dv = &dvbuf[(size_t)pfr * mvStride];
        int n = 0;
        scan<false>(mv, dv, &n, 0, 0);
        const i32* H = &hist[side][0];
        int k0 = killer[pfr][0], k1 = killer[pfr][1];
        int cm = (w.cmb && prevMove >= 0) ? cmove[prevMove] : -1;
        for (int i = 0; i < n; i++) {
            int m = mv[i];
            i32 d = sgn * dv[i];
            dv[i] = d;
            i32 o = depth > 3 ? d + (H[m] >> 6) : d;
            if (m == ttMove) o += 1 << 24;
            else if (m == k0) o += 1 << 18;
            else if (m == k1) o += 1 << 17;
            else if (m == cm) o += w.cmb;
            ord[i] = o;
        }

        i32 best = -INFSC;
        int bestMove = -1;
        i32 a0 = alpha;
        int moveCount = 0;
        int lmpLim = depth < 12 ? w.lmp[depth] : 1 << 20;
        for (int i = 0; i < n; i++) {
            {
                int bi = i;
                for (int j = i + 1; j < n; j++) if (ord[j] > ord[bi]) bi = j;
                if (bi != i) { std::swap(mv[i], mv[bi]); std::swap(ord[i], ord[bi]); std::swap(dv[i], dv[bi]); }
            }
            int m = mv[i];
            moveCount++;
            if (moveCount > 1 && best > -WINBASE) {
                if (moveCount > lmpLim) break;
                if (depth <= 3 && childBase + dv[i] + w.fut[depth] <= alpha) break;
            }
            make(m, undo[pfr]);
            i32 sc;
            int nd = depth - 1;
            int red = 0;
            if (depth >= 3 && moveCount > 3) {
                red = redTab[depth < 64 ? depth : 63][moveCount < 64 ? moveCount : 63];
                if (pvNode && red > 0) red--;
                if (red > nd - 1) red = nd - 1;
                if (red < 0) red = 0;
            }
            if (moveCount == 1) {
                sc = -search(nd, -beta, -alpha, pfr + 1, true, m);
            } else {
                sc = -search(nd - red, -alpha - 1, -alpha, pfr + 1, true, m);
                if (!stopped && sc > alpha && red > 0) sc = -search(nd, -alpha - 1, -alpha, pfr + 1, true, m);
                if (!stopped && sc > alpha && sc < beta) sc = -search(nd, -beta, -alpha, pfr + 1, true, m);
            }
            unmake(undo[pfr]);
            if (stopped) return 0;
            if (sc > best) {
                best = sc; bestMove = m;
                if (sc > alpha) {
                    alpha = sc;
                    if (sc >= beta) {
                        i32 bonus = depth * depth * 32;
                        hist[side][m] += bonus;
                        if (w.hmal) {
                            i32 mal = bonus * w.hmal / 100;
                            for (int j = 0; j < i; j++) {
                                hist[side][mv[j]] -= mal;
                                if (hist[side][mv[j]] < -(1 << 22)) hist[side][mv[j]] = -(1 << 22);
                            }
                        }
                        if (hist[side][m] > (1 << 22)) for (size_t q = 0; q < hist[side].size(); q++) hist[side][q] >>= 1;
                        if (killer[pfr][0] != m) { killer[pfr][1] = killer[pfr][0]; killer[pfr][0] = m; }
                        if (w.cmb && prevMove >= 0) cmove[prevMove] = (int16_t)m;
                        break;
                    }
                }
            }
        }
        if (best == -INFSC) best = evalStm();
        uint8_t fl = best <= a0 ? TT_UPPER : (best >= beta ? TT_LOWER : TT_EXACT);
        if (te.flag == TT_NONE || te.key != key || te.depth <= depth) {
            te.key = key; te.score = best; te.move = (int16_t)bestMove; te.depth = (int8_t)depth; te.flag = fl;
        }
        return best;
    }

    std::vector<u64> sortTmp;
    size_t mvStride = 1;
    std::vector<int> mvbuf;
    std::vector<i32> ordbuf, dvbuf;

    void allocBufs() {
        mvStride = (size_t)g.N * 6;
        mvbuf.assign(mvStride * MAXDEPTH, 0);
        ordbuf.assign(mvStride * MAXDEPTH, 0);
        dvbuf.assign(mvStride * MAXDEPTH, 0);
    }

    // ---- root ----

    struct RootMove { int move; i32 score; };
    std::vector<RootMove> rootMoves;

    int redTab[64][64];
    int lastPhase = -1;

    void buildRedTab() {
        for (int d = 0; d < 64; d++) for (int mc = 0; mc < 64; mc++) {
            int r;
            if (w.rlog) {
                double ld = log((double)std::max(1, d));
                double lm = log((double)std::max(1, mc));
                r = (int)(w.rl0 / 100.0 + ld * lm * w.rl1 / 100.0);
            } else {
                r = w.rbase + (mc > w.rm1) + (mc > w.rm2) + (d >= 7 && mc > w.rm3);
            }
            if (r < 0) r = 0;
            redTab[d][mc] = r;
        }
    }

    int think(double ms, u64 nlim, int dlim, int* outDepth = 0, i32* outScore = 0) {
        t0 = std::chrono::steady_clock::now();
        {
            int phase = 256;
            if (w.phl > 0) {
                int left = g.maxply - ply;
                phase = left >= w.phl ? 256 : (left <= 0 ? 0 : 256 * left / w.phl);
                phase = (phase / 32) * 32;   // quantised, so tables change rarely
            }
            if (phase != lastPhase) {
                bool first = lastPhase < 0;
                lastPhase = phase;
                we = w.scaled(phase);
                buildEval();
                recompute();
                if (!first) for (size_t q = 0; q < tt.size(); q++) tt[q].flag = TT_NONE;
            }
            buildRedTab();
        }
        budgetMs = ms;
        nodeLimit = nlim;
        depthLimit = dlim > 0 ? dlim : (w.dlim > 0 ? w.dlim : 64);
        nodes = 0; visits = 0; stopped = false; seldepthReached = 0;
        for (int i = 0; i < MAXDEPTH; i++) { killer[i][0] = killer[i][1] = -1; }
        for (int s = 0; s < 2; s++) for (size_t q = 0; q < hist[s].size(); q++) hist[s][q] >>= 2;

        int tmp[MAXN * 6];
        int n = genMoves(tmp);
        if (n == 0) return -1;
        rootMoves.clear();
        int sgn = side == 0 ? 1 : -1;
        for (int i = 0; i < n; i++) {
            RootMove rm; rm.move = tmp[i]; rm.score = sgn * staticDelta(tmp[i]);
            rootMoves.push_back(rm);
        }
        std::sort(rootMoves.begin(), rootMoves.end(), [](const RootMove& a, const RootMove& b) { return a.score > b.score; });

        int bestMove = rootMoves[0].move;
        i32 bestScore = rootMoves[0].score;
        if (depthLimit >= 1) {
            for (int depth = 1; depth <= depthLimit; depth++) {
                i32 alpha = -INFSC, beta = INFSC;
                if (depth >= 4) { alpha = bestScore - w.asp; beta = bestScore + w.asp; }
                i32 bs;
                int bm;
                int done;
                for (;;) {
                    bs = -INFSC; bm = -1; done = 0;
                    i32 a = alpha;
                    for (size_t i = 0; i < rootMoves.size(); i++) {
                        int m = rootMoves[i].move;
                        make(m, undo[0]);
                        i32 sc;
                        if (i == 0) sc = -search(depth - 1, -beta, -a, 1);
                        else {
                            sc = -search(depth - 1, -a - 1, -a, 1);
                            if (!stopped && sc > a && sc < beta) sc = -search(depth - 1, -beta, -a, 1);
                        }
                        unmake(undo[0]);
                        if (stopped) break;
                        done++;
                        rootMoves[i].score = sc;
                        if (sc > bs) { bs = sc; bm = m; if (sc > a) a = sc; }
                        if (bs >= beta) break;
                    }
                    if (stopped) break;
                    if (bs <= alpha && alpha > -INFSC) { alpha = bs - 2400; beta = (alpha + beta) / 2; continue; }
                    if (bs >= beta && beta < INFSC) { beta = bs + 2400; continue; }
                    break;
                }
                if (stopped) {
                    // a partly finished iteration still gives a usable best move
                    if (done >= 1 && bm >= 0 && bs > alpha) { bestScore = bs; bestMove = bm; }
                    break;
                }
                bestScore = bs; bestMove = bm;
                // keep the best move first, then by score
                std::stable_sort(rootMoves.begin(), rootMoves.end(), [](const RootMove& a, const RootMove& b) { return a.score > b.score; });
                if (outDepth) *outDepth = depth;
                if (outScore) *outScore = bestScore;
                if (verbose) {
                    std::chrono::duration<double, std::milli> el = std::chrono::steady_clock::now() - t0;
                    fprintf(stderr, "depth %2d score %7d nodes %9llu  %6.1f ms  %6.0f knps  best %s\n",
                            depth, (int)bestScore, (unsigned long long)nodes, el.count(),
                            el.count() > 0 ? nodes / el.count() : 0.0, g.moveStr(bestMove).c_str());
                }
                if (bestScore > WINBASE / 2 || bestScore < -WINBASE / 2) break;
                if (!nodeLimit) {
                    std::chrono::duration<double, std::milli> el = std::chrono::steady_clock::now() - t0;
                    if (el.count() > budgetMs * 0.62) break;
                }
            }
        }
        return bestMove;
    }

    // ---- evaluation features, for fitting the weights to game results ----

    // Features for maxH == 5: 15 piece-table entries, then mobility, mobility by
    // height, centrality, captures and tempo.
    static const int NPW = 15;
    static const int NF = NPW + 11;
    static int pwIndex(int h, int ix) { return (h * (h - 1)) / 2 + ix; }
    void features(double* f) const {
        for (int i = 0; i < NF; i++) f[i] = 0;
        for (int c = 0; c < g.N; c++) {
            u16 cd = code[c];
            int h = g.htab[cd];
            if (h == 0 || h > 5) continue;
            double sg0 = (cd & 1) ? -1.0 : 1.0;
            int net = 0;
            for (int ix = 0; ix < h; ix++) {
                int si = ((cd >> ix) & 1) ? -1 : 1;
                net += si;
                f[pwIndex(h, ix)] += si;
            }
            f[NPW + 0] += sg0 * g.deg[c];
            f[NPW + 1] += sg0 * g.deg[c] * (h - 1);
            f[NPW + 2] += sg0 * g.cen[c];
            f[NPW + 3] += net * g.deg[c];
            f[NPW + 4] += net * g.cen[c];
            if (h >= 2 && (net == h || net == -h)) f[NPW + 5 + (h - 2)] += sg0;
        }
        f[NPW + 9] = capW - capB;
        f[NPW + 10] = side == 0 ? 1.0 : -1.0;
    }

    // ---- notation ----

    std::string toCSN() const {
        std::string s;
        for (size_t r = 0; r < g.rowStart.size(); r++) {
            if (r) s += '/';
            for (int k = 0; k < g.rowLen[r]; k++) {
                if (k) s += ',';
                int c = g.rowStart[r] + k;
                u16 cd = code[c];
                int h = g.htab[cd];
                if (h == 0) { s += '-'; continue; }
                for (int i = h - 1; i >= 0; i--) s += ((cd >> i) & 1) ? 'b' : 'w';
            }
        }
        char buf[64];
        snprintf(buf, sizeof buf, " %c %d %d %d", side ? 'b' : 'w', ply, capW, capB);
        s += buf;
        return s;
    }

    bool fromCSN(const std::string& str) {
        std::vector<std::string> f;
        size_t i = 0;
        while (i < str.size()) {
            while (i < str.size() && isspace((unsigned char)str[i])) i++;
            size_t j = i;
            while (j < str.size() && !isspace((unsigned char)str[j])) j++;
            if (j > i) f.push_back(str.substr(i, j - i));
            i = j;
        }
        if (f.size() != 5) return false;
        std::vector<std::string> rows;
        {
            size_t p = 0;
            while (true) {
                size_t q = f[0].find('/', p);
                rows.push_back(f[0].substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        if (rows.size() != g.rowStart.size()) return false;
        u16 nc[MAXN];
        for (int c = 0; c < g.N; c++) nc[c] = 1;
        for (size_t r = 0; r < rows.size(); r++) {
            std::vector<std::string> cells;
            size_t p = 0;
            while (true) {
                size_t q = rows[r].find(',', p);
                cells.push_back(rows[r].substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
            if ((int)cells.size() != g.rowLen[r]) return false;
            for (size_t k = 0; k < cells.size(); k++) {
                int c = g.rowStart[r] + (int)k;
                const std::string& cs = cells[k];
                if (cs == "-") continue;
                if ((int)cs.size() >= g.collapse) return false;
                u16 cd = 1;
                for (size_t t = 0; t < cs.size(); t++) {
                    if (cs[t] == 'w') cd = (u16)(cd << 1);
                    else if (cs[t] == 'b') cd = (u16)((cd << 1) | 1);
                    else return false;
                }
                nc[c] = cd;
            }
        }
        if (f[1] != "w" && f[1] != "b") return false;
        for (int c = 0; c < g.N; c++) code[c] = nc[c];
        side = f[1] == "b" ? 1 : 0;
        ply = atoi(f[2].c_str());
        capW = atoi(f[3].c_str());
        capB = atoi(f[4].c_str());
        if (ply < 0 || capW < 0 || capB < 0) return false;
        if (ply > g.maxply) ply = g.maxply;
        recompute();
        return true;
    }

    u64 perft(int depth) {
        if (depth == 0) return 1;
        if (ply >= g.maxply) return 0;
        int mv[MAXN * 6];
        int n = genMoves(mv);
        if (depth == 1) return (u64)n;
        u64 total = 0;
        Undo u;
        for (int i = 0; i < n; i++) {
            make(mv[i], u);
            total += perft(depth - 1);
            unmake(u);
        }
        return total;
    }
};

// ---------------------------------------------------------------- driver ----

static std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        size_t j = i;
        while (j < s.size() && !isspace((unsigned char)s[j])) j++;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

static const char* ENGINE_NAME = "cascade-ab";

static void protocolLoop(const Weights& w0, double timeFrac, double timeReserve) {
    Engine* e = new Engine();
    e->w = w0;
    e->setRules(5, 6, 150, 0.5);
    e->allocBufs();
    char line[65536];
    while (fgets(line, sizeof line, stdin)) {
        std::vector<std::string> t = tokenize(line);
        if (t.empty()) continue;
        if (t[0] == "cascade") {
            printf("name %s\n", ENGINE_NAME);
            printf("ready\n");
            fflush(stdout);
        } else if (t[0] == "newgame") {
            int side = 5, collapse = 6, maxply = 150;
            double komi = 0.5;
            for (size_t i = 1; i < t.size(); i++) {
                size_t eq = t[i].find('=');
                if (eq == std::string::npos) continue;
                std::string k = t[i].substr(0, eq);
                double v = atof(t[i].c_str() + eq + 1);
                if (k == "side") side = (int)v;
                else if (k == "collapse") collapse = (int)v;
                else if (k == "maxply") maxply = (int)v;
                else if (k == "komi") komi = v;
            }
            if (side < 3) side = 3; if (side > 13) side = 13;
            if (collapse < 3) collapse = 3; if (collapse > 12) collapse = 12;
            if (maxply < 1) maxply = 1;
            e->lastPhase = -1;
            e->setRules(side, collapse, maxply, komi);
            e->allocBufs();
            e->setTTSize(21);
        } else if (t[0] == "position") {
            std::string csn;
            for (size_t i = 1; i < t.size(); i++) { if (i > 1) csn += ' '; csn += t[i]; }
            e->fromCSN(csn);
        } else if (t[0] == "go") {
            double ms = 0; u64 nlim = 0; int dlim = 0;
            for (size_t i = 1; i + 1 < t.size(); i += 2) {
                if (t[i] == "movetime") ms = atof(t[i + 1].c_str());
                else if (t[i] == "nodes") nlim = (u64)atoll(t[i + 1].c_str());
                else if (t[i] == "depth") dlim = atoi(t[i + 1].c_str());
            }
            double budget = 1e18;
            if (ms > 0) {
                budget = ms * timeFrac - timeReserve;
                if (budget < 1) budget = 1;
            } else if (!nlim && !dlim) {
                budget = 200;
            }
            int depth = 0; i32 sc = 0;
            int m = e->think(budget, nlim, dlim, &depth, &sc);
            if (m < 0) {
                int mv[MAXN * 6];
                int n = e->genMoves(mv);
                m = n > 0 ? mv[0] : 0;
            }
            printf("info depth %d score %.3f nodes %llu\n", depth, sc / 1000.0, (unsigned long long)e->nodes);
            printf("bestmove %s\n", e->g.moveStr(m).c_str());
            fflush(stdout);
        } else if (t[0] == "quit") {
            break;
        }
    }
    exit(0);
}

// ---- self-play harness, used only for tuning ----

struct MatchCfg {
    int games = 100;
    double msA = 0, msB = 0;
    u64 nodesA = 60000, nodesB = 60000;
    int threads = 4;
    int seed0 = 1;
    Weights wa, wb;
    int ttbits = 18;
};

struct MatchResult { int aWins = 0, bWins = 0, aWhiteWins = 0, aWhiteGames = 0; };

static void playBatch(const MatchCfg& cfg, int from, int to, MatchResult* res, std::mutex* mu) {
    Engine* ea = new Engine(); ea->w = cfg.wa; ea->setTTSize(cfg.ttbits);
    Engine* eb = new Engine(); eb->w = cfg.wb; eb->setTTSize(cfg.ttbits);
    Engine* ref = new Engine();
    ea->setRules(5, 6, 150, 0.5); ea->allocBufs();
    eb->setRules(5, 6, 150, 0.5); eb->allocBufs();
    ref->setRules(5, 6, 150, 0.5); ref->allocBufs();
    int aw = 0, bw = 0, aww = 0, awg = 0;
    for (int gi = from; gi < to; gi++) {
        int seed = cfg.seed0 + gi / 2;
        bool aIsWhite = (gi % 2) == 0;
        ref->reset((u32)seed);
        ea->setTTSize(cfg.ttbits); eb->setTTSize(cfg.ttbits);
        while (!ref->terminal()) {
            bool isA = ((ref->side == 0) == aIsWhite);
            Engine* mover = isA ? ea : eb;
            double mms = isA ? cfg.msA : cfg.msB;
            u64 mnodes = isA ? cfg.nodesA : cfg.nodesB;
            std::string csn = ref->toCSN();
            mover->fromCSN(csn);
            int m = mover->think(mms > 0 ? mms : 1e18, mms > 0 ? 0 : mnodes, 0);
            if (m < 0 || !ref->isLegal(m)) {
                int mv[MAXN * 6];
                int n = ref->genMoves(mv);
                m = n > 0 ? mv[0] : -1;
                if (m < 0) break;
            }
            Undo u;
            ref->make(m, u);
        }
        i32 sw = ref->exactScoreWhite();
        bool whiteWon = sw > 0;
        if (whiteWon == aIsWhite) aw++; else bw++;
        if (aIsWhite) { awg++; if (whiteWon) aww++; }
    }
    {
        std::lock_guard<std::mutex> lk(*mu);
        res->aWins += aw; res->bWins += bw;
        res->aWhiteWins += aww; res->aWhiteGames += awg;
    }
    delete ea; delete eb; delete ref;
}

static void runMatch(MatchCfg cfg) {
    MatchResult res;
    std::mutex mu;
    // Games are played in chunks so that a clearly decided match can stop early.
    int chunk = cfg.threads * 16;
    for (int done = 0; done < cfg.games; ) {
        int upto = std::min(cfg.games, done + chunk);
        std::vector<std::thread> th;
        int per = (upto - done + cfg.threads - 1) / cfg.threads;
        for (int i = 0; i < cfg.threads; i++) {
            int from = done + i * per, to = std::min(upto, from + per);
            if (from >= to) break;
            th.push_back(std::thread(playBatch, cfg, from, to, &res, &mu));
        }
        for (size_t i = 0; i < th.size(); i++) th[i].join();
        done = upto;
        int t2 = res.aWins + res.bWins;
        if (t2 >= 96 && done < cfg.games) {
            double p2 = (double)res.aWins / t2;
            double z = (p2 - 0.5) / sqrt(0.25 / t2);
            if (z > 3.2 || z < -3.2) break;    // the outcome is already clear
        }
    }
    int tot = res.aWins + res.bWins;
    double p = tot ? (double)res.aWins / tot : 0.5;
    double elo = (p <= 0 || p >= 1) ? (p > 0.5 ? 999 : -999) : -400.0 * log10(1.0 / p - 1.0);
    double se = tot ? sqrt(p * (1 - p) / tot) : 0;
    double eloSe = 400.0 / log(10.0) * (se / std::max(1e-9, p * (1 - p)));
    printf("A %d - B %d  (%d games)  A score %.1f%%  elo %+.0f +/- %.0f  [A as white %d/%d]\n",
           res.aWins, res.bWins, tot, 100 * p, elo, 1.96 * eloSe, res.aWhiteWins, res.aWhiteGames);
    fflush(stdout);
}

// ---- self-play data generation and weight fitting -------------------------

struct GenCfg { int games = 500, threads = 4, seed0 = 1, every = 3, rnd = 4; u64 nodes_ = 400000; Weights w; };

static std::mutex g_out;
static std::vector<std::string> g_rows;

static void genBatch(GenCfg cfg, int from, int to) {
    Engine* e = new Engine(); e->w = cfg.w; e->setTTSize(17);
    e->setRules(5, 6, 150, 0.5); e->allocBufs();
    std::vector<std::string> rows;
    for (int gi = from; gi < to; gi++) {
        e->reset((u32)(cfg.seed0 + gi));
        e->lastPhase = -1;
        u32 a = (u32)(gi * 2654435761u + 99u);
        std::vector<std::string> pend;
        std::vector<int> plies;
        double f[Engine::NF];
        while (!e->terminal()) {
            int m;
            if (e->ply < cfg.rnd) {
                int mv[MAXN * 6];
                int n = e->genMoves(mv);
                a = (u32)(a + 0x9e3779b9u);
                u32 z = a; z = (u32)((z ^ (z >> 16)) * 0x21f0aaadu); z = (u32)((z ^ (z >> 15)) * 0x735a2d97u); z = z ^ (z >> 15);
                m = mv[z % (u32)n];
            } else {
                m = e->think(1e18, cfg.nodes_, 0);
                if (m < 0 || !e->isLegal(m)) { int mv[MAXN * 6]; int n = e->genMoves(mv); if (!n) break; m = mv[0]; }
            }
            if (e->ply >= cfg.rnd && (e->ply % cfg.every) == 0) {
                e->features(f);
                char buf[512];
                int off = 0;
                for (int i = 0; i < Engine::NF; i++) off += snprintf(buf + off, sizeof buf - off, "%.0f ", f[i]);
                pend.push_back(std::string(buf));
                plies.push_back(e->ply);
            }
            Undo u; e->make(m, u);
        }
        int res = e->exactScoreWhite() > 0 ? 1 : 0;
        for (size_t k = 0; k < pend.size(); k++) {
            char tail[64];
            snprintf(tail, sizeof tail, "%d %d", res, plies[k]);
            rows.push_back(pend[k] + tail);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_out);
        for (size_t i = 0; i < rows.size(); i++) g_rows.push_back(rows[i]);
    }
    delete e;
}

static void runGen(GenCfg cfg, const std::string& out) {
    std::vector<std::thread> th;
    int per = (cfg.games + cfg.threads - 1) / cfg.threads;
    for (int i = 0; i < cfg.threads; i++) {
        int from = i * per, to = std::min(cfg.games, from + per);
        if (from >= to) break;
        th.push_back(std::thread(genBatch, cfg, from, to));
    }
    for (size_t i = 0; i < th.size(); i++) th[i].join();
    FILE* fp = fopen(out.c_str(), "w");
    for (size_t i = 0; i < g_rows.size(); i++) fprintf(fp, "%s\n", g_rows[i].c_str());
    fclose(fp);
    printf("wrote %zu positions to %s\n", g_rows.size(), out.c_str());
}

static const char* FNAMES[Engine::NF] = {
    "p1_0",
    "p2_0", "p2_1",
    "p3_0", "p3_1", "p3_2",
    "p4_0", "p4_1", "p4_2", "p4_3",
    "p5_0", "p5_1", "p5_2", "p5_3", "p5_4",
    "mob", "mobh", "pos", "mobA", "posA",
    "pure2", "pure3", "pure4", "pure5", "wcap", "tempo"
};

// Fits the evaluation weights to game results by logistic regression (Texel tuning).
static void runFit(const std::string& file, int plyLo, int plyHi, double l2) {
    FILE* fp = fopen(file.c_str(), "r");
    if (!fp) { printf("cannot open %s\n", file.c_str()); return; }
    const int NF = Engine::NF;
    std::vector<float> X;
    std::vector<float> Y;
    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        double v[NF + 2];
        int k = 0;
        char* p = line;
        while (k < NF + 2) {
            char* end;
            double x = strtod(p, &end);
            if (end == p) break;
            v[k++] = x; p = end;
        }
        if (k < NF + 2) continue;
        int ply = (int)v[NF + 1];
        if (ply < plyLo || ply > plyHi) continue;
        for (int i = 0; i < NF; i++) X.push_back((float)v[i]);
        Y.push_back((float)v[NF]);
    }
    fclose(fp);
    size_t n = Y.size();
    printf("fitting %zu positions (plies %d..%d)\n", n, plyLo, plyHi);
    if (n < 100) return;
    std::vector<double> wv(NF, 0.0), mom(NF + 1, 0.0), vel(NF + 1, 0.0);
    double b = 0.0;
    wv[0] = 0.004;   // a sensible starting scale for the control feature
    double lr = 0.004, beta1 = 0.9, beta2 = 0.999, eps = 1e-8;
    std::vector<double> gr(NF + 1, 0.0);
    for (int it = 1; it <= 3000; it++) {
        for (int i = 0; i <= NF; i++) gr[i] = 0;
        double loss = 0;
        for (size_t r = 0; r < n; r++) {
            const float* x = &X[r * NF];
            double sc = b;
            for (int i = 0; i < NF; i++) sc += wv[i] * x[i];
            double pr = 1.0 / (1.0 + exp(-sc));
            double d = pr - Y[r];
            for (int i = 0; i < NF; i++) gr[i] += d * x[i];
            gr[NF] += d;
            loss += -(Y[r] * log(pr + 1e-12) + (1 - Y[r]) * log(1 - pr + 1e-12));
        }
        for (int i = 0; i <= NF; i++) gr[i] /= (double)n;
        for (int i = 0; i < NF; i++) gr[i] += l2 * wv[i];
        for (int i = 0; i <= NF; i++) {
            mom[i] = beta1 * mom[i] + (1 - beta1) * gr[i];
            vel[i] = beta2 * vel[i] + (1 - beta2) * gr[i] * gr[i];
            double mh = mom[i] / (1 - pow(beta1, it)), vh = vel[i] / (1 - pow(beta2, it));
            double step = lr * mh / (sqrt(vh) + eps);
            if (i < NF) wv[i] -= step; else b -= step;
        }
        if (it % 500 == 0) printf("  iter %4d loss %.5f\n", it, loss / n);
    }
    if (wv[0] <= 0) { printf("degenerate fit (ctrl <= 0)\n"); return; }
    double sc = 1000.0 / wv[0];
    printf("fitted (scaled so ctrl=1000), sigmoid scale %.1f eval units:\n", 1.0 / wv[0] * 1000.0);
    std::string outw;
    for (int i = 0; i < NF; i++) {
        long vi = lround(wv[i] * sc);
        printf("  %-7s %6ld\n", FNAMES[i], vi);
        if (i) { if (!outw.empty()) outw += ","; outw += std::string(FNAMES[i]) + "=" + std::to_string(vi); }
    }
    long bi = lround(b * sc);
    printf("  %-7s %6ld\n", "bias", bi);
    outw += ",bias=" + std::to_string(bi);
    printf("--w \"%s\"\n", outw.c_str());
}

int main(int argc, char** argv) {
    Weights w0;
    double timeFrac = 0.94, timeReserve = 10.0;
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
    if (const char* ew = getenv("CASCADE_W")) w0.parse(ew);

    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--w" && i + 1 < args.size()) { w0.parse(args[i + 1]); i++; }
        else if (args[i] == "--timefrac" && i + 1 < args.size()) { timeFrac = atof(args[i + 1].c_str()); i++; }
        else if (args[i] == "--perft" && i + 1 < args.size()) {
            Engine* e = new Engine(); e->setRules(5, 6, 150, 0.5); e->allocBufs();
            int seed = 1, dmax = atoi(args[i + 1].c_str());
            if (i + 2 < args.size() && args[i + 2][0] != '-') seed = atoi(args[i + 2].c_str());
            e->reset((u32)seed);
            printf("%s\n", e->toCSN().c_str());
            for (int d = 1; d <= dmax; d++) {
                std::chrono::steady_clock::time_point st = std::chrono::steady_clock::now();
                u64 r = e->perft(d);
                std::chrono::duration<double> el = std::chrono::steady_clock::now() - st;
                printf("perft %d = %llu  (%.2fs, %.1f Mnps)\n", d, (unsigned long long)r, el.count(),
                       el.count() > 0 ? r / el.count() / 1e6 : 0.0);
            }
            return 0;
        }
        else if (args[i] == "--bench") {
            Engine* e = new Engine(); e->w = w0; e->setRules(5, 6, 150, 0.5); e->allocBufs();
            double ms = 250;
            int seed = 1;
            if (i + 1 < args.size() && args[i + 1][0] != '-') { ms = atof(args[i + 1].c_str()); i++; }
            if (i + 1 < args.size() && args[i + 1][0] != '-') { seed = atoi(args[i + 1].c_str()); i++; }
            e->verbose = true;
            e->reset((u32)seed);
            int d = 0; i32 sc = 0;
            int mvs = 0;
            while (!e->terminal() && mvs < 150) {
                int m = e->think(ms, 0, 0, &d, &sc);
                fprintf(stderr, "ply %d: %s (depth %d score %.2f)\n", e->ply, e->g.moveStr(m).c_str(), d, sc / 1000.0);
                Undo u; e->make(m, u);
                mvs++;
            }
            printf("final %s  whiteScore %d\n", e->toCSN().c_str(), (int)e->exactScoreWhite());
            return 0;
        }
        else if (args[i] == "--benchpos") {
            Engine* e = new Engine(); e->w = w0; e->setRules(5, 6, 150, 0.5); e->allocBufs();
            double ms = 250; int seed = 1; int upto = 20;
            if (i + 1 < args.size() && args[i + 1][0] != '-') { ms = atof(args[i + 1].c_str()); i++; }
            if (i + 1 < args.size() && args[i + 1][0] != '-') { seed = atoi(args[i + 1].c_str()); i++; }
            if (i + 1 < args.size() && args[i + 1][0] != '-') { upto = atoi(args[i + 1].c_str()); i++; }
            e->reset((u32)seed);
            e->verbose = true;
            double tot = 0; u64 tn = 0; int dsum = 0, cnt = 0;
            for (int k = 0; k < upto && !e->terminal(); k++) {
                int d = 0; i32 sc = 0;
                std::chrono::steady_clock::time_point st = std::chrono::steady_clock::now();
                int m = e->think(ms, 0, 0, &d, &sc);
                std::chrono::duration<double, std::milli> el = std::chrono::steady_clock::now() - st;
                tot += el.count(); tn += e->nodes; dsum += d; cnt++;
                Undo u; e->make(m, u);
            }
            printf("avg depth %.2f, avg ms %.1f, knps %.0f\n", (double)dsum / cnt, tot / cnt, tn / tot);
            return 0;
        }
        else if (args[i] == "--gendata") {
            GenCfg cfg; cfg.w = w0;
            std::string out = "data.txt";
            for (size_t j = i + 1; j < args.size(); j++) {
                size_t eq = args[j].find('=');
                if (eq == std::string::npos) continue;
                std::string k = args[j].substr(0, eq), v = args[j].substr(eq + 1);
                if (k == "games") cfg.games = atoi(v.c_str());
                else if (k == "threads") cfg.threads = atoi(v.c_str());
                else if (k == "nodes") cfg.nodes_ = (u64)atoll(v.c_str());
                else if (k == "seed") cfg.seed0 = atoi(v.c_str());
                else if (k == "every") cfg.every = atoi(v.c_str());
                else if (k == "rnd") cfg.rnd = atoi(v.c_str());
                else if (k == "out") out = v;
            }
            runGen(cfg, out);
            return 0;
        }
        else if (args[i] == "--fit") {
            std::string file = i + 1 < args.size() ? args[i + 1] : "data.txt";
            int lo = 0, hi = 1000; double l2 = 0.0;
            for (size_t j = i + 2; j < args.size(); j++) {
                size_t eq = args[j].find('=');
                if (eq == std::string::npos) continue;
                std::string k = args[j].substr(0, eq), v = args[j].substr(eq + 1);
                if (k == "lo") lo = atoi(v.c_str());
                else if (k == "hi") hi = atoi(v.c_str());
                else if (k == "l2") l2 = atof(v.c_str());
            }
            runFit(file, lo, hi, l2);
            return 0;
        }
        else if (args[i] == "--csn") {
            Engine* e = new Engine(); e->setRules(5, 6, 150, 0.5); e->allocBufs();
            char line[65536];
            int bad = 0, n = 0;
            while (fgets(line, sizeof line, stdin)) {
                std::string in(line);
                while (!in.empty() && (in[in.size() - 1] == '\n' || in[in.size() - 1] == '\r')) in.erase(in.size() - 1);
                if (in.empty()) continue;
                if (!e->fromCSN(in)) { printf("PARSE FAIL: %s\n", in.c_str()); bad++; continue; }
                std::string back = e->toCSN();
                if (back != in) { printf("ROUNDTRIP FAIL\n in:  %s\n out: %s\n", in.c_str(), back.c_str()); bad++; }
                n++;
            }
            printf("%s: %d positions, %d bad\n", bad ? "FAIL" : "OK", n, bad);
            return bad ? 1 : 0;
        }
        else if (args[i] == "--selftest") {
            Engine* e = new Engine(); e->w = w0; e->setRules(5, 6, 150, 0.5); e->allocBufs();
            long bad = 0, checked = 0;
            for (int seed = 1; seed <= 40; seed++) {
                e->reset((u32)seed);
                u32 a = (u32)(seed * 2654435761u + 7u);
                while (!e->terminal()) {
                    int mv[MAXN * 6]; i32 dv[MAXN * 6];
                    int n = 0;
                    e->scan<false>(mv, dv, &n, 0, 0);
                    int n2 = e->genMoves(mv);
                    if (n != n2) { printf("movecount mismatch %d %d\n", n, n2); bad++; }
                    for (int k = 0; k < n; k++) {
                        i32 pre = e->evalAcc;
                        i32 d2 = e->staticDelta(mv[k]);
                        Undo u; e->make(mv[k], u);
                        i32 post = e->evalAcc;
                        i32 keyAfter = 0; (void)keyAfter;
                        u64 kk = e->key;
                        i32 ea = e->evalAcc; int wc = e->wCells, bc = e->bCells;
                        e->recompute();
                        if (e->key != kk || e->evalAcc != ea || e->wCells != wc || e->bCells != bc) {
                            printf("state mismatch at seed %d ply %d move %s\n", seed, e->ply, e->g.moveStr(mv[k]).c_str());
                            bad++;
                        }
                        e->unmake(u);
                        if (e->evalAcc != pre) { printf("unmake eval mismatch\n"); bad++; }
                        if (post - pre != dv[k] || post - pre != d2) {
                            printf("delta mismatch seed %d ply %d %s: scan %d sd %d real %d\n",
                                   seed, e->ply, e->g.moveStr(mv[k]).c_str(), dv[k], d2, post - pre);
                            bad++;
                        }
                        checked++;
                        if (bad > 8) { printf("too many\n"); return 1; }
                    }
                    a = (u32)(a + 0x9e3779b9u);
                    u32 z = a; z = (u32)((z ^ (z >> 16)) * 0x21f0aaadu); z = (u32)((z ^ (z >> 15)) * 0x735a2d97u); z = z ^ (z >> 15);
                    Undo u; e->make(mv[z % (u32)n], u);
                }
            }
            printf("%s: %ld move evaluations checked, %ld bad\n", bad ? "FAIL" : "OK", checked, bad);
            return bad ? 1 : 0;
        }
        else if (args[i] == "--rgame") {
            Engine* e = new Engine(); e->setRules(5, 6, 150, 0.5); e->allocBufs();
            int seed = 1;
            if (i + 1 < args.size() && args[i + 1][0] != '-') { seed = atoi(args[i + 1].c_str()); i++; }
            e->reset((u32)seed);
            u32 a = (u32)(seed * 2654435761u + 12345u);
            while (!e->terminal()) {
                int mv[MAXN * 6];
                int n = e->genMoves(mv);
                a = (u32)(a + 0x9e3779b9u);
                u32 z = a;
                z = (u32)((z ^ (z >> 16)) * 0x21f0aaadu);
                z = (u32)((z ^ (z >> 15)) * 0x735a2d97u);
                z = z ^ (z >> 15);
                int m = mv[z % (u32)n];
                Undo u; e->make(m, u);
                printf("%s %s\n", e->g.moveStr(m).c_str(), e->toCSN().c_str());
            }
            return 0;
        }
        else if (args[i] == "--match") {
            MatchCfg cfg;
            cfg.wa = w0;
            for (size_t j = i + 1; j < args.size(); j++) {
                size_t eq = args[j].find('=');
                if (eq == std::string::npos) continue;
                std::string k = args[j].substr(0, eq), v = args[j].substr(eq + 1);
                if (k == "games") cfg.games = atoi(v.c_str());
                else if (k == "ms") { cfg.msA = cfg.msB = atof(v.c_str()); }
                else if (k == "msA") cfg.msA = atof(v.c_str());
                else if (k == "msB") cfg.msB = atof(v.c_str());
                else if (k == "nodes") { cfg.nodesA = cfg.nodesB = (u64)atoll(v.c_str()); }
                else if (k == "nodesA") cfg.nodesA = (u64)atoll(v.c_str());
                else if (k == "nodesB") cfg.nodesB = (u64)atoll(v.c_str());
                else if (k == "ttbits") cfg.ttbits = atoi(v.c_str());
                else if (k == "threads") cfg.threads = atoi(v.c_str());
                else if (k == "seed") cfg.seed0 = atoi(v.c_str());
                else if (k == "A") cfg.wa.parse(v);
                else if (k == "B") cfg.wb.parse(v);
            }
            runMatch(cfg);
            return 0;
        }
    }
    protocolLoop(w0, timeFrac, timeReserve);
    return 0;
}
