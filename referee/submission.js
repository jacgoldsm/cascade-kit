'use strict';
// Loads, validates and builds submissions: self-contained directories with an
// engine.json manifest (see SUBMISSION.md).

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const MANIFEST = 'engine.json';
const KNOWN_KEYS = new Set(['protocol', 'name', 'cmd', 'build', 'description', 'authors']);

// Returns { manifest, errors, warnings }. The manifest is usable only if errors is empty.
function loadManifest(dir) {
  const errors = [], warnings = [];
  const file = path.join(dir, MANIFEST);
  if (!fs.existsSync(file)) return { manifest: null, errors: [`no ${MANIFEST} in ${dir}`], warnings };
  let m;
  try {
    m = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (err) {
    return { manifest: null, errors: [`${MANIFEST} is not valid JSON: ${err.message}`], warnings };
  }
  if (typeof m !== 'object' || m === null || Array.isArray(m)) {
    return { manifest: null, errors: [`${MANIFEST} must contain a JSON object`], warnings };
  }
  if (m.protocol !== 1) errors.push('"protocol" must be 1');
  if (typeof m.name !== 'string' || !/^[A-Za-z0-9._-]{1,64}$/.test(m.name)) {
    errors.push('"name" must be 1-64 characters from A-Z a-z 0-9 . _ -');
  }
  if (typeof m.cmd !== 'string' || !m.cmd.trim()) errors.push('"cmd" must be a non-empty string');
  if (m.build !== undefined && (typeof m.build !== 'string' || !m.build.trim())) {
    errors.push('"build", if present, must be a non-empty string');
  }
  if (m.description !== undefined && typeof m.description !== 'string') errors.push('"description" must be a string');
  if (m.authors !== undefined && (!Array.isArray(m.authors) || !m.authors.every((a) => typeof a === 'string'))) {
    errors.push('"authors" must be an array of strings');
  }
  for (const k of Object.keys(m)) if (!KNOWN_KEYS.has(k)) warnings.push(`unknown key "${k}" is ignored`);
  return { manifest: errors.length ? null : m, errors, warnings };
}

// Runs the manifest's build command (through the shell) in dir.
// Resolves to { ok, ms, output } where output is the tail of stdout and stderr.
function build(dir, manifest, timeoutMs) {
  if (!manifest.build) return Promise.resolve({ ok: true, ms: 0, output: '' });
  const start = Date.now();
  return new Promise((resolve) => {
    const child = spawn(manifest.build, { cwd: dir, shell: true, windowsHide: true });
    let output = '';
    const collect = (chunk) => { output = (output + chunk).slice(-8000); };
    child.stdout.on('data', collect);
    child.stderr.on('data', collect);
    const timer = setTimeout(() => {
      output += `\n[build killed after ${timeoutMs} ms]`;
      child.kill();
    }, timeoutMs);
    child.on('error', (err) => { output += '\n' + err.message; });
    child.on('close', (code) => {
      clearTimeout(timer);
      if (code !== 0) output += `\n[build exited with status ${code}]`;
      resolve({ ok: code === 0, ms: Date.now() - start, output });
    });
  });
}

// Turns a tournament engine entry { name, submission: dir, go } into a runnable
// spec { name, cmd, cwd, go }, building the submission first. Throws on failure.
async function prepareSubmission(entry, buildTimeoutMs) {
  const dir = path.resolve(entry.submission);
  const { manifest, errors } = loadManifest(dir);
  if (!manifest) throw new Error(`submission ${dir}: ${errors.join('; ')}`);
  const result = await build(dir, manifest, buildTimeoutMs);
  if (!result.ok) throw new Error(`submission ${dir}: build failed\n${result.output}`);
  return { ...entry, name: entry.name || manifest.name, cmd: manifest.cmd, cwd: dir };
}

module.exports = { MANIFEST, loadManifest, build, prepareSubmission };
