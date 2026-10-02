// Cascade engine: protocol front end.
#include "search.hpp"
#include <iostream>
#include <sstream>

using namespace cz;

int main() {
  std::ios::sync_with_stdio(false);
  T();
  Searcher* s = new Searcher(22);
  s->clearTT();  // touch the table's pages now, not during the first move
  static Net net;
  if (net.load("nn.bin")) s->net = &net;
  Pos pos;
  pos.initial(0);
  bool defaultRules = true;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "cascade") {
      std::cout << "name claude-cascade-nn\nready" << std::endl;
    } else if (cmd == "newgame") {
      std::string tok;
      defaultRules = true;
      while (in >> tok) {
        if (tok != "side=5" && tok != "collapse=6" && tok != "maxply=150" && tok != "komi=0.5") defaultRules = false;
      }
    } else if (cmd == "position") {
      std::string rest;
      std::getline(in, rest);
      pos.fromCSN(rest);
    } else if (cmd == "go") {
      std::string k;
      long long v = 0;
      double movetime = 0;
      int depth = 64;
      long long nodes = 0;
      while (in >> k >> v) {
        if (k == "movetime") movetime = (double)v;
        else if (k == "depth") depth = (int)v;
        else if (k == "nodes") nodes = v;
      }
      double hard = 1e18, soft = 1e18;
      if (movetime > 0) {
        hard = std::max(1.0, std::min(movetime * 0.94, movetime - 15));
        soft = hard;
      }
      Move m = s->think(pos, soft, hard, depth, nodes);
      std::cout << "info depth " << s->depthDone << " score " << s->rootScore << " nodes " << s->nodes
                << " time " << (int)s->elapsedMs() << "\n";
      if (m < 0) {
        Move ms[400];
        int n = pos.genMoves(ms);
        m = n ? ms[0] : 0;
      }
      std::cout << "bestmove " << pos.moveStr(m) << std::endl;
    } else if (cmd == "quit") {
      break;
    }
  }
  return 0;
}
