'use strict';
// Reference-style opponent: plays the move that maximises the immediate score margin.
const { runEngine } = require('../../engines/lib/protocol');
const { splitmix32 } = require('../../cascade/rules');
const rng = splitmix32((Math.random() * 2 ** 32) >>> 0);
runEngine({
  name: 'greedy',
  think(game, state) {
    const moves = game.legalMoves(state);
    const me = game.sideToMove(state);
    let best = [], bestV = -1e9;
    for (const m of moves) {
      const t = game.play(state, m);
      const [w, b] = game.scores(t);
      const v = me === 0 ? w - b : b - w;
      if (v > bestV) { bestV = v; best = [m]; } else if (v === bestV) best.push(m);
    }
    return best[rng() % best.length];
  },
});
