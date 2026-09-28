'use strict';
// Runs a round-robin or gauntlet tournament between engines and reports Elo.
//
//   node referee/tournament.js <config.json> [--pairs N] [--concurrency N] [--out DIR]
//
// Config:
//   {
//     "rules": { "side": 5, "collapse": 6, "maxply": 150, "komi": 0.5 },
//     "engines": [
//       { "name": "random", "cmd": "node engines/random.js", "go": "movetime 1000" },
//       { "submission": "path/to/dir", "go": "movetime 1000" },   // built from engine.json
//       ...
//     ],
//     "buildTimeoutSec": 600,      // for submissions' build commands
//     "gauntlet": "my-engine",     // optional: only pair this engine against the others
//     "anchor": "random",          // engine fixed at 0 Elo
//     "pairs": 20,                 // game pairs per pairing (same seed, colours swapped)
//     "seed": 1,                   // first opening seed; pair k uses seed + k
//     "concurrency": 4,            // games played at once
//     "out": "results/run"         // games.jsonl and summary.txt are written here
//   }
//
// Games already in <out>/games.jsonl are not replayed, so an interrupted run can be
// resumed by running the same command again.

const fs = require('fs');
const path = require('path');
const { playGame } = require('./match');
const { report } = require('./elo');
const { prepareSubmission } = require('./submission');

function parseArgs(argv) {
  const opts = { file: null };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i].startsWith('--')) opts[argv[i].slice(2)] = argv[++i];
    else opts.file = argv[i];
  }
  return opts;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.file) {
    console.error('usage: node referee/tournament.js <config.json> [--pairs N] [--concurrency N] [--out DIR]');
    process.exit(2);
  }
  const config = JSON.parse(fs.readFileSync(args.file, 'utf8'));
  const pairs = Number(args.pairs ?? config.pairs ?? 10);
  const concurrency = Number(args.concurrency ?? config.concurrency ?? 2);
  const out = args.out ?? config.out ?? 'results/' + path.basename(args.file, '.json');
  const seed0 = Number(config.seed ?? 1);
  const engines = [];
  for (const entry of config.engines) {
    if (!entry.submission) { engines.push(entry); continue; }
    console.log(`building submission ${entry.submission} ...`);
    engines.push(await prepareSubmission(entry, 1000 * Number(config.buildTimeoutSec ?? 600)));
  }
  const pairings = [];
  for (let i = 0; i < engines.length; i++) {
    for (let j = i + 1; j < engines.length; j++) {
      const a = engines[i].name, b = engines[j].name;
      if (!config.gauntlet || a === config.gauntlet || b === config.gauntlet) pairings.push([a, b]);
    }
  }

  fs.mkdirSync(out, { recursive: true });
  const games = await runGames({
    rules: config.rules || {}, engines, pairings, pairs, seed: seed0, concurrency,
    gamesFile: path.join(out, 'games.jsonl'),
  });
  if (!games.length) return;
  const summary = report(games, config.anchor || engines[0].name);
  fs.writeFileSync(path.join(out, 'summary.txt'), summary + '\n');
  console.log('\n' + summary);
}

function readGames(file) {
  return fs.existsSync(file)
    ? fs.readFileSync(file, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l))
    : [];
}

// Plays `pairs` colour-swapped game pairs for every pairing [nameA, nameB], using
// opening seeds seed, seed + 1, .... Games already in gamesFile are kept and not
// replayed; new games are appended to it as they finish. Returns all the games.
async function runGames({ rules, engines, pairings, pairs, seed, concurrency, gamesFile, log = console.log }) {
  const byName = new Map(engines.map((e) => [e.name, e]));
  if (byName.size !== engines.length) throw new Error('engine names must be unique');
  const games = readGames(gamesFile);
  const done = new Set(games.map((g) => `${g.white}|${g.black}|${g.seed}`));

  // Interleave pairings so partial results cover every pairing evenly.
  const jobs = [];
  for (let k = 0; k < pairs; k++) {
    for (const [a, b] of pairings) {
      for (const [w, bl] of [[a, b], [b, a]]) {
        if (!done.has(`${w}|${bl}|${seed + k}`)) jobs.push({ white: w, black: bl, seed: seed + k });
      }
    }
  }

  const total = jobs.length;
  let finished = 0;
  const started = Date.now();
  log(`${pairings.length} pairings x ${pairs} pairs: ${total} games to play (${games.length} already done), ` +
    `concurrency ${concurrency}, writing ${gamesFile}`);

  async function worker() {
    while (jobs.length) {
      const job = jobs.shift();
      const record = await playGame({
        rules, seed: job.seed, white: byName.get(job.white), black: byName.get(job.black),
      });
      games.push(record);
      fs.appendFileSync(gamesFile, JSON.stringify(record) + '\n');
      finished++;
      const eta = ((Date.now() - started) / finished) * (total - finished) / 1000;
      const winner = record.winner === 'white' ? record.white : record.black;
      const score = record.score ? ` ${record.score[0]}-${record.score[1]}` : '';
      log(`[${finished}/${total}] seed ${job.seed} ${record.white} vs ${record.black}: ` +
        `${winner} wins (${record.reason}${score}, ${record.plies} plies) eta ${Math.round(eta)}s`);
    }
  }
  await Promise.all(Array.from({ length: Math.max(1, concurrency) }, worker));
  return games;
}

if (require.main === module) main().catch((err) => { console.error(err); process.exit(1); });

module.exports = { runGames, readGames };
