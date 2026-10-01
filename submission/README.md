# cascade-ab

A Cascade engine: alpha-beta search over an incrementally updated evaluation.

## Files

| File | Contents |
|---|---|
| `engine.cpp` | The whole engine: rules, evaluation, search, protocol, plus the self-test, self-play and weight-fitting modes used to develop it. |
| `build.sh` | Compiles `engine.cpp`, falling back to plainer compiler flags and finally to the bundled binary / JavaScript engine. |
| `run.sh` | Starts `./engine`, or `fallback.js` if no binary is available. |
| `fallback.js` | A two-ply JavaScript engine, used only if the C++ build fails. |
| `rules.js` | The kit's reference rules, needed by `fallback.js`. |

## Board representation

A cell's stack is one integer: start at 1 and push a piece of colour `x` with
`code = code * 2 + x`. The code is therefore a sentinel bit followed by the
stack's colours from bottom to top, so

* height = `bit_width(code) - 1`,
* top colour = `code & 1`,
* black pieces = `popcount(code) - 1`.

Sowing walks are precomputed per (cell, direction), including the bounce off the
edge. For each (move, height) a small program lists the *distinct* cells the sow
touches and which pieces land on each, so a move's effect — including a collapse
and its captures — is found without replaying the walk.

## Evaluation

The evaluation is a sum of independent per-cell terms plus captures and a tempo
bonus. A cell contributes

* `p[h][i]` for the piece `i` places below the top of a height-`h` stack, signed
  by its colour,
* a bonus for the controller scaled by the cell's degree (which is exactly the
  mobility the cell provides) and by its distance from the edge,
* a bonus when every piece in the stack is one colour.

Because the terms are independent, the evaluation is kept incrementally, and the
change caused by *any* move is a handful of table lookups. Depth-1 nodes are
therefore a scan with no moves made, and move ordering uses the exact change in
the evaluation rather than a guess.

Cells enter the evaluation only through (degree, distance from edge), so the
tables are indexed by cell class and stay small enough to sit in L1.

`p[h][i]` and the other weights were fitted by logistic regression on the results
of self-play games (`--gendata`, `--fit`). The fit is what revealed that a piece
buried in a stack keeps most of its value — sowing a stack puts *every* piece in
it on top of a cell — which is worth about 650 Elo over hand-chosen weights that
treated buried pieces as liabilities.

## Search

Iterative deepening with aspiration windows; principal variation search; a
transposition table keyed on the board, side, ply and capture counts; late move
reductions and move-count pruning; null-move and reverse-futility pruning;
killer moves, history and counter-moves. Terminal positions are scored exactly,
so the last plies of a game are solved rather than evaluated.

## Checks

```
./engine --perft 3 1      # 153 / 23010 / 3400084, as in RULES.md
./engine --selftest       # incremental evaluation, hash and make/unmake agree
./engine --csn < file     # CSN round trip
```

`tools/check.js` passes, and 900 plies of random play reproduce the reference
module's positions exactly.
