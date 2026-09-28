# Cascade — Rules

Cascade is a two-player, deterministic, perfect-information game played by sowing
stacks of pieces across a hexagonal board. Games cannot be drawn.

`cascade/rules.js` is the reference implementation. If this document and the code ever
disagree, the code is authoritative and the document has a bug.

## 1. Board

The board is a hexagon of hexagonal cells with **side 5** (61 cells). Rows are
horizontal and lettered `a`–`i` from top to bottom. Cells within a row are numbered
from 1 at the left. Rows have 5, 6, 7, 8, 9, 8, 7, 6, 5 cells. The centre cell is `e5`.

```
a              a1    a2    a3    a4    a5
b           b1    b2    b3    b4    b5    b6
c        c1    c2    c3    c4    c5    c6    c7
d     d1    d2    d3    d4    d5    d6    d7    d8
e  e1    e2    e3    e4    e5    e6    e7    e8    e9
f     f1    f2    f3    f4    f5    f6    f7    f8
g        g1    g2    g3    g4    g5    g6    g7
h           h1    h2    h3    h4    h5    h6
i              i1    i2    i3    i4    i5
```

Each cell has up to six neighbours, in the directions **E, NE, NW, W, SW, SE**. For
example, the neighbours of `e5` are `e6` (E), `d5` (NE), `d4` (NW), `e4` (W), `f4` (SW)
and `f5` (SE).

In axial coordinates `(q, r)`, with `r` increasing downward and the centre at `(0, 0)`,
the six direction vectors are E `(+1, 0)`, NE `(+1, −1)`, NW `(0, −1)`, W `(−1, 0)`,
SW `(−1, +1)` and SE `(0, +1)`. A cell is on the board when `|q|`, `|r|` and `|q + r|` are
all at most 4. Row `r` runs from `q = max(−4, −4 − r)` on the left to
`q = min(4, 4 − r)` on the right.

## 2. Pieces and stacks

There are white pieces and black pieces. Each cell holds a **stack** of zero or more
pieces, ordered from bottom to top. A stack is **controlled** by the player whose
colour is on top. Only the top piece determines control; the pieces underneath matter
because they are sown out when the stack moves.

Between moves every stack is shorter than the **collapse height** (6), so no stack is
ever taller than 5.

## 3. Setup

The centre starts empty. Every other cell starts with one piece. The 60 non-centre
cells form 30 **antipodal pairs** `(q, r)` and `(−q, −r)`; each pair holds one white and
one black piece. The starting position is therefore unchanged by a 180° rotation
combined with swapping the colours, so neither player's pieces start better placed.

Which cell of each pair is white is decided by a 32-bit **seed**, so every game can
start from a different position and opening books are useless. The procedure is
exact, so independent implementations produce the same position:

1. Initialise a splitmix32 generator with the seed (as an unsigned 32-bit integer
   `a`). Each draw does the following, with all arithmetic modulo 2³²:
   ```
   a = a + 0x9E3779B9
   z = a
   z = (z XOR (z >> 16)) * 0x21F0AAAD
   z = (z XOR (z >> 15)) * 0x735A2D97
   return z XOR (z >> 15)
   ```
2. Visit the cells in reading order (`a1, a2, …, a5, b1, …, i5`). For each cell `c`
   whose antipode comes later in reading order, take one draw `x`. If `x` is odd, `c`
   gets a black piece and its antipode a white one; otherwise `c` is white and its
   antipode black.

White moves first.

## 4. Moves

On your turn you must make exactly one move. There is no passing.

1. Choose a stack you control. Say it has height `h`.
2. Choose a direction whose neighbouring cell is on the board.
3. Lift the whole stack off its cell, which becomes empty.
4. **Sow** the lifted pieces one at a time, **bottom piece first**, onto the next `h`
   cells along the chosen direction, one piece per step. Each piece is placed on top
   of whatever is already in the cell it lands on, including empty cells. Because the
   bottom piece goes first, your top piece always lands on the last cell.
5. **Bounce**: if the next step would leave the board, reverse direction and keep
   going. The path can pass back over the starting cell, and a cell can receive more
   than one piece in the same move.
6. **Collapse**: after all pieces are sown, every stack that has reached the collapse
   height (6 or more) is removed from the board. You **capture** each of your
   opponent's pieces in it, and each capture is worth one point to you. Your own
   pieces in a collapsed stack are removed with no score to anyone.

The sowing path is a walk: start at the chosen cell, facing the chosen direction.
For each of the `h` pieces, if the neighbour in the current direction is off the
board, reverse the direction; then step to that neighbour and drop the piece there.
Every line through the board is at least 5 cells long, so a reversed step always
exists.

Example: a white-topped stack on `e7` with pieces (bottom to top) `w b w b w` sown to
the east drops `w` on `e8`, `b` on `e9`, bounces, drops `w` on `e8`, `b` on `e7`, and
`w` on `e6`.

## 5. End of the game and scoring

The game ends when either:

- the player to move controls no stack, and so has no legal move; or
- 150 plies (moves by either player) have been played.

Each player's **score** is the number of stacks they control plus the number of pieces
they have captured. Black, who moves second, also receives a **komi** of 0.5 points.
The player with the higher score wins. The half-point komi means a game can never be
tied or drawn.

## 6. Variants

The benchmark may use a different variant, announced when the game starts. The
parameters are:

| Parameter  | Default | Meaning                                           |
|------------|---------|---------------------------------------------------|
| `side`     | 5       | Cells per board edge (3–13). The board has `3·side·(side−1)+1` cells. |
| `collapse` | 6       | Height at which a stack collapses (3–12).         |
| `maxply`   | 150     | Plies after which the game ends.                  |
| `komi`     | 0.5     | Points added to black's score. Must end in `.5`.  |

Everything else generalises directly: rows are lettered from `a`, the centre starts
empty, and the stack limit between moves is `collapse − 1`.

## 7. Notation

**Cells** are a row letter and a cell number, e.g. `e5`.

**Moves** are the starting cell followed by the direction, e.g. `e5NE`, `c3E`,
`i2W`. Parsers should accept any letter case; writers should use the case shown.

**Positions** use Cascade State Notation (CSN), five space-separated fields:

```
<board> <side to move> <ply> <white captures> <black captures>
```

The board lists rows from top to bottom separated by `/`, and the cells within a row
from left to right separated by `,`. Each cell is its stack from bottom to top, using
`w` and `b`, or `-` if empty. The side to move is `w` or `b`. The seed-1 opening is:

```
w,w,b,b,w/w,w,b,b,w,b/b,w,w,b,w,b,w/w,w,b,b,b,b,w,b/b,w,w,w,-,b,b,b,w/w,b,w,w,w,w,b,b/b,w,b,w,b,b,w/w,b,w,w,b,b/b,w,w,b,b w 0 0 0
```

## 8. Checking an implementation

`perft(n)` counts the distinct sequences of `n` legal moves from a position. A
finished game has no legal moves. From the seed-1 opening:

| n | perft(n)  |
|---|-----------|
| 1 | 153       |
| 2 | 23,010    |
| 3 | 3,400,084 |

`node --test test/rules.test.js` also covers bounces, double drops, collapses,
the end of the game and notation.
