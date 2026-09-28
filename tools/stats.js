'use strict';
// Plays uniformly random games and reports how the rules behave: game length, how
// games end, branching factor, stack heights, collapses and first-player results.
//
//   node tools/stats.js [games=2000] [key=value rule options...]

const { Cascade, parseOptions, splitmix32 } = require('../cascade/rules');

const args = process.argv.slice(2);
const games = args[0] && !args[0].includes('=') ? Number(args.shift()) : 2000;
const game = new Cascade(parseOptions(args));
const rng = splitmix32(12345);
const buf = new Int32Array(game.N * 6);

let plies = 0, cappedGames = 0, whiteWins = 0, collapses = 0, captured = 0;
let branchSum = 0, branchN = 0, marginSum = 0;
const heightHist = new Array(game.collapse).fill(0);
const start = Date.now();

for (let g = 0; g < games; g++) {
  const s = game.initialState(g);
  for (;;) {
    const n = game.legalMovesInto(s, buf);
    if (n === 0) break;
    branchSum += n; branchN++;
    collapses += game.applyInPlace(s, buf[rng() % n]);
  }
  for (let c = 0; c < game.N; c++) heightHist[s[c]]++;
  plies += game.ply(s);
  if (game.ply(s) >= game.maxply) cappedGames++;
  if (game.winner(s) === 0) whiteWins++;
  captured += s[game.CAPW] + s[game.CAPB];
  marginSum += Math.abs(game.margin(s));
}

const pct = (x) => (100 * x / games).toFixed(1) + '%';
console.log('rules:', game.options);
console.log(`${games} random games in ${Date.now() - start} ms`);
console.log('mean length (plies):     ', (plies / games).toFixed(1));
console.log('ended by move cap:       ', pct(cappedGames));
console.log('white (first) wins:      ', pct(whiteWins));
console.log('mean branching factor:   ', (branchSum / branchN).toFixed(1));
console.log('collapses per game:      ', (collapses / games).toFixed(2));
console.log('pieces captured per game:', (captured / games).toFixed(2));
console.log('mean |final margin|:     ', (marginSum / games).toFixed(2));
console.log('final stack heights (0 = empty):',
  heightHist.map((v, h) => `${h}:${(v / games).toFixed(1)}`).join(' '));
