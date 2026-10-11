// Round-robin runner. usage: node dev/rr.js --mt 100 --pairs 20 --conc 4 --tag name [--go "nodes 50000"] [--gauntlet name] name=cmd ...
const fs = require('fs');
const { spawnSync } = require('child_process');
const args = process.argv.slice(2);
const o = { mt: 100, pairs: 20, conc: 4, tag: 'rr', go: null, gauntlet: null, seed: 1 };
const engines = [];
for (let i = 0; i < args.length; i++) {
  if (args[i].startsWith('--')) o[args[i].slice(2)] = args[++i];
  else { const k = args[i].indexOf('='); engines.push({ name: args[i].slice(0, k), cmd: args[i].slice(k + 1), go: null }); }
}
for (const e of engines) e.go = o.go || `movetime ${o.mt}`;
const cfg = { engines, pairs: Number(o.pairs), concurrency: Number(o.conc), seed: Number(o.seed), out: `results/${o.tag}` };
if (o.gauntlet) cfg.gauntlet = o.gauntlet;
fs.mkdirSync('results', { recursive: true });
fs.rmSync(`results/${o.tag}`, { recursive: true, force: true });
fs.writeFileSync(`results/${o.tag}.json`, JSON.stringify(cfg, null, 1));
const r = spawnSync('node', ['referee/tournament.js', `results/${o.tag}.json`], { encoding: 'utf8', maxBuffer: 1 << 28 });
const out = r.stdout || '';
const lines = out.split('\n');
const start = lines.findIndex((l) => l.startsWith('engine '));
console.log(lines.slice(start).filter((l) => !l.startsWith('game endings')).join('\n'));
