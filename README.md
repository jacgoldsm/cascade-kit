# Cascade Entrant Kit

Everything needed to build and test an engine for the Cascade benchmark. Start with
`TASK.md`.

| File                | Contents |
|---------------------|----------|
| `TASK.md`           | The task and the conditions your engine is scored under. |
| `RULES.md`          | The rules of Cascade. |
| `PROTOCOL.md`       | The text protocol between engines and the referee. |
| `SUBMISSION.md`     | The `engine.json` manifest and how submissions are built and run. |
| `benchmark.json`    | The scoring conditions, as read by `tools/check.js`. |
| `docker/Dockerfile` | The Linux environment engines are built and scored in. |
| `submission/`       | Where your engine goes. |
| `examples/`         | Complete minimal submissions in JavaScript and Python. |
| `cascade/rules.js`  | Reference rules implementation (JavaScript). |
| `engines/random.js` | A random engine, and `engines/lib/protocol.js`, a protocol helper. |
| `referee/`          | The referee and tournament runner used for scoring. |
| `tools/`            | `check.js` (validate a submission), `replay.js`, `stats.js`. |

The tools need Node.js 18 or later and have no dependencies.

Scores for submitted engines are published in [leaderboard/](leaderboard/README.md).

## Quick start

Check one of the examples:

```
node tools/check.js examples/python-random
```

To check in the scoring environment itself (needs Docker):

```
docker build -t cascade-runtime:local docker
node tools/check.js examples/python-random --docker cascade-runtime:local
```

Start your own engine from an example:

```
cp -r examples/js-random/* submission/
node tools/check.js submission
```

Play two engines against each other, e.g. a new version of yours against an old
copy, by writing a tournament config:

```json
{
  "engines": [
    { "submission": "submission", "go": "movetime 200" },
    { "submission": "old-version", "go": "movetime 200" }
  ],
  "pairs": 20,
  "concurrency": 2,
  "out": "results/new-vs-old"
}
```

```
node referee/tournament.js new-vs-old.json
node tools/replay.js results/new-vs-old/games.jsonl 1 --every 10
```

Engine names come from each `engine.json` unless the config gives a `"name"`, so the
two versions need different names.
