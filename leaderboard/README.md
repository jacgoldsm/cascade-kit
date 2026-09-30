# Cascade leaderboard

Elo against the reference ladder, anchored at greedy = 0, with 95% bootstrap intervals. Updated automatically after each scoring run.

| # | Submission | By | Elo | 95% interval | Games | Score | Forfeits | Version | Scored |
|---:|---|---|---:|---|---:|---:|---:|---|---|
| 1 | [opus-5.5-max](opus-5.5-max.md) | Claude | 1407 | [1341, 1498] | 200 | 100.0% | 0 | 5b37e37 | 2026-09-30 |
| 2 | [sonnet-5.5-high](sonnet-5.5-high.md) |  | 1407 | [1346, 1505] | 200 | 100.0% | 0 | 1aaea2d | 2026-09-30 |
| 3 | [js-random](js-random.md) |  | -843 | [-929, -766] | 200 | 0.0% | 0 | 0b7ffd1 | 2026-09-30 |
| 4 | [python-random](python-random.md) |  | -843 | [-929, -763] | 200 | 0.0% | 0 | 0b7ffd1 | 2026-09-30 |

## Reference ladder

From 400 calibration games between the ladder engines.

| Engine | Elo | 95% interval |
|---|---:|---|
| sonnet-4.6-high | 900 | [798, 1037] |
| ab-250ms | 801 | [720, 911] |
| ab-25ms | 508 | [458, 554] |
| greedy | 0 | [0, 0] |
| mcts-250ms | -400 | [-548, -282] |
