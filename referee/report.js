'use strict';
// Prints the Elo report for one or more games.jsonl files, e.g. to merge runs.
//
//   node referee/report.js <games.jsonl>... [--anchor NAME]

const fs = require('fs');
const { report } = require('./elo');

const argv = process.argv.slice(2);
const a = argv.indexOf('--anchor');
const anchor = a >= 0 ? argv.splice(a, 2)[1] : 'random';
const games = argv.flatMap((f) => fs.readFileSync(f, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l)));
if (!games.length) {
  console.error('usage: node referee/report.js <games.jsonl>... [--anchor NAME]');
  process.exit(2);
}
console.log(report(games, anchor));
