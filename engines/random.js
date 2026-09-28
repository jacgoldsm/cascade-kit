'use strict';
// Plays a uniformly random legal move. The Elo floor.

const { runEngine } = require('./lib/protocol');
const { splitmix32 } = require('../cascade/rules');

const rng = splitmix32((Math.random() * 2 ** 32) >>> 0);

runEngine({
  name: 'random',
  think(game, state) {
    const moves = game.legalMoves(state);
    return moves[rng() % moves.length];
  },
});
