# Cascade leaderboard

Elo against the reference ladder, anchored at greedy = 0, with 95% bootstrap intervals. Updated automatically after each scoring run.

| # | Submission | By | Elo | 95% interval | Games | Score | Forfeits | Version | Scored |
|---:|---|---|---:|---|---:|---:|---:|---|---|
| 1 | [opus-5.5-max](opus-5.5-max.md) | Claude | 1523 | [1433, 1652] | 240 | 97.1% | 0 | 5b37e37 | 2026-09-30 |
| 2 | [js-random](js-random.md) |  | -849 | [-904, -782] | 240 | 0.0% | 0 | 6cc5cd8 | 2026-09-30 |
| 3 | [python-random](python-random.md) |  | -849 | [-908, -784] | 240 | 0.0% | 0 | 6cc5cd8 | 2026-09-30 |

## Reference ladder

From 1120 calibration games between the ladder engines.

| Engine | Elo | 95% interval |
|---|---:|---|
| sonnet-5.5-high | 1643 | [1545, 1773] |
| sonnet-5.5-100ms | 1460 | [1379, 1574] |
| sonnet-5.5-25ms | 1063 | [986, 1166] |
| sonnet-4.6-high | 785 | [705, 885] |
| ab-250ms | 652 | [589, 719] |
| ab-25ms | 440 | [393, 487] |
| greedy | 0 | [0, 0] |
| mcts-250ms | -197 | [-294, -92] |
