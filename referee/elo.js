'use strict';
// Elo estimation from game records: a Bradley-Terry maximum-likelihood fit with
// bootstrap confidence intervals, plus summary statistics and a crosstable.

const { splitmix32 } = require('../cascade/rules');

// Fits ratings to games [{ white, black, winner }]. Each pairing that played also
// gets one virtual drawn game, which keeps ratings finite for 100% scores.
function fitElo(games, names) {
  const n = names.length, idx = new Map(names.map((x, i) => [x, i]));
  const wins = Array.from({ length: n }, () => new Float64Array(n));
  for (const g of games) {
    const w = idx.get(g.winner === 'white' ? g.white : g.black);
    const l = idx.get(g.winner === 'white' ? g.black : g.white);
    wins[w][l] += 1;
  }
  for (let i = 0; i < n; i++) {
    for (let j = 0; j < n; j++) if (i !== j && wins[i][j] + wins[j][i] > 0) wins[i][j] += 0.5;
  }
  // Minorization-maximization (Hunter 2004).
  let gamma = new Float64Array(n).fill(1);
  for (let iter = 0; iter < 10000; iter++) {
    const next = new Float64Array(n);
    let maxChange = 0;
    for (let i = 0; i < n; i++) {
      let w = 0, denom = 0;
      for (let j = 0; j < n; j++) {
        if (i === j) continue;
        const games_ij = wins[i][j] + wins[j][i];
        if (games_ij === 0) continue;
        w += wins[i][j];
        denom += games_ij / (gamma[i] + gamma[j]);
      }
      next[i] = denom > 0 ? w / denom : gamma[i];
    }
    const logMean = next.reduce((a, x) => a + Math.log(x), 0) / n;
    for (let i = 0; i < n; i++) {
      next[i] /= Math.exp(logMean);
      maxChange = Math.max(maxChange, Math.abs(Math.log(next[i] / gamma[i])));
    }
    gamma = next;
    if (maxChange < 1e-10) break;
  }
  return Array.from(gamma, (x) => 400 * Math.log10(x));
}

// Returns [{ name, elo, lo, hi, games, score }] sorted by Elo, anchored so that the
// anchor engine is 0. lo/hi bound a 95% bootstrap interval.
function ratings(games, anchor, bootstrap = 300) {
  const names = [...new Set(games.flatMap((g) => [g.white, g.black]))];
  if (!names.includes(anchor)) anchor = names[0];
  const a = names.indexOf(anchor);
  const anchored = (elos) => elos.map((x) => x - elos[a]);
  const point = anchored(fitElo(games, names));

  const rng = splitmix32(1);
  const samples = names.map(() => []);
  for (let b = 0; b < bootstrap; b++) {
    const resampled = games.map(() => games[rng() % games.length]);
    anchored(fitElo(resampled, names)).forEach((x, i) => samples[i].push(x));
  }
  return names.map((name, i) => {
    const sorted = samples[i].sort((x, y) => x - y);
    const played = games.filter((g) => g.white === name || g.black === name);
    const won = played.filter((g) => (g.winner === 'white' ? g.white : g.black) === name);
    return {
      name,
      elo: point[i],
      lo: sorted[Math.floor(0.025 * (sorted.length - 1))] ?? point[i],
      hi: sorted[Math.ceil(0.975 * (sorted.length - 1))] ?? point[i],
      games: played.length,
      score: played.length ? won.length / played.length : 0,
    };
  }).sort((x, y) => y.elo - x.elo);
}

function report(games, anchor) {
  const lines = [];
  const table = ratings(games, anchor);
  const w = Math.max(6, ...table.map((r) => r.name.length));
  lines.push(`${'engine'.padEnd(w)}    elo    95% interval   games  score`);
  for (const r of table) {
    const iv = `[${Math.round(r.lo)}, ${Math.round(r.hi)}]`;
    lines.push(`${r.name.padEnd(w)} ${Math.round(r.elo).toString().padStart(6)}  ${iv.padStart(14)}  ${String(r.games).padStart(6)}  ${(100 * r.score).toFixed(1).padStart(5)}%`);
  }

  // Crosstable: row engine's wins against column engine, "wins/games".
  const names = table.map((r) => r.name);
  lines.push('', 'crosstable (row wins / games vs column):');
  lines.push(' '.repeat(w) + names.map((_, j) => ('#' + (j + 1)).padStart(9)).join(''));
  names.forEach((a, i) => {
    let row = `#${i + 1} ${a}`.padEnd(w);
    for (const b of names) {
      if (a === b) { row += '        -'; continue; }
      const vs = games.filter((g) => (g.white === a && g.black === b) || (g.white === b && g.black === a));
      const won = vs.filter((g) => (g.winner === 'white' ? g.white : g.black) === a).length;
      row += (vs.length ? `${won}/${vs.length}` : '').padStart(9);
    }
    lines.push(row);
  });

  const n = games.length;
  const reasons = {};
  for (const g of games) reasons[g.reason] = (reasons[g.reason] || 0) + 1;
  const whiteWins = games.filter((g) => g.winner === 'white').length;
  const meanPlies = games.reduce((a, g) => a + g.plies, 0) / n;
  const scored = games.filter((g) => g.score);
  const raw = scored.map((g) => g.score[0] - g.score[1]); // white minus black, before komi
  const mean = (xs) => xs.reduce((a, x) => a + x, 0) / xs.length;
  lines.push('', `${n} games; white (first player) won ${(100 * whiteWins / n).toFixed(1)}%; ` +
    `mean length ${meanPlies.toFixed(1)} plies; mean |margin| ${mean(scored.map((g, i) => Math.abs(raw[i] - g.rules.komi))).toFixed(2)}; ` +
    `mean white-minus-black score before komi ${mean(raw).toFixed(2)}`);
  lines.push('game endings: ' + Object.entries(reasons).map(([k, v]) => `${k} ${v}`).join(', '));
  return lines.join('\n');
}

module.exports = { fitElo, ratings, report };
