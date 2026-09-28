'use strict';
// Runs engine processes and plays refereed games between them (see PROTOCOL.md).

const { spawn } = require('child_process');
const path = require('path');
const readline = require('readline');
const { performance } = require('perf_hooks');
const { Cascade, optionsToString, WHITE } = require('../cascade/rules');

const HANDSHAKE_MS = 15000;
const UNTIMED_LIMIT_MS = 300000; // hang protection for `go nodes` / `go depth`

function splitCommand(cmd) {
  const argv = [];
  for (const m of cmd.matchAll(/"([^"]*)"|'([^']*)'|(\S+)/g)) argv.push(m[1] ?? m[2] ?? m[3]);
  return argv;
}

// Hard limit for one move: movetime plus a grace allowance for process and OS
// scheduling overhead. Untimed budgets only get hang protection.
function moveLimitMs(spec) {
  const m = /movetime\s+(\d+)/.exec(spec.go || '');
  if (!m) return spec.hardLimitMs || UNTIMED_LIMIT_MS;
  const t = Number(m[1]);
  return Math.ceil(t * (spec.graceFactor ?? 1.1) + (spec.graceMs ?? 250));
}

let containerCount = 0;

// Options shared by engine and build containers: no network, a CPU and memory
// allowance, no swap, a process limit, no capabilities, and the host user's uid
// (so files a build writes into the mounted directory stay owned by that user).
function dockerLimits(docker) {
  const args = [
    '--network', 'none',
    '--cpus', String(docker.cpus),
    '--memory', `${docker.memoryMb}m`, '--memory-swap', `${docker.memoryMb}m`,
    '--pids-limit', '256',
    '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges',
    '-e', 'HOME=/tmp', '-e', 'CARGO_HOME=/tmp/.cargo',
  ];
  if (process.getuid) args.push('--user', `${process.getuid()}:${process.getgid()}`);
  return args;
}

// The argv that runs `cmd` in `dir` inside a container. The directory is mounted
// read-only at /engine and the root filesystem is read-only; /tmp is a small tmpfs.
function dockerEngineArgv(docker, dir, cmd, name) {
  return [
    'docker', 'run', '--rm', '-i', '--name', name,
    ...dockerLimits(docker),
    '--read-only', '--tmpfs', '/tmp:rw,exec,size=256m',
    '-v', `${path.resolve(dir)}:/engine:ro`, '-w', '/engine',
    docker.image, ...splitCommand(cmd),
  ];
}

class EngineProcess {
  // spec: { name, cmd, cwd?, go, docker? }. With docker: { image, cpus, memoryMb },
  // the engine runs in a container instead of directly on this machine.
  constructor(spec) {
    this.spec = spec;
    let argv = splitCommand(spec.cmd);
    if (spec.docker) {
      this.container = `cascade-${process.pid}-${++containerCount}`;
      argv = dockerEngineArgv(spec.docker, spec.cwd || process.cwd(), spec.cmd, this.container);
    }
    const [exe, ...args] = argv;
    this.child = spawn(exe, args, { cwd: spec.cwd, stdio: ['pipe', 'pipe', 'ignore'], windowsHide: true });
    this.lines = [];
    this.waiter = null;
    this.exited = false;
    this.child.on('error', () => this.onExit());
    this.child.on('exit', () => this.onExit());
    this.child.stdin.on('error', () => {}); // engine died; reported via exit
    readline.createInterface({ input: this.child.stdout }).on('line', (line) => {
      if (this.waiter) { const w = this.waiter; this.waiter = null; w(line); } else this.lines.push(line);
    });
  }

  onExit() {
    this.exited = true;
    if (this.waiter) { const w = this.waiter; this.waiter = null; w(null); }
  }

  send(line) {
    if (!this.exited) this.child.stdin.write(line + '\n');
  }

  // Resolves to the next line, or null if the engine exited or the deadline passed.
  nextLine(deadline) {
    if (this.lines.length) return Promise.resolve(this.lines.shift());
    if (this.exited) return Promise.resolve(null);
    return new Promise((resolve) => {
      const timer = setTimeout(() => { this.waiter = null; resolve(null); }, Math.max(0, deadline - performance.now()));
      this.waiter = (line) => { clearTimeout(timer); resolve(line); };
    });
  }

  // Reads lines until one starts with `prefix`. Returns its arguments, or null.
  async waitFor(prefix, timeoutMs) {
    const deadline = performance.now() + timeoutMs;
    for (;;) {
      const line = await this.nextLine(deadline);
      if (line === null) return null;
      const [cmd, ...rest] = line.trim().split(/\s+/);
      if (cmd === 'name') this.name = rest.join(' ');
      if (cmd === prefix) return rest;
    }
  }

  async handshake() {
    this.send('cascade 1');
    return (await this.waitFor('ready', HANDSHAKE_MS)) !== null;
  }

  // Asks the engine to exit and resolves once it has, killing it after one second.
  quit() {
    this.send('quit');
    if (this.exited) return Promise.resolve();
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        // Killing the `docker run` client does not stop its container.
        if (this.container) spawn('docker', ['rm', '-f', this.container], { stdio: 'ignore' });
        this.child.kill();
      }, 1000);
      const done = () => { clearTimeout(timer); resolve(); };
      this.child.once('exit', done);
      this.child.once('error', done);
    });
  }
}

// Plays one game. white and black are engine specs: { name, cmd, go, cwd? }.
// Returns a JSON-serialisable game record.
async function playGame({ rules, seed, white, black }) {
  const game = new Cascade(rules);
  const specs = [white, black];
  const engines = specs.map((spec) => new EngineProcess(spec));
  const record = {
    white: white.name, black: black.name, seed, rules: game.options,
    winner: null, reason: null, plies: 0, score: null, moves: [], ms: [],
  };
  const forfeit = (side, reason) => {
    record.winner = side === WHITE ? 'black' : 'white';
    record.reason = reason + ' by ' + (side === WHITE ? 'white' : 'black');
  };

  try {
    const ok = await Promise.all(engines.map((e) => e.handshake()));
    if (!ok[0] || !ok[1]) { forfeit(ok[0] ? 1 : 0, 'no handshake'); return record; }
    const newgame = 'newgame ' + optionsToString(game.options);
    engines.forEach((e) => e.send(newgame));

    let state = game.initialState(seed);
    while (!game.isTerminal(state)) {
      const side = game.sideToMove(state);
      const engine = engines[side];
      engine.send('position ' + game.toCSN(state));
      const t0 = performance.now();
      engine.send('go ' + (specs[side].go || ''));
      const reply = await engine.waitFor('bestmove', moveLimitMs(specs[side]));
      const ms = Math.round(performance.now() - t0);
      if (reply === null) { forfeit(side, engine.exited ? 'crash' : 'timeout'); break; }
      const m = game.parseMove(reply[0] || '');
      if (m < 0 || !game.isLegal(state, m)) {
        record.moves.push(reply[0] || '');
        forfeit(side, 'illegal move');
        break;
      }
      record.moves.push(game.moveToString(m));
      record.ms.push(ms);
      state = game.play(state, m);
    }
    record.plies = game.ply(state);
    record.score = game.scores(state);
    if (!record.reason) {
      record.winner = game.winner(state) === WHITE ? 'white' : 'black';
      record.reason = game.ply(state) >= game.maxply ? 'move cap' : 'no moves';
    }
    return record;
  } finally {
    await Promise.all(engines.map((e) => e.quit()));
  }
}

module.exports = { playGame, EngineProcess, splitCommand, moveLimitMs, dockerLimits };
