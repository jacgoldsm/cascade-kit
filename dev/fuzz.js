// Legality fuzz: random-ish games; at sampled positions ask the engine for a move and validate it.
const { spawn } = require('child_process');
const readline = require('readline');
const { Cascade, splitmix32 } = require('../cascade/rules');
const bin = process.argv[2], games = Number(process.argv[3] || 10), go = process.argv[4] || 'nodes 3000';
const opts = process.argv[5] ? JSON.parse(process.argv[5]) : {};
const g = new Cascade(opts);
const env = { ...process.env };
const child = spawn('sh', ['-c', bin], { stdio: ['pipe', 'pipe', 'inherit'], env });
const rl = readline.createInterface({ input: child.stdout });
const waiters = [], queue = [];
rl.on('line', (l) => { const w = waiters.shift(); if (w) w(l); else queue.push(l); });
const next = () => new Promise((res) => { if (queue.length) res(queue.shift()); else waiters.push(res); });
(async () => {
  const rng = splitmix32(4242);
  child.stdin.write('cascade 1\n'); await next(); await next();
  const o = g.options;
  child.stdin.write(`newgame side=${o.side} collapse=${o.collapse} maxply=${o.maxply} komi=${o.komi}\n`);
  let asked = 0;
  for (let gi = 0; gi < games; gi++) {
    let s = g.initialState(gi * 104729 + 7);
    while (!g.isTerminal(s)) {
      const moves = g.legalMoves(s);
      if (g.ply(s) % 7 === 3 || moves.length <= 2) {
        child.stdin.write('position ' + g.toCSN(s) + '\n');
        child.stdin.write('go ' + go + '\n');
        let line; do { line = await next(); } while (!line.startsWith('bestmove'));
        const m = g.parseMove(line.split(' ')[1]);
        if (m < 0 || !g.isLegal(s, m)) { console.log('ILLEGAL', line, g.toCSN(s)); process.exit(1); }
        asked++;
      }
      // random move, biased towards moves that create tall stacks / captures
      let m = moves[rng() % moves.length];
      if (rng() % 3 === 0) {
        let bestC = -1;
        for (const mm of moves) { const t = g.play(s, mm); const c = t[g.CAPW] + t[g.CAPB]; if (c > bestC || (c === bestC && rng() % 2)) { bestC = c; m = mm; } }
      }
      s = g.play(s, m);
    }
  }
  console.log('OK', asked, 'positions answered legally');
  child.stdin.write('quit\n'); process.exit(0);
})();
