'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { Cascade, perft, splitmix32, WHITE, BLACK } = require('../cascade/rules');

const game = new Cascade();

// Builds a state from { cellName: 'stack bottom-to-top' }.
function position(stacks, side = WHITE, g = game) {
  const s = new Int32Array(g.stateSize);
  for (const [name, stack] of Object.entries(stacks)) {
    const c = g.cellIndex.get(name);
    s[c] = stack.length;
    for (let i = 0; i < stack.length; i++) if (stack[i] === 'b') s[g.N + c] |= 1 << i;
  }
  s[g.SIDE] = side;
  return s;
}
const stackAt = (s, name) => {
  const c = game.cellIndex.get(name);
  let str = '';
  for (let i = 0; i < s[c]; i++) str += ((s[game.N + c] >>> i) & 1) ? 'b' : 'w';
  return str;
};
const move = (str) => game.parseMove(str);

test('board geometry', () => {
  assert.equal(game.N, 61);
  assert.deepEqual(game.rowLength, [5, 6, 7, 8, 9, 8, 7, 6, 5]);
  assert.equal(game.cellNames[0], 'a1');
  assert.equal(game.cellNames[30], 'e5');
  for (let c = 0; c < game.N; c++) {
    for (let d = 0; d < 6; d++) {
      const n = game.neighbor[c * 6 + d];
      if (n >= 0) assert.equal(game.neighbor[n * 6 + (d + 3) % 6], c);
    }
  }
  // Corner a1 has three neighbours: E, SW, SE.
  assert.deepEqual(['E', 'NE', 'NW', 'W', 'SW', 'SE'].filter((_, d) => game.neighbor[d] >= 0), ['E', 'SW', 'SE']);
});

test('opening setup is symmetric under rotation plus colour swap', () => {
  for (const seed of [0, 1, 2, 99, 123456]) {
    const s = game.initialState(seed);
    assert.equal(s[game.cellIndex.get('e5')], 0);
    const [w, b] = game.stackCounts(s);
    assert.equal(w, 30);
    assert.equal(b, 30);
    for (let c = 0; c < game.N; c++) {
      if (c === 30) continue;
      assert.equal(s[c], 1);
      assert.equal(game.top(s, c), 1 - game.top(s, game.antipode[c]));
    }
  }
  assert.notEqual(game.toCSN(game.initialState(1)), game.toCSN(game.initialState(2)));
});

test('seed 1 opening matches the documented position', () => {
  assert.equal(game.toCSN(game.initialState(1)),
    'w,w,b,b,w/w,w,b,b,w,b/b,w,w,b,w,b,w/w,w,b,b,b,b,w,b/b,w,w,w,-,b,b,b,w/w,b,w,w,w,w,b,b/b,w,b,w,b,b,w/w,b,w,w,b,b/b,w,w,b,b w 0 0 0');
});

test('perft from the seed 1 opening', () => {
  const s = game.initialState(1);
  assert.equal(perft(game, s, 1), 153);
  assert.equal(perft(game, s, 2), 23010);
  assert.equal(perft(game, s, 3), 3400084);
});

test('sowing drops the bottom piece first and the top piece farthest', () => {
  const s = position({ c3: 'bbw', c4: 'w' });
  const t = game.play(s, move('c3E'));
  assert.equal(stackAt(t, 'c3'), '');
  assert.equal(stackAt(t, 'c4'), 'wb');
  assert.equal(stackAt(t, 'c5'), 'b');
  assert.equal(stackAt(t, 'c6'), 'w');
  assert.equal(game.sideToMove(t), BLACK);
  assert.equal(game.ply(t), 1);
});

test('sowing bounces off the edge and can pass back over the origin', () => {
  // e7 has height 5 moving east: e8, e9, bounce, e8, e7, e6.
  const s = position({ e7: 'wbwbw' });
  const t = game.play(s, move('e7E'));
  assert.equal(stackAt(t, 'e8'), 'ww');
  assert.equal(stackAt(t, 'e9'), 'b');
  assert.equal(stackAt(t, 'e7'), 'b');
  assert.equal(stackAt(t, 'e6'), 'w');
});

test('moves whose first step leaves the board are illegal', () => {
  const s = position({ e9: 'w' });
  assert.equal(game.isLegal(s, move('e9E')), false);
  assert.equal(game.isLegal(s, move('e9W')), true);
  assert.deepEqual(game.legalMoves(s).map((m) => game.moveToString(m)).sort(), ['e9NW', 'e9SW', 'e9W']);
});

test('only stacks topped by the side to move can move', () => {
  const s = position({ e5: 'wb', e6: 'bw' });
  assert.deepEqual([...new Set(game.legalMoves(s).map((m) => game.cellNames[(m / 6) | 0]))], ['e6']);
});

test('collapse: the mover captures opponent pieces and loses their own', () => {
  const s = position({ e4: 'w', e5: 'wbbbb', a1: 'b' });
  const t = game.play(s, move('e4E'));
  assert.equal(stackAt(t, 'e5'), '');
  assert.equal(t[game.CAPW], 4);
  assert.equal(t[game.CAPB], 0);
  assert.deepEqual(game.scores(t), [4, 1]);
});

test('collapse by two drops on the same cell in one move', () => {
  // e7 moving east with height 5 drops its 1st and 3rd pieces on e8 (4 + 2 = 6 = collapse).
  const s = position({ e7: 'bbbbw', e8: 'bbbb' }, WHITE);
  const t = game.play(s, move('e7E'));
  assert.equal(stackAt(t, 'e8'), '');
  assert.equal(stackAt(t, 'e9'), 'b');
  assert.equal(t[game.CAPW], 6); // e8's four black pieces plus the two sown onto it
});

test('the game ends when the side to move controls no stack, or at the move cap', () => {
  const s = position({ e5: 'wb' }, WHITE);
  assert.equal(game.isTerminal(s), true);
  assert.equal(game.winner(s), BLACK);
  const short = new Cascade({ maxply: 2 });
  let t = short.initialState(5);
  t = short.play(t, short.legalMoves(t)[0]);
  t = short.play(t, short.legalMoves(t)[0]);
  assert.equal(short.isTerminal(t), true);
  assert.deepEqual(short.legalMoves(t), []);
});

test('komi decides equal scores in favour of black', () => {
  const s = position({ e5: 'w', e6: 'b' });
  assert.equal(game.margin(s), -0.5);
  assert.equal(game.winner(s), BLACK);
  assert.throws(() => new Cascade({ komi: 1 }));
});

test('CSN and move notation round-trip', () => {
  const rng = splitmix32(7);
  let s = game.initialState(3);
  for (let i = 0; i < 60 && !game.isTerminal(s); i++) {
    const moves = game.legalMoves(s);
    const m = moves[rng() % moves.length];
    assert.equal(game.parseMove(game.moveToString(m)), m);
    s = game.play(s, m);
    assert.deepEqual(game.fromCSN(game.toCSN(s)), s);
  }
  assert.equal(game.parseMove('z9E'), -1);
  assert.equal(game.parseMove('e5N'), -1);
  assert.throws(() => game.fromCSN('garbage'));
});
