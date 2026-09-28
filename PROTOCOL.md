# Cascade Engine Protocol

An engine is any program that plays Cascade by exchanging lines of text with the
referee over standard input and output. Engines can be written in any language. The
referee starts a fresh engine process for every game.

Messages are single lines of ASCII text, ending in `\n`. Tokens are separated by
spaces. Engines must flush standard output after each line. Anything an engine writes
to standard error is ignored.

## Referee → engine

| Command                   | Meaning |
|---------------------------|---------|
| `cascade 1`               | Sent once, first. The engine must reply `ready` within 15 seconds; do any expensive loading (e.g. network weights) before replying. |
| `newgame <options>`       | A game is starting under these rules, as `key=value` pairs, e.g. `newgame side=5 collapse=6 maxply=150 komi=0.5`. See RULES.md §6. |
| `position <CSN>`          | The current position (RULES.md §7). Sent before every `go`. The position contains everything needed to play; engines do not need to track the game history. |
| `go movetime <ms>`        | Think for at most `<ms>` milliseconds, then reply with a move. |
| `go nodes <n>` / `go depth <d>` | Optional untimed budgets used by the baseline engines for reproducible experiments. Engines may ignore them. |
| `quit`                    | Exit. The referee kills engines that are still running one second later. |

Unknown commands must be ignored.

Engines may only use the CPU between receiving `go` and replying `bestmove`. Thinking
on the opponent's time ("pondering"), or doing any other work between moves, is not
allowed: engines share the scoring machine.

## Engine → referee

| Reply              | Meaning |
|--------------------|---------|
| `name <text>`      | Optional, before `ready`: the engine's name. |
| `ready`            | Handshake complete. |
| `bestmove <move>`  | The engine's move in the current position, e.g. `bestmove e5NE`. |
| `info <text>`      | Optional, any time: free-form diagnostics, which the referee ignores. |

## Time control and forfeits

In timed games the engine gets a fixed `movetime` for every move. Time is measured by
the referee, from sending `go` to receiving `bestmove`. A move is accepted if it arrives
within `1.1 × movetime + 250 ms`. The allowance covers process scheduling and pipe
latency and is not extra thinking time.

An engine **loses the game immediately** if it:

- does not reply `ready` to `cascade 1` within 15 seconds;
- does not reply `bestmove` within the allowance;
- exits or crashes; or
- plays an illegal or unparseable move.

## Example session

```
> cascade 1
< name example-engine
< ready
> newgame side=5 collapse=6 maxply=150 komi=0.5
> position w,w,b,b,w/w,w,b,b,w,b/b,w,w,b,w,b,w/w,w,b,b,b,b,w,b/b,w,w,w,-,b,b,b,w/w,b,w,w,w,w,b,b/b,w,b,w,b,b,w/w,b,w,w,b,b/b,w,w,b,b w 0 0 0
> go movetime 1000
< info depth 5 score 3.5
< bestmove c2E
...
> quit
```

`engines/lib/protocol.js` implements the engine side for JavaScript engines, and
`engines/random.js` shows the smallest possible engine built on it.
