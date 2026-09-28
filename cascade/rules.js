'use strict';
// Cascade — reference rules implementation.
//
// This module is the ground truth for the rules described in RULES.md. The referee
// uses it to validate every move, and the baseline engines use it as their move
// generator. It has no dependencies.
//
// Board geometry uses axial hex coordinates (q, r) with r increasing downward
// (south). Cells are indexed in reading order: top row first, left to right.
//
// A state is an Int32Array laid out as:
//   [0, N)        stack heights
//   [N, 2N)       stack contents as bits, bit i = colour of the i-th piece from the
//                 bottom (0 = white, 1 = black)
//   2N + 0        side to move (0 = white, 1 = black)
//   2N + 1        ply (number of moves played so far)
//   2N + 2        pieces captured by white
//   2N + 3        pieces captured by black

const WHITE = 0;
const BLACK = 1;

// Direction d and its opposite (d + 3) % 6.
const DIR_NAMES = ['E', 'NE', 'NW', 'W', 'SW', 'SE'];
const DIR_DQ = [1, 1, 0, -1, -1, 0];
const DIR_DR = [0, -1, -1, 0, 1, 1];

const DEFAULTS = Object.freeze({ side: 5, collapse: 6, maxply: 150, komi: 0.5 });
const ROW_LETTERS = 'abcdefghijklmnopqrstuvwxyz';

// splitmix32: the PRNG that defines the seeded opening setup.
function splitmix32(seed) {
  let a = seed >>> 0;
  return function next() {
    a = (a + 0x9e3779b9) >>> 0;
    let z = a;
    z = Math.imul(z ^ (z >>> 16), 0x21f0aaad) >>> 0;
    z = Math.imul(z ^ (z >>> 15), 0x735a2d97) >>> 0;
    return (z ^ (z >>> 15)) >>> 0;
  };
}

function popcount(x) {
  x -= (x >>> 1) & 0x55555555;
  x = (x & 0x33333333) + ((x >>> 2) & 0x33333333);
  return (Math.imul((x + (x >>> 4)) & 0x0f0f0f0f, 0x01010101) >>> 24);
}

function validateOptions(o) {
  const isInt = (v) => Number.isInteger(v);
  if (!isInt(o.side) || o.side < 3 || o.side > 13) throw new Error('side must be an integer in [3, 13]');
  if (!isInt(o.collapse) || o.collapse < 3 || o.collapse > 12) throw new Error('collapse must be an integer in [3, 12]');
  if (!isInt(o.maxply) || o.maxply < 1) throw new Error('maxply must be a positive integer');
  if (typeof o.komi !== 'number' || !Number.isFinite(o.komi) || Math.abs(o.komi * 2 % 2) !== 1) {
    throw new Error('komi must be a finite number ending in .5 (so games cannot tie)');
  }
}

