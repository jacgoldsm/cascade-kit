# Development tools for the Cascade engine

The submission in `submission/` is generated from the full development source here.

| File | Purpose |
|---|---|
| `engine_dev.cpp` | Full engine source, including development modes (`perft`, `gen`, `bench*`, ...) and environment-variable parameter overrides. Blocks between `// DEV-BEGIN` / `// DEV-END` are removed from the submission. |
| `make_submission.py` | Writes `submission/engine.cpp` from `engine_dev.cpp` (strips the DEV blocks). |
| `mknet.py` | Converts a trained `.net` file into `submission/netdata.h` (the network is embedded in the source and built with `-DEMBED_NET`). |
| `train.cpp` | Trainer for the evaluation network (OpenMP, Adam, 12-fold board-symmetry augmentation). |
| `xcheck.js` | Cross-checks the engine's move application against `cascade/rules.js` on random games. |
| `rr.js`, `cmp.sh` | Round-robin / head-to-head match runners built on `referee/tournament.js`. |
| `featexp.py`, `errs.py`, `smooth.py` | Analysis scripts used while designing the evaluation. |

## Engine

* Board: 61 cells, each stack stored as a "sentinel code" (leading 1 bit, then the pieces bottom to top),
  so adding a piece on top is `(code << 1) | colour` and the top colour is `code & 1`.
* Search: iterative-deepening PVS with a transposition table, null-move pruning, late-move reductions
  (gap-based, using the one-ply child evaluations that also order the moves), killer/history ordering, exact
  terminal handling (move cap / no stacks).
* Evaluation: a small network (61 cells x 62 stack codes sparse input -> 32 ReLU units -> 1) predicting the final
  score margin minus the current margin, from the side to move's point of view. Two int16 accumulators (one per
  colour perspective) are updated incrementally; leaves are evaluated without a full make/unmake
  (`leafAfterMove`).
* Training data: self-play (`gen` mode) with node-limited searches; labels are the search score (smoothed over
  3 plies, proven-result scores replaced by the realised margin) mixed with the game outcome.
