'use strict';
// Fallback engine, used only if the C++ engine fails to build.  Plays a two-ply
// minimax on a simple evaluation, speaking the protocol directly.
const readline = require('readline');
const { Cascade, parseOptions } = require('./rules');

let game = new Cascade();
let state = game.initialState(0);

function evaluate(g, s) {
  const N = g.N;
  let v = 0;
  for (let c = 0; c < N; c++) {
    const h = s[c];
    if (h === 0) continue;
    const bits = s[N + c];
    let blacks = 0;
    for (let i = 0; i < h; i++) blacks += (bits >>> i) & 1;
    const whites = h - blacks;
    const sg = ((bits >>> (h - 1)) & 1) ? -1 : 1;
    const deg = (() => { let d = 0; for (let k = 0; k < 6; k++) if (g.neighbor[c * 6 + k] >= 0) d++; return d; })();
    const hv = [0, 0, 150, 60, -120, -220];
    v += sg * (1000 + 25 * deg) + (hv[Math.min(h, 5)] || -250) * (whites - blacks);
  }
  v += 1150 * (s[g.CAPW] - s[g.CAPB]) - Math.round(g.komi * 1000);
  return v;
}

function think(g, s, limits) {
  const deadline = limits.deadline || (Date.now() + 100);
  const moves = g.legalMoves(s);
  const me = g.sideToMove(s);
  const sgn = me === 0 ? 1 : -1;
  let best = moves[0], bestV = -Infinity;
  for (const m of moves) {
    const t = g.play(s, m);
    let v;
    if (g.isTerminal(t)) {
      v = sgn * (g.margin(t) > 0 ? 1e7 : -1e7);
    } else {
      const reply = g.legalMoves(t);
      let worst = Infinity;
      for (const r of reply) {
        const u = g.play(t, r);
        const vv = sgn * evaluate(g, u);
        if (vv < worst) worst = vv;
        if (Date.now() > deadline - 5) break;
      }
      v = worst === Infinity ? sgn * evaluate(g, t) : worst;
    }
    if (v > bestV) { bestV = v; best = m; }
    if (Date.now() > deadline - 10) break;
  }
  return best;
}

const out = (l) => process.stdout.write(l + '\n');
readline.createInterface({ input: process.stdin, terminal: false }).on('line', (raw) => {
  const [cmd, ...args] = raw.trim().split(/\s+/);
  try {
    if (cmd === 'cascade') { out('name cascade-ab'); out('ready'); }
    else if (cmd === 'newgame') { game = new Cascade(parseOptions(args)); state = game.initialState(0); }
    else if (cmd === 'position') { state = game.fromCSN(args.join(' ')); }
    else if (cmd === 'go') {
      const limits = {};
      for (let i = 0; i + 1 < args.length; i += 2) limits[args[i]] = Number(args[i + 1]);
      if (limits.movetime) limits.deadline = Date.now() + limits.movetime * 0.8;
      out('bestmove ' + game.moveToString(think(game, state, limits)));
    } else if (cmd === 'quit') process.exit(0);
  } catch (err) {
    try { out('bestmove ' + game.moveToString(game.legalMoves(state)[0])); } catch (e) {}
  }
}).on('close', () => process.exit(0));
