'use strict';
// Prints a game from a games.jsonl file move by move.
//
//   node tools/replay.js <games.jsonl> <line number, 1-based> [--every N]

const fs = require('fs');
const { Cascade } = require('../cascade/rules');

const [file, lineArg, flag, everyArg] = process.argv.slice(2);
if (!file || !lineArg) {
  console.error('usage: node tools/replay.js <games.jsonl> <line> [--every N]');
  process.exit(2);
}
const every = flag === '--every' ? Number(everyArg) : 1;
const record = JSON.parse(fs.readFileSync(file, 'utf8').split('\n').filter(Boolean)[Number(lineArg) - 1]);
const game = new Cascade(record.rules);
let s = game.initialState(record.seed);

console.log(`${record.white} (white) vs ${record.black} (black), seed ${record.seed}\n`);
console.log(game.render(s) + '\n');
record.moves.forEach((mv, i) => {
  const m = game.parseMove(mv);
  if (!game.isLegal(s, m)) { console.log(`${i + 1}. ${mv} (illegal)`); return; }
  s = game.play(s, m);
  if ((i + 1) % every === 0 || i === record.moves.length - 1) {
    console.log(`${i + 1}. ${i % 2 ? 'black' : 'white'} ${mv}`);
    console.log(game.render(s) + '\n');
  }
});
console.log(`${record.winner} wins (${record.reason})`);
