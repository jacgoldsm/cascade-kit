// Self-play match runner: plays A vs B on paired seeded openings.
#include "../submission/search.hpp"
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <map>
using namespace cz;

struct Cfg { Params p; double softf = 0.5; double ms = 0; long long nodes = 0; int depth = 64; std::string netPath; Net* net = nullptr; };

static bool setParam(Params& p, const std::string& kv) {
  auto eq = kv.find('=');
  std::string k = kv.substr(0, eq);
  int v = atoi(kv.c_str() + eq + 1);
  for (int i = 0; i < P_COUNT; i++) if (k == PNAMES[i]) { p.v[i] = v; return true; }
  fprintf(stderr, "unknown param %s\n", k.c_str()); exit(1);
}
static void parseCfg(Cfg& c, const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
    std::string kv = s.substr(i, j - i);
    if (!kv.empty()) {
      if (kv.rfind("ms=", 0) == 0) c.ms = atof(kv.c_str() + 3);
      else if (kv.rfind("nodes=", 0) == 0) c.nodes = atoll(kv.c_str() + 6);
      else if (kv.rfind("depth=", 0) == 0) c.depth = atoi(kv.c_str() + 6);
      else if (kv.rfind("net=", 0) == 0) c.netPath = kv.substr(4);
      else if (kv.rfind("softf=", 0) == 0) c.softf = atof(kv.c_str() + 6);
      else setParam(c.p, kv);
    }
    i = j + 1;
  }
}

// Returns winner colour (0 white, 1 black).
static int playGame(uint32_t seed, Searcher* w, const Cfg& cw, Searcher* b, const Cfg& cb, int* margin) {
  Pos p; p.initial(seed);
  w->clearTT(); b->clearTT();
  while (!p.terminal()) {
    Searcher* s = p.stm ? b : w;
    const Cfg& c = p.stm ? cb : cw;
    double hard = c.ms > 0 ? c.ms * 0.9 : 1e18;
    double soft = c.ms > 0 ? c.ms * c.softf : 1e18;
    Move m = s->think(p, soft, hard, c.depth, c.nodes);
    p.make(m);
  }
  *margin = p.marginHalf(0);
  return p.marginHalf(0) > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  int pairs = 50, threads = 4; uint32_t seed0 = 1000;
  Cfg A, B; std::string sa, sb, common;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-n") pairs = atoi(argv[++i]);
    else if (a == "-t") threads = atoi(argv[++i]);
    else if (a == "-s") seed0 = atoi(argv[++i]);
    else if (a == "-c") common = argv[++i];
    else if (a == "-A") sa = argv[++i];
    else if (a == "-B") sb = argv[++i];
  }
  parseCfg(A, common); parseCfg(A, sa);
  parseCfg(B, common); parseCfg(B, sb);
  for (Cfg* c : {&A, &B}) if (!c->netPath.empty()) {
    c->net = new Net();
    if (!c->net->load(c->netPath.c_str())) { fprintf(stderr, "cannot load %s\n", c->netPath.c_str()); return 1; }
  }
  std::atomic<int> next(0);
  std::mutex mu;
  int winsA = 0, games = 0, pairWinsA = 0, pairLossA = 0;
  auto worker = [&]() {
    Searcher* sA = new Searcher(20);
    Searcher* sB = new Searcher(20);
    sA->pr = A.p; sB->pr = B.p; sA->net = A.net; sB->net = B.net;
    for (;;) {
      int k = next++;
      if (k >= pairs) break;
      uint32_t seed = seed0 + k * 7919;
      int m1, m2;
      int r1 = playGame(seed, sA, A, sB, B, &m1);  // A white
      int r2 = playGame(seed, sB, B, sA, A, &m2);  // A black
      int a = (r1 == 0) + (r2 == 1);
      std::lock_guard<std::mutex> g(mu);
      winsA += a; games += 2;
      if (a == 2) pairWinsA++; else if (a == 0) pairLossA++;
      if (games % 20 == 0) {
        double sc = (double)winsA / games;
        fprintf(stderr, "%d games: A %.1f%% (%d-%d) pairs +%d -%d\n", games, 100 * sc, winsA, games - winsA, pairWinsA, pairLossA);
      }
    }
    delete sA; delete sB;
  };
  std::vector<std::thread> th;
  for (int i = 0; i < threads; i++) th.emplace_back(worker);
  for (auto& t : th) t.join();
  double sc = (double)winsA / games;
  double elo = -400 * log10(1 / std::min(0.999, std::max(0.001, sc)) - 1);
  double se = sqrt(sc * (1 - sc) / games);
  double eloLo = -400 * log10(1 / std::min(0.999, std::max(0.001, sc - 1.96 * se)) - 1);
  printf("A=[%s] B=[%s] common=[%s]: A scores %d/%d = %.1f%%, Elo %+.0f (+-%.0f) pairs +%d -%d\n", sa.c_str(), sb.c_str(), common.c_str(),
         winsA, games, 100 * sc, elo, elo - eloLo, pairWinsA, pairLossA);
}
