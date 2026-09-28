# Task: build the strongest Cascade engine you can

Cascade is a two-player strategy game described in `RULES.md`. Your task is to write
a program that plays it as well as possible. Your score is the **Elo rating** of your
engine, measured in games against a set of reference engines you do not have access
to.

## What to deliver

Put your engine in the `submission/` directory, with an `engine.json` manifest, as
described in `SUBMISSION.md`. The directory is copied on its own to the scoring
machine, so it must contain everything the engine needs. Before you finish, run:

```
node tools/check.js submission
```

It must print `PASS`. A submission that fails to build, crashes, times out or plays
an illegal move loses those games.

## Conditions

| | |
|---|---|
| Time per move | 250 ms (`go movetime 250`), measured by the referee |
| Scoring machine | 1 CPU core and 1 GB RAM per engine, in a Docker container on a GitHub Actions ubuntu-24.04 runner (x86-64); no GPU |
| Operating system | Linux (Debian 12 container image defined by docker/Dockerfile); builds run through sh |
| Build | runs once, at most 600 s, network not available |
| Games | no network access |
| Rules | `side=5 collapse=6 maxply=150 komi=0.5` (the defaults in RULES.md) |
| Development time | your session |

Runtimes available on the scoring machine:

- node (Node.js 22)
- python / python3 (Python 3.11, standard library only)
- gcc / g++ (GCC 12) and make
- rustc / cargo (Rust 1.85, no crates.io access)

## What you have

- `RULES.md`: the complete rules. `PROTOCOL.md`: how engines talk to the referee.
- `cascade/rules.js`: a reference implementation of the rules in JavaScript. You may
  copy it, port it or ignore it. If your implementation disagrees with it, it is
  your implementation that is wrong. RULES.md §8 has perft counts for checking.
- `referee/`: the same referee used for scoring. `node referee/tournament.js
  <config.json>` plays matches between engines and reports Elo, so you can test
  versions of your engine against each other.
- `engines/random.js`: a random engine, and `examples/`: minimal complete
  submissions in JavaScript and Python to start from.
- `tools/replay.js`: prints a recorded game move by move.

Games in the benchmark are played in pairs from the same seeded opening, with
colours swapped. Openings are randomly generated, so there is no fixed opening to
prepare for.
