'use strict';
// Checks that a submission is ready to be scored: the manifest is valid, it builds
// from a clean copy of the directory, it completes the handshake, and it plays legal
// moves within the time limit. It then plays a few games against the random engine.
//
//   node tools/check.js <submission-dir> [--games N] [--movetime MS] [--keep]
//
// Conditions (time control, rules, build timeout) come from benchmark.json.
// Exits with status 0 if the submission passes and 1 if it does not.

const fs = require('fs');
const os = require('os');
const path = require('path');
const { performance } = require('perf_hooks');
const { loadManifest, build } = require('../referee/submission');
const { playGame, EngineProcess, moveLimitMs } = require('../referee/match');

const ROOT = path.join(__dirname, '..');
const RANDOM_ENGINE = `node "${path.join(ROOT, 'engines', 'random.js')}"`;
// Played in addition to the benchmark rules when the benchmark uses a hidden variant.
const OTHER_VARIANT = { side: 6, collapse: 5, maxply: 120, komi: 1.5 };

function parseArgs(argv) {
  const opts = { dir: null, games: 2, movetime: null, keep: false };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--games') opts.games = Number(argv[++i]);
    else if (argv[i] === '--movetime') opts.movetime = Number(argv[++i]);
    else if (argv[i] === '--keep') opts.keep = true;
    else opts.dir = argv[i];
  }
  return opts;
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (!opts.dir) {
    console.error('usage: node tools/check.js <submission-dir> [--games N] [--movetime MS] [--keep]');
    process.exit(2);
  }
  const bench = JSON.parse(fs.readFileSync(path.join(ROOT, 'benchmark.json'), 'utf8'));
  const movetime = opts.movetime || bench.movetime;
  const problems = [];
  const fail = (msg) => { problems.push(msg); console.log('  FAIL ' + msg); };

  // 1. Manifest.
  console.log('manifest');
  const { manifest, errors, warnings } = loadManifest(opts.dir);
  warnings.forEach((w) => console.log('  warning: ' + w));
  if (!manifest) {
    errors.forEach(fail);
    return finish(problems);
  }
  console.log(`  ok: "${manifest.name}", cmd: ${manifest.cmd}`);

  // 2. Build a clean copy, so anything outside the directory is unavailable, as it
  //    will be when the submission is scored.
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'cascade-check-'));
  fs.cpSync(opts.dir, work, { recursive: true, filter: (src) => path.basename(src) !== '.git' });
  try {
    console.log(`build (copied to ${work})`);
    const built = await build(work, manifest, 1000 * bench.buildTimeoutSec);
    if (!built.ok) {
      fail('build failed');
      console.log(built.output.trim().split('\n').map((l) => '    ' + l).join('\n'));
      return finish(problems);
    }
    console.log(manifest.build ? `  ok (${(built.ms / 1000).toFixed(1)} s)` : '  no build step');

    // 3. Handshake.
    console.log('handshake');
    const spec = { name: manifest.name, cmd: manifest.cmd, cwd: work, go: `movetime ${movetime}` };
    const probe = new EngineProcess(spec);
    const t0 = performance.now();
    const ready = await probe.handshake();
    await probe.quit();
    if (!ready) {
      fail(probe.exited
        ? 'engine exited before replying "ready" (does cmd work from a copy of the directory, ' +
          'without files outside it? try --keep and run it there)'
        : 'no "ready" within 15 s');
      return finish(problems);
    }
    console.log(`  ok (${Math.round(performance.now() - t0)} ms${probe.name ? ', name ' + probe.name : ''})`);

    // 4. Games against the random engine, alternating colours.
    const variants = [bench.rules];
    if (bench.hiddenVariant) variants.push(OTHER_VARIANT);
    const opponent = { name: 'random', cmd: RANDOM_ENGINE, go: `movetime ${movetime}` };
    const limit = moveLimitMs(spec);
    console.log(`games vs random (movetime ${movetime} ms, forfeit after ${limit} ms)`);
    const times = [];
    let wins = 0, played = 0;
    for (const rules of variants) {
      for (let i = 0; i < opts.games; i++) {
        const white = i % 2 === 0 ? spec : opponent, black = i % 2 === 0 ? opponent : spec;
        const seed = 1 + Math.floor(i / 2);
        const record = await playGame({ rules, seed, white, black });
        const side = white === spec ? 'white' : 'black';
        const mine = record.ms.filter((_, k) => (k % 2 === 0) === (side === 'white'));
        times.push(...mine);
        played++;
        if (record.winner === side) wins++;
        const variant = rules === bench.rules ? '' : ` [variant ${JSON.stringify(rules)}]`;
        const score = record.score ? ` ${record.score[0]}-${record.score[1]}` : '';
        console.log(`  seed ${seed}, as ${side}: ${record.winner === side ? 'won' : 'lost'} ` +
          `(${record.reason}${score}, ${record.plies} plies` +
          `${mine.length ? `, slowest move ${Math.max(...mine)} ms` : ''})${variant}`);
        if (record.reason.endsWith('by ' + side)) { // a forfeit by the submission
          fail(`${record.reason.replace(/ by \w+$/, '')} as ${side}` +
            (record.reason.startsWith('illegal') ? `: "${record.moves[record.moves.length - 1]}"` : ''));
        }
      }
    }
    if (times.length) {
      const sorted = [...times].sort((a, b) => a - b);
      console.log(`  move times: mean ${Math.round(times.reduce((a, x) => a + x, 0) / times.length)} ms, ` +
        `max ${sorted[sorted.length - 1]} ms (limit ${limit} ms)`);
      if (sorted[sorted.length - 1] > movetime) {
        console.log(`  warning: some moves took longer than movetime (${movetime} ms). Only moves over ` +
          `${limit} ms forfeit, but the scoring machine may be slower than this one.`);
      }
    }
    if (wins < played) console.log(`  warning: lost ${played - wins} of ${played} games to a random player`);
    return finish(problems);
  } finally {
    if (opts.keep) console.log(`kept build directory ${work}`);
    else {
      try {
        fs.rmSync(work, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
      } catch (err) {
        console.log(`warning: could not remove ${work}: ${err.code || err.message}`);
      }
    }
  }
}

function finish(problems) {
  console.log(problems.length ? `\nFAIL: ${problems.length} problem(s)` : '\nPASS');
  process.exitCode = problems.length ? 1 : 0;
}

main().catch((err) => { console.error(err); process.exit(1); });
