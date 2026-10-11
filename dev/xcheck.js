// Cross-check the C++ engine's move application against cascade/rules.js on random games.
const { spawn } = require('child_process');
const readline = require('readline');
const { Cascade, splitmix32 } = require('../cascade/rules');
const bin = process.argv[2];
const games = Number(process.argv[3] || 30);
const opts = process.argv[4] ? JSON.parse(process.argv[4]) : {};
const g = new Cascade(opts);
const child = spawn(bin, [], { stdio: ['pipe', 'pipe', 'inherit'] });
const rl = readline.createInterface({ input: child.stdout });
const waiters = [], queue = [];
rl.on('line', (l) => { const w = waiters.shift(); if (w) w(l); else queue.push(l); });
const next = () => new Promise((res) => { if (queue.length) res(queue.shift()); else waiters.push(res); });
const ask = (cmd) => { child.stdin.write(cmd + '\n'); return next(); };
(async () => {
  const rng = splitmix32(777);
  await ask('cascade 1'); // name line
  await next(); // ready
  const o = g.options;
  child.stdin.write(`newgame side=${o.side} collapse=${o.collapse} maxply=${o.maxply} komi=${o.komi}\n`);
  let checks = 0, collapses = 0;
  for (let gi = 0; gi < games; gi++) {
    let s = g.initialState(gi * 7919 + 1);
    while (!g.isTerminal(s)) {
      child.stdin.write('position ' + g.toCSN(s) + '\n');
      const moves = g.legalMoves(s);
      const picks = Math.min(moves.length, 6);
      for (let k = 0; k < picks; k++) {
        const m = moves[rng() % moves.length];
        const reply = await ask('try ' + g.moveToString(m));
        const t = g.play(s, m);
        const exp = g.toCSN(t);
        if (reply !== 'ok ' + exp) { console.log('MISMATCH\n got ', reply, '\n want', 'ok ' + exp, '\n from', g.toCSN(s), g.moveToString(m)); process.exit(1); }
        checks++;
      }
      let m = moves[rng() % moves.length];
      const before = s[g.CAPW] + s[g.CAPB];
      if (rng() % 10 < 8) {
        const caps = moves.filter((mm) => { const t2 = g.play(s, mm); return t2[g.CAPW] + t2[g.CAPB] > before; });
        if (caps.length) m = caps[rng() % caps.length];
      }
      s = g.play(s, m);
      if (s[g.CAPW] + s[g.CAPB] > before) collapses++;
    }
  }
  console.log('OK', checks, 'moves checked,', collapses, 'collapse moves in the games');
  child.stdin.write('quit\n');
  process.exit(0);
})();
