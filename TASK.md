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
| Time per move | 1000 ms (`go movetime 1000`), measured by the referee |
| Scoring machine | Intel N100 (4 cores, 4 threads, 0.8 GHz base / 3.4 GHz boost), 16 GB RAM, Intel UHD integrated graphics (no CUDA GPU) |
| Operating system | Windows 11 Home, 64-bit (builds run through cmd.exe) |
| Build | runs once, at most 600 s, network not available |
| Games | no network access |
| Rules | `side=5 collapse=6 maxply=150 komi=0.5` (the defaults in RULES.md) |
| Development time | your session |

Runtimes available on the scoring machine:

- node (Node.js 24.11)
- python (Python 3.13.6, standard library only)
- No C, C++, Rust, Go, Java or .NET compilers are installed.

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
