'use strict';
// Packs a submission directory into the single file that is handed in for scoring.
//
//   node tools/pack.js <submission-dir> [--out cascade-submission.tar.gz] [--skip-check]
//
// It first runs tools/check.js on the directory and stops if the check fails. The
// archive holds the directory's files (not .git) with engine.json at the top level.
// It prints the archive's SHA-256, which identifies exactly what was handed in.

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { spawnSync } = require('child_process');
const { listFiles, pack, MAX_UNPACKED_BYTES } = require('./lib/tar');

function parseArgs(argv) {
  const opts = { dir: null, out: 'cascade-submission.tar.gz', check: true };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--out') opts.out = argv[++i];
    else if (argv[i] === '--skip-check') opts.check = false;
    else opts.dir = argv[i];
  }
  return opts;
}

const opts = parseArgs(process.argv.slice(2));
if (!opts.dir) {
  console.error('usage: node tools/pack.js <submission-dir> [--out cascade-submission.tar.gz] [--skip-check]');
  process.exit(2);
}
const dir = path.resolve(opts.dir);
const out = path.resolve(opts.out);
if (!fs.existsSync(path.join(dir, 'engine.json'))) {
  console.error(`no engine.json in ${dir}; the submission directory must have it at its top level`);
  process.exit(1);
}
if (out.startsWith(dir + path.sep)) {
  console.error('--out must be outside the submission directory');
  process.exit(1);
}

if (opts.check) {
  console.log(`checking ${dir} ...\n`);
  const check = spawnSync(process.execPath, [path.join(__dirname, 'check.js'), dir], { stdio: 'inherit' });
  if (check.status !== 0) {
    console.error('\nThe check failed, so no archive was written. Fix the problems above and run pack again.');
    process.exit(1);
  }
  console.log('');
}

const files = listFiles(dir);
const bytes = files.reduce((a, f) => a + fs.statSync(path.join(dir, f)).size, 0);
if (bytes > MAX_UNPACKED_BYTES) {
  console.error(`the submission is ${(bytes / 2 ** 20).toFixed(0)} MB; the limit is 1024 MB`);
  process.exit(1);
}
const archive = pack(dir, files);
fs.writeFileSync(out, archive);
const sha = crypto.createHash('sha256').update(archive).digest('hex');
console.log(`wrote ${out}`);
console.log(`  ${files.length} files, ${(bytes / 1024).toFixed(0)} KB unpacked, ${(archive.length / 1024).toFixed(0)} KB packed`);
console.log(`  sha256 ${sha}`);
