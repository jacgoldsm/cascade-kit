# Submitting a Cascade Engine

A submission is a **directory** containing an engine and an `engine.json` manifest at
its top level. For scoring, the directory is copied **on its own** into a clean
machine with the benchmark conditions (`benchmark.json`). The engine therefore cannot
use anything outside the directory except the runtimes listed there. Source code,
build scripts, data files and network weights all go inside the directory.

## engine.json

```json
{
  "protocol": 1,
  "name": "my-engine",
  "build": "g++ -O2 -std=c++17 -o engine engine.cpp",
  "cmd": "./engine",
  "description": "Alpha-beta search with a tuned evaluation.",
  "authors": ["..."]
}
```

| Key           | Required | Meaning |
|---------------|----------|---------|
| `protocol`    | yes      | Engine protocol version. Must be `1`. |
| `name`        | yes      | 1–64 characters from `A–Z a–z 0–9 . _ -`. Used in results. |
| `cmd`         | yes      | Starts the engine. Run once per game, in the submission directory. |
| `build`       | no       | Run once, before any games, in the submission directory. |
| `description` | no       | Free text. |
| `authors`     | no       | List of strings. |

Other keys are ignored.

## How a submission is run

1. **Build.** If `build` is present, it runs through the scoring machine's shell
   (`sh -c` on Linux, `cmd.exe` on Windows; see `os` in `benchmark.json`) in the
   submission directory. It must exit with status 0 within the build timeout in
   `benchmark.json`. `buildNetwork` there says whether the network is available
   during the build. If it is not, vendor your dependencies.
2. **Games.** For each game, the referee starts a fresh process from `cmd` with the
   submission directory as its working directory, and speaks the protocol in
   PROTOCOL.md over stdin/stdout. `cmd` is split into words, and double or single
   quotes group words. It is **not** run through a shell, so pipes, redirects,
   `&&` and environment-variable assignments do not work. If you need any of those,
   put them in a script and make `cmd` run the script.
3. **Isolation.** Games have no network access. Several games may run at the same
   time, and each engine may be stopped at any moment. Do not rely on files written
   during one game being there in another.

An engine that fails to build, fails the handshake, crashes, times out or plays an
illegal move loses (see PROTOCOL.md). A build failure loses every game.

## Checking a submission

```
node tools/check.js path/to/submission
```

This validates `engine.json`, builds a clean copy of the directory (so it catches
dependencies on files outside it), checks the handshake, and plays games against a
random engine under the benchmark's time control. It reports any forfeits and the
engine's slowest moves, then prints `PASS` or `FAIL`. Options: `--games N` (default
2), `--movetime MS` to test at a different time control, and `--keep` to keep the
build directory for inspection.

A `PASS` means the submission is valid, not that it is strong. The check machine may
also be faster than the scoring machine, so leave headroom on time.

## For benchmark operators

Tournament configs accept submissions directly:

```json
{ "submission": "submissions/model-x", "go": "movetime 1000" }
```

`referee/tournament.js` builds each submission once, then uses its `cmd` with the
submission directory as the working directory. The referee does not sandbox engines.
The CPU, memory and network limits in `benchmark.json` must be enforced by the
machine or container the tournament runs in.