class Cascade {
  constructor(opts = {}) {
    const o = { ...DEFAULTS, ...opts };
    validateOptions(o);
    this.options = Object.freeze({ side: o.side, collapse: o.collapse, maxply: o.maxply, komi: o.komi });
    this.side = o.side;
    this.collapse = o.collapse;
    this.maxply = o.maxply;
    this.komi = o.komi;
    this.maxH = o.collapse - 1; // tallest stack that can exist between moves

    const R = o.side - 1;
    const qs = [], rs = [], names = [];
    this.rowStart = [];
    this.rowLength = [];
    const index = new Map();
    for (let r = -R; r <= R; r++) {
      const qmin = Math.max(-R, -R - r), qmax = Math.min(R, R - r);
      this.rowStart.push(qs.length);
      this.rowLength.push(qmax - qmin + 1);
      for (let q = qmin; q <= qmax; q++) {
        index.set(q + ',' + r, qs.length);
        names.push(ROW_LETTERS[r + R] + (q - qmin + 1));
        qs.push(q);
        rs.push(r);
      }
    }
    const N = qs.length;
    this.N = N;
    this.cellQ = qs;
    this.cellR = rs;
    this.cellNames = names;
    this.cellIndex = new Map(names.map((n, i) => [n, i]));

    this.neighbor = new Int16Array(N * 6).fill(-1);
    this.antipode = new Int16Array(N);
    for (let c = 0; c < N; c++) {
      for (let d = 0; d < 6; d++) {
        const k = (qs[c] + DIR_DQ[d]) + ',' + (rs[c] + DIR_DR[d]);
        if (index.has(k)) this.neighbor[c * 6 + d] = index.get(k);
      }
      this.antipode[c] = index.get((-qs[c]) + ',' + (-rs[c]));
    }

    // walk[(c * 6 + d) * maxH + i] is the cell receiving the (i+1)-th sown piece when
    // a stack on c is sown in direction d. A sow of height h uses the first h entries.
    // Sowing reverses direction whenever the next step would leave the board.
    const maxH = this.maxH;
    this.walk = new Int16Array(N * 6 * maxH).fill(-1);
    for (let c = 0; c < N; c++) {
      for (let d0 = 0; d0 < 6; d0++) {
        if (this.neighbor[c * 6 + d0] < 0) continue; // first step off-board: illegal
        let pos = c, d = d0;
        for (let i = 0; i < maxH; i++) {
          if (this.neighbor[pos * 6 + d] < 0) d = (d + 3) % 6;
          pos = this.neighbor[pos * 6 + d];
          this.walk[(c * 6 + d0) * maxH + i] = pos;
        }
      }
    }

    this.SIDE = 2 * N;
    this.PLY = 2 * N + 1;
    this.CAPW = 2 * N + 2;
    this.CAPB = 2 * N + 3;
    this.stateSize = 2 * N + 4;
  }

  // ---- setup ---------------------------------------------------------------

  // Every cell except the centre starts with one piece. Cells come in antipodal pairs
  // (c, -c); each pair holds one white and one black piece, and the seed decides which
  // is which. The position is therefore symmetric under a 180° rotation plus a colour
  // swap. Pairs are visited in cell-index order, one PRNG draw per pair.
  initialState(seed = 0) {
    const N = this.N;
    const s = new Int32Array(this.stateSize);
    const rng = splitmix32(seed);
    for (let c = 0; c < N; c++) {
      const a = this.antipode[c];
      if (a <= c) continue; // centre, or pair already assigned
      const blackHere = rng() & 1;
      s[c] = 1; s[N + c] = blackHere;
      s[a] = 1; s[N + a] = blackHere ^ 1;
    }
    return s;
  }

  // ---- queries -------------------------------------------------------------

  sideToMove(s) { return s[this.SIDE]; }
  ply(s) { return s[this.PLY]; }

  // Colour of the top piece of cell c, or -1 if empty.
  top(s, c) {
    const h = s[c];
    return h === 0 ? -1 : (s[this.N + c] >>> (h - 1)) & 1;
  }

  // The player to move controls at least one stack.
  hasMoves(s) {
    const N = this.N, side = s[this.SIDE];
    for (let c = 0; c < N; c++) {
      const h = s[c];
      if (h > 0 && ((s[N + c] >>> (h - 1)) & 1) === side) return true;
    }
    return false;
  }

  isTerminal(s) {
    return s[this.PLY] >= this.maxply || !this.hasMoves(s);
  }

  // Writes legal moves into buf (an Int32Array or Array) and returns the count.
  // A move is encoded as cell * 6 + direction.
  legalMovesInto(s, buf) {
    if (s[this.PLY] >= this.maxply) return 0;
    const N = this.N, side = s[this.SIDE], nb = this.neighbor;
    let n = 0;
    for (let c = 0; c < N; c++) {
      const h = s[c];
      if (h === 0 || ((s[N + c] >>> (h - 1)) & 1) !== side) continue;
      const base = c * 6;
      for (let d = 0; d < 6; d++) if (nb[base + d] >= 0) buf[n++] = base + d;
    }
    return n;
  }

  legalMoves(s) {
    const buf = new Int32Array(this.N * 6);
    return Array.from(buf.subarray(0, this.legalMovesInto(s, buf)));
  }

