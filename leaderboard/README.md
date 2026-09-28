# Cascade leaderboard

Elo against the reference ladder, anchored at greedy = 0, with 95% bootstrap intervals. Updated automatically after each scoring run.

| # | Submission | By | Elo | 95% interval | Games | Score | Forfeits | Version | Scored |
|---:|---|---|---:|---|---:|---:|---:|---|---|
| 1 | [opus-5.5-max](opus-5.5-max.md) | Claude | 1389 | [1293, 1486] | 160 | 100.0% | 0 | 5b37e37 | 2026-09-28 |
| 2 | [js-random](js-random.md) |  | -906 | [-1009, -803] | 160 | 0.0% | 0 | 489b120 | 2026-09-28 |
| 3 | [python-random](python-random.md) |  | -906 | [-1009, -795] | 160 | 0.0% | 0 | 489b120 | 2026-09-28 |

## Reference ladder

From 240 calibration games between the ladder engines.

| Engine | Elo | 95% interval |
|---|---:|---|
| ab-250ms | 969 | [861, 1128] |
| ab-25ms | 548 | [494, 589] |
| greedy | 0 | [0, 0] |
| mcts-250ms | -421 | [-589, -287] |
