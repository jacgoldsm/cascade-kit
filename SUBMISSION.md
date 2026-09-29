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

Submissions are built and played in Docker containers made from the image in
`docker/Dockerfile` (Linux, with the runtimes listed in `benchmark.json`).

1. **Build.** If `build` is present, it runs once through `sh -c` in a container,
   with the submission directory mounted read-write as the working directory. It
   must exit with status 0 within the build timeout in `benchmark.json`. The build
   has no network access (`buildNetwork`), so vendor any dependencies.
2. **Games.** For each game, the referee starts a fresh container that runs `cmd`
   with the submission directory as its working directory, and speaks the protocol
   in PROTOCOL.md over stdin/stdout. `cmd` is split into words, and double or
   single quotes group words. It is **not** run through a shell, so pipes,
   redirects, `&&` and environment-variable assignments do not work. If you need
   any of those, put them in a script and make `cmd` run the script.
3. **Limits.** Each engine gets the CPU and memory in `benchmark.json`
   (`engineCpus`, `engineMemoryMb`) and no swap. It runs as a non-root user with
   no network. The submission directory is **read-only** during games; `/tmp` is
   writable (256 MB) and is where `HOME` points. Several games run at the same
   time, and each engine may be stopped at any moment, so do not rely on files
   written during one game being there in another.

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

To check in the scoring environment itself, with Docker installed:

```
docker build -t cascade-runtime:local docker
node tools/check.js path/to/submission --docker cascade-runtime:local
```

This runs the build and the engine in containers with the same limits as scoring,
so it also catches missing runtimes and writes to the read-only directory.

## Packing a submission

```
node tools/pack.js path/to/submission
```

This runs the check above and, if it passes, writes `cascade-submission.tar.gz`:
the directory's files (without `.git`) with `engine.json` at the top level, at
most 1 GiB unpacked. It prints the archive's SHA-256, which identifies exactly what
was handed in. `--out FILE` writes somewhere else. Archives may contain only
regular files: no symbolic links.

A `PASS` means the submission is valid, not that it is strong. The check machine may
also be faster than the scoring machine, so leave headroom on time.

## For benchmark operators

To add a packed submission, unpack it into `submissions/<name>/` of the main
repository, then commit and push:

```
node tools/add-submission.js cascade-submission.tar.gz <name>
```

It validates the archive and its `engine.json` before writing anything, rejects
paths outside the directory, and will not overwrite an existing submission unless
given `--replace`.

Commit a submission as `submissions/<name>/` in the main repository and push. The
**Score submissions** workflow scores every changed submission against the ladder in
`configs/scoring.json`, then commits `scores/<name>/` and updates `LEADERBOARD.md`.
It can also be run by hand from the Actions tab, for one submission or all of them.
Scoring fits each submission together with the ladder's calibration games in
`calibration/`, so run the **Calibrate ladder** workflow first, and again after
changing the ladder or `benchmark.json`. The same steps run locally with
`node tools/score.js --calibrate` and `node tools/score.js submissions/<name>`.

Tournament configs also accept submissions directly:

```json
{ "submission": "submissions/model-x", "go": "movetime 250",
  "docker": { "image": "cascade-runtime:local", "cpus": 1, "memoryMb": 1024 } }
```

Without `docker`, the referee runs engines directly on the machine, unsandboxed.
