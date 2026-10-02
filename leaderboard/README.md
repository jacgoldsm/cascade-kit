# Cascade leaderboard

Elo anchored at greedy = 0, with 95% bootstrap intervals. Reference engines (in italics) are the ladder that submissions are scored against; they are rated from their games against each other, and each submission from its games against them, on the same scale. Some reference engines are given less time per move than submissions, as described. Updated automatically after each scoring run.

| # | Engine | By | Elo | 95% interval | Games | Score | Forfeits | Version | Rated |
|---:|---|---|---:|---|---:|---:|---:|---|---|
| 1 | [opus-5.5-max](opus-5.5-max.md) | Claude | 1769 | [1670, 1891] | 320 | 97.5% | 0 | 5b37e37 | 2026-10-01 |
| 2 | *sonnet-5.5-high* (reference) | Claude Sonnet 5.5 (high effort) submission | 1643 | [1545, 1773] | 280 | 96.8% | 0 |  | 2026-10-01 |
| 3 | [opus-5-high](opus-5-high.md) |  | 1603 | [1519, 1710] | 320 | 92.2% | 0 | 68dc1e3 | 2026-10-02 |
| 4 | *sonnet-5.5-100ms* (reference) | Claude Sonnet 5.5 (high effort) submission, limited to 100 ms per move | 1460 | [1379, 1574] | 280 | 88.6% | 0 |  | 2026-10-01 |
| 5 | *sonnet-5.5-25ms* (reference) | Claude Sonnet 5.5 (high effort) submission, limited to 25 ms per move | 1063 | [986, 1166] | 280 | 69.6% | 0 |  | 2026-10-01 |
| 6 | *sonnet-4.6-high* (reference) | Claude Sonnet 4.6 (high effort) submission | 785 | [705, 885] | 280 | 53.6% | 0 |  | 2026-10-01 |
| 7 | *ab-250ms* (reference) | Alpha-beta search on the score margin, 250 ms per move | 652 | [589, 719] | 280 | 45.0% | 0 |  | 2026-10-01 |
| 8 | *ab-25ms* (reference) | Alpha-beta search on the score margin, 25 ms per move | 440 | [393, 487] | 280 | 32.1% | 0 |  | 2026-10-01 |
| 9 | *greedy* (reference) | Plays the move that maximises the immediate score (the 0 anchor) | 0 | [0, 0] | 280 | 11.4% | 0 |  | 2026-10-01 |
| 10 | *mcts-250ms* (reference) | Plain Monte Carlo tree search with random playouts, 250 ms per move | -197 | [-294, -92] | 280 | 2.9% | 0 |  | 2026-10-01 |
| 11 | [js-random](js-random.md) |  | -627 | [-700, -559] | 320 | 0.0% | 0 | 730f79c | 2026-10-01 |
| 12 | [python-random](python-random.md) |  | -627 | [-697, -566] | 320 | 0.0% | 0 | 730f79c | 2026-10-01 |
