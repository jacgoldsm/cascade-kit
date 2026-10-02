// Self-play data generator: writes every position with the game's final margin.
#include "../submission/search.hpp"
#include <thread>
#include <mutex>
#include <atomic>
using namespace cz;

#pragma pack(push, 1)
struct Rec { uint8_t c[N]; uint8_t stm, ply, cap0, cap1; int16_t score; int8_t finalHalf; uint8_t pad; };
#pragma pack(pop)

int main(int argc, char** argv) {
  int games = 100, threads = 4; uint32_t seed0 = 1; long long nodes = 20000; double ms = 0;
  const char* out = "data.bin"; int randomPlies = 0;
  Params pr;
  std::string netPath;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-g") games = atoi(argv[++i]);
    else if (a == "-t") threads = atoi(argv[++i]);
    else if (a == "-s") seed0 = atoi(argv[++i]);
    else if (a == "-nodes") nodes = atoll(argv[++i]);
    else if (a == "-ms") ms = atof(argv[++i]);
    else if (a == "-o") out = argv[++i];
    else if (a == "-r") randomPlies = atoi(argv[++i]);
    else if (a == "-net") netPath = argv[++i];
    else if (a == "-p") {
      std::string kv = argv[++i]; auto eq = kv.find('=');
      for (int k = 0; k < P_COUNT; k++) if (kv.substr(0, eq) == PNAMES[k]) pr.v[k] = atoi(kv.c_str() + eq + 1);
    }
  }
  Net* net = nullptr;
  if (!netPath.empty()) { net = new Net(); if (!net->load(netPath.c_str())) { fprintf(stderr, "bad net\n"); return 1; } }
  FILE* f = fopen(out, "wb");
  std::mutex mu; std::atomic<int> next(0); std::atomic<long> npos(0);
  auto worker = [&]() {
    Searcher* s = new Searcher(20);
    s->pr = pr;
    s->net = net;
    std::vector<Rec> recs;
    for (;;) {
      int g = next++;
      if (g >= games) break;
      uint32_t seed = seed0 + g * 2654435761u;
      uint64_t rng = seed * 0x9E3779B97F4A7C15ULL + 1;
      Pos p; p.initial(seed);
      s->clearTT();
      recs.clear();
      while (!p.terminal()) {
        Move m;
        if (p.ply < randomPlies) {
          Move ms_[400]; int n = p.genMoves(ms_);
          rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
          m = ms_[rng % n];
          s->rootScore = 0;
        } else {
          m = s->think(p, ms > 0 ? ms * 0.5 : 1e18, ms > 0 ? ms * 0.9 : 1e18, 64, ms > 0 ? 0 : nodes);
        }
        Rec r; memcpy(r.c, p.c, N); r.stm = p.stm; r.ply = p.ply; r.cap0 = p.cap[0]; r.cap1 = p.cap[1];
        r.score = (int16_t)std::max(-32000, std::min(32000, s->rootScore)); r.pad = 0;
        if (p.ply >= randomPlies) recs.push_back(r);
        p.make(m);
      }
      int fh = p.marginHalf(0);
      for (auto& r : recs) r.finalHalf = (int8_t)fh;
      std::lock_guard<std::mutex> lk(mu);
      fwrite(recs.data(), sizeof(Rec), recs.size(), f);
      npos += recs.size();
      if (g % 100 == 0) fprintf(stderr, "game %d positions %ld\n", g, (long)npos);
    }
    delete s;
  };
  std::vector<std::thread> th;
  for (int i = 0; i < threads; i++) th.emplace_back(worker);
  for (auto& t : th) t.join();
  fclose(f);
  fprintf(stderr, "done: %ld positions\n", (long)npos);
}
