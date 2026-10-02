# Cascade leaderboard

Elo against the reference ladder, anchored at greedy = 0, with 95% bootstrap intervals. Updated automatically after each scoring run.

| # | Submission | By | Elo | 95% interval | Games | Score | Forfeits | Version | Scored |
|---:|---|---|---:|---|---:|---:|---:|---|---|
| 1 | [opus-5.5-max](opus-5.5-max.md) | Claude | 1769 | [1670, 1891] | 320 | 97.5% | 0 | 5b37e37 | 2026-10-01 |
| 2 | [opus-5-high](opus-5-high.md) |  | 1603 | [1519, 1710] | 320 | 92.2% | 0 | 68dc1e3 | 2026-10-02 |
| 3 | [js-random](js-random.md) |  | -627 | [-700, -559] | 320 | 0.0% | 0 | 730f79c | 2026-10-01 |
| 4 | [python-random](python-random.md) |  | -627 | [-697, -566] | 320 | 0.0% | 0 | 730f79c | 2026-10-01 |

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
