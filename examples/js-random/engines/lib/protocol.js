'use strict';
// Implements the engine side of the Cascade engine protocol (PROTOCOL.md) for
// JavaScript engines. An engine supplies a name and a think() function:
//
//   runEngine({
//     name: 'my-engine',
//     think(game, state, limits) { ...; return move; },  // move code from rules.js
//     newGame(game) { ... },                               // optional
//   });
//
// limits holds whichever of { movetime, nodes, depth } the referee sent, plus
// `deadline`, an absolute performance.now() time when movetime is set, and
// `info(text)`, which sends an `info` line to the referee.

const readline = require('readline');
const { performance } = require('perf_hooks');
const { Cascade, parseOptions } = require('../../cascade/rules');

function runEngine(engine) {
  let game = new Cascade();
  let state = game.initialState(0);
  const out = (line) => process.stdout.write(line + '\n');
  const rl = readline.createInterface({ input: process.stdin, terminal: false });

  rl.on('line', (raw) => {
    const [cmd, ...args] = raw.trim().split(/\s+/);
    try {
      switch (cmd) {
        case 'cascade':
          out('name ' + engine.name);
          out('ready');
          break;
        case 'newgame':
          game = new Cascade(parseOptions(args));
          state = game.initialState(0);
          if (engine.newGame) engine.newGame(game);
          break;
        case 'position':
          state = game.fromCSN(args.join(' '));
          break;
        case 'go': {
          const start = performance.now();
          const limits = { info: (text) => out('info ' + text) };
          for (let i = 0; i + 1 < args.length; i += 2) limits[args[i]] = Number(args[i + 1]);
          if (limits.movetime) limits.deadline = start + limits.movetime;
          const move = engine.think(game, state, limits);
          out('bestmove ' + game.moveToString(move));
          break;
        }
        case 'quit':
          process.exit(0);
          break;
        default:
          break; // unknown commands are ignored
      }
    } catch (err) {
      out('info error ' + String(err && err.message || err).replace(/\s+/g, ' '));
    }
  });
  rl.on('close', () => process.exit(0));
}

module.exports = { runEngine };