  isLegal(s, m) {
    if (!Number.isInteger(m) || m < 0 || m >= this.N * 6) return false;
    if (s[this.PLY] >= this.maxply) return false;
    const c = (m / 6) | 0, h = s[c];
    return h > 0 && ((s[this.N + c] >>> (h - 1)) & 1) === s[this.SIDE] && this.neighbor[m] >= 0;
  }

  // ---- making moves --------------------------------------------------------

  // Applies a legal move to s in place. Returns the number of stacks that collapsed.
  applyInPlace(s, m) {
    const N = this.N, maxH = this.maxH, walk = this.walk, collapse = this.collapse;
    const c = (m / 6) | 0;
    const h = s[c], bits = s[N + c];
    const base = m * maxH;
    const mover = s[this.SIDE];
    s[c] = 0; s[N + c] = 0;
    // Sow bottom piece first; the mover's top piece lands last, on the farthest cell.
    for (let i = 0; i < h; i++) {
      const t = walk[base + i];
      s[N + t] |= ((bits >>> i) & 1) << s[t];
      s[t]++;
    }
    // Every stack that reached the collapse height is removed. The mover captures the
    // opponent's pieces in it; the mover's own pieces in it are simply lost.
    let collapses = 0;
    for (let i = 0; i < h; i++) {
      const t = walk[base + i];
      const ht = s[t];
      if (ht >= collapse) {
        const blacks = popcount(s[N + t]);
        s[mover === WHITE ? this.CAPW : this.CAPB] += mover === WHITE ? blacks : ht - blacks;
        s[t] = 0; s[N + t] = 0;
        collapses++;
      }
    }
    s[this.SIDE] = mover ^ 1;
    s[this.PLY]++;
    return collapses;
  }

  play(s, m) {
    if (!this.isLegal(s, m)) throw new Error('illegal move: ' + this.moveToString(m));
    const t = Int32Array.from(s);
    this.applyInPlace(t, m);
    return t;
  }

  // ---- scoring -------------------------------------------------------------

  // [white stacks, black stacks]: stacks whose top piece is that colour.
  stackCounts(s) {
    const N = this.N;
    let w = 0, b = 0;
    for (let c = 0; c < N; c++) {
      const h = s[c];
      if (h === 0) continue;
      if ((s[N + c] >>> (h - 1)) & 1) b++; else w++;
    }
    return [w, b];
  }

  // [white score, black score], komi not included.
  scores(s) {
    const [w, b] = this.stackCounts(s);
    return [w + s[this.CAPW], b + s[this.CAPB]];
  }

  // White's score minus black's, including komi. Positive means white is ahead.
  margin(s) {
    const [w, b] = this.scores(s);
    return w - b - this.komi;
  }

  // The winner if the game ended now (WHITE or BLACK). Never a tie: komi is x.5.
  winner(s) {
    return this.margin(s) > 0 ? WHITE : BLACK;
  }

  // ---- notation ------------------------------------------------------------

  moveToString(m) {
    return this.cellNames[(m / 6) | 0] + DIR_NAMES[m % 6];
  }

  // Parses e.g. "e5NE". Returns the move code, or -1 if it does not name a
  // (cell, direction) pair on this board. Legality is checked separately.
  parseMove(str) {
    const x = /^([a-z]\d+)(NE|NW|SE|SW|E|W)$/i.exec(String(str).trim());
    if (!x) return -1;
    const c = this.cellIndex.get(x[1].toLowerCase());
    if (c === undefined) return -1;
    return c * 6 + DIR_NAMES.indexOf(x[2].toUpperCase());
  }

  // Cascade State Notation: "<board> <side> <ply> <capW> <capB>". The board lists rows
  // top to bottom separated by '/', and cells left to right separated by ','. Each cell
  // is its stack from bottom to top ('w'/'b'), or '-' if empty.
  toCSN(s) {
    const N = this.N;
    const rows = this.rowStart.map((start, r) => {
      const cells = [];
      for (let c = start; c < start + this.rowLength[r]; c++) {
        let str = '';
        for (let i = 0; i < s[c]; i++) str += ((s[N + c] >>> i) & 1) ? 'b' : 'w';
        cells.push(str || '-');
      }
      return cells.join(',');
    });
    return [rows.join('/'), s[this.SIDE] ? 'b' : 'w', s[this.PLY], s[this.CAPW], s[this.CAPB]].join(' ');
  }

