# Cascade leaderboard

Elo against the reference ladder, anchored at greedy = 0, with 95% bootstrap intervals. Updated automatically after each scoring run.

| # | Submission | By | Elo | 95% interval | Games | Score | Forfeits | Version | Scored |
|---:|---|---|---:|---|---:|---:|---:|---|---|
| 1 | [opus-5.5-max](opus-5.5-max.md) | Claude | 1407 | [1341, 1498] | 200 | 100.0% | 0 | 5b37e37 | 2026-09-30 |
| 2 | [js-random](js-random.md) |  | -843 | [-929, -766] | 200 | 0.0% | 0 | 0b7ffd1 | 2026-09-30 |
| 3 | [python-random](python-random.md) |  | -843 | [-929, -763] | 200 | 0.0% | 0 | 0b7ffd1 | 2026-09-30 |

## Reference ladder

From 600 calibration games between the ladder engines.

| Engine | Elo | 95% interval |
|---|---:|---|
| sonnet-5.5-high | 1435 | [1359, 1526] |
| sonnet-4.6-high | 879 | [784, 1003] |
| ab-250ms | 740 | [668, 833] |
| ab-25ms | 446 | [393, 490] |
| greedy | 0 | [0, 0] |
| mcts-250ms | -493 | [-542, -431] |