  fromCSN(str) {
    const parts = String(str).trim().split(/\s+/);
    if (parts.length !== 5) throw new Error('CSN needs 5 fields');
    const [board, side, ply, capW, capB] = parts;
    const rows = board.split('/');
    if (rows.length !== this.rowStart.length) throw new Error('CSN has wrong number of rows');
    const N = this.N;
    const s = new Int32Array(this.stateSize);
    rows.forEach((row, r) => {
      const cells = row.split(',');
      if (cells.length !== this.rowLength[r]) throw new Error('CSN row ' + (r + 1) + ' has wrong length');
      cells.forEach((cell, k) => {
        const c = this.rowStart[r] + k;
        if (cell === '-') return;
        if (!/^[wb]+$/.test(cell) || cell.length >= this.collapse) throw new Error('bad stack: ' + cell);
        for (let i = 0; i < cell.length; i++) if (cell[i] === 'b') s[N + c] |= 1 << i;
        s[c] = cell.length;
      });
    });
    if (side !== 'w' && side !== 'b') throw new Error('bad side to move');
    s[this.SIDE] = side === 'b' ? BLACK : WHITE;
    const nums = [ply, capW, capB].map(Number);
    if (!nums.every((v) => Number.isInteger(v) && v >= 0)) throw new Error('bad counters');
    [s[this.PLY], s[this.CAPW], s[this.CAPB]] = nums;
    return s;
  }

  // Human-readable board. Stacks are shown bottom to top; uppercase marks the top piece.
  render(s) {
    const N = this.N, R = this.side - 1;
    const slot = 2 * Math.ceil((this.maxH + 1) / 2); // even, so rows offset by half a slot
    const lines = [];
    this.rowStart.forEach((start, r) => {
      let line = ROW_LETTERS[r] + '  ' + ' '.repeat(Math.abs(r - R) * slot / 2);
      for (let c = start; c < start + this.rowLength[r]; c++) {
        let str = '';
        for (let i = 0; i < s[c]; i++) {
          const ch = ((s[N + c] >>> i) & 1) ? 'b' : 'w';
          str += i === s[c] - 1 ? ch.toUpperCase() : ch;
        }
        line += (str || '.').padEnd(slot);
      }
      lines.push(line.trimEnd());
    });
    const [sw, sb] = this.scores(s);
    lines.push(`ply ${s[this.PLY]}/${this.maxply}, ${s[this.SIDE] ? 'black' : 'white'} to move; ` +
      `white ${sw} (${s[this.CAPW]} captured), black ${sb} (${s[this.CAPB]} captured) + komi ${this.komi}`);
    return lines.join('\n');
  }
}

// Number of move sequences of the given length from s. Used to cross-check
// independent implementations of the rules (see RULES.md).
function perft(game, s, depth) {
  if (depth === 0) return 1;
  const buf = new Int32Array(game.N * 6);
  const n = game.legalMovesInto(s, buf);
  if (depth === 1) return n;
  const t = new Int32Array(game.stateSize);
  let total = 0;
  for (let i = 0; i < n; i++) {
    t.set(s);
    game.applyInPlace(t, buf[i]);
    total += perft(game, t, depth - 1);
  }
  return total;
}

// Parses "key=value" rule options, e.g. from a `newgame` command.
function parseOptions(tokens) {
  const o = {};
  for (const tok of tokens) {
    const [k, v] = tok.split('=');
    if (!(k in DEFAULTS) || v === undefined || v === '' || !Number.isFinite(Number(v))) {
      throw new Error('bad rule option: ' + tok);
    }
    o[k] = Number(v);
  }
  return o;
}

function optionsToString(o) {
  return Object.keys(DEFAULTS).map((k) => k + '=' + o[k]).join(' ');
}

module.exports = {
  Cascade, WHITE, BLACK, DIR_NAMES, DEFAULTS, splitmix32, popcount, perft,
  parseOptions, optionsToString,
};
