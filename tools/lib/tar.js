'use strict';
// Minimal gzipped tar (ustar) support for submission archives: regular files and
// directories only. Archives are deterministic: entries are sorted and carry no
// owners or timestamps, so the same files always produce the same tar.

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const BLOCK = 512;
const MAX_UNPACKED_BYTES = 1024 ** 3; // 1 GiB

// Relative, forward-slash paths of the regular files under dir, sorted. Skips .git.
// Throws on symbolic links and other special files.
function listFiles(dir) {
  const out = [];
  (function walk(rel) {
    for (const entry of fs.readdirSync(path.join(dir, rel), { withFileTypes: true })) {
      const r = rel ? `${rel}/${entry.name}` : entry.name;
      if (entry.name === '.git') continue;
      if (entry.isSymbolicLink()) throw new Error(`${r} is a symbolic link; archives may only contain regular files`);
      if (entry.isDirectory()) walk(r);
      else if (entry.isFile()) out.push(r);
      else throw new Error(`${r} is not a regular file`);
    }
  })('');
  return out.sort();
}

function octal(value, length) {
  return value.toString(8).padStart(length - 1, '0') + '\0';
}

function header(name, size, mode) {
  const h = Buffer.alloc(BLOCK);
  let prefix = '';
  if (Buffer.byteLength(name) > 100) {
    // ustar splits long paths at a slash into prefix (<= 155) and name (<= 100).
    const cut = name.lastIndexOf('/', 155);
    if (cut < 0 || Buffer.byteLength(name.slice(cut + 1)) > 100) throw new Error(`path too long for an archive: ${name}`);
    prefix = name.slice(0, cut);
    name = name.slice(cut + 1);
  }
  h.write(name, 0, 100);
  h.write(octal(mode, 8), 100);
  h.write(octal(0, 8), 108); // uid
  h.write(octal(0, 8), 116); // gid
  h.write(octal(size, 12), 124);
  h.write(octal(0, 12), 136); // mtime
  h.write('        ', 148); // checksum placeholder
  h.write('0', 156); // regular file
  h.write('ustar\0', 257);
  h.write('00', 263);
  h.write(prefix, 345, 155);
  let sum = 0;
  for (const b of h) sum += b;
  h.write(octal(sum, 7) + ' ', 148);
  return h;
}

// Returns a gzipped tar of the given files (relative paths under dir).
function pack(dir, files) {
  const parts = [];
  for (const rel of files) {
    const full = path.join(dir, rel);
    const data = fs.readFileSync(full);
    const executable = (fs.statSync(full).mode & 0o111) !== 0;
    parts.push(header(rel, data.length, executable ? 0o755 : 0o644), data);
    const pad = (BLOCK - (data.length % BLOCK)) % BLOCK;
    if (pad) parts.push(Buffer.alloc(pad));
  }
  parts.push(Buffer.alloc(2 * BLOCK));
  return zlib.gzipSync(Buffer.concat(parts), { level: 9 });
}

function field(h, start, length) {
  const s = h.toString('latin1', start, start + length);
  const nul = s.indexOf('\0');
  return nul >= 0 ? s.slice(0, nul) : s;
}

// Checks that an entry path stays inside the extraction directory.
function safePath(name) {
  if (!name || name.startsWith('/') || name.includes('\\') || /^[A-Za-z]:/.test(name)) {
    throw new Error(`unsafe path in archive: ${JSON.stringify(name)}`);
  }
  const parts = name.split('/').filter((p) => p !== '' && p !== '.');
  if (!parts.length || parts.includes('..')) throw new Error(`unsafe path in archive: ${JSON.stringify(name)}`);
  return parts.join('/');
}

// Parses a gzipped tar into [{ name, mode, data }] for its regular files.
// Rejects anything but regular files and directories, and unsafe paths.
function unpack(gz) {
  let tar;
  try {
    tar = zlib.gunzipSync(gz, { maxOutputLength: MAX_UNPACKED_BYTES });
  } catch (err) {
    throw new Error(`not a readable .tar.gz archive, or larger than 1 GiB unpacked (${err.code || err.message})`);
  }
  const files = [];
  const seen = new Set();
  for (let off = 0; off + BLOCK <= tar.length;) {
    const h = tar.subarray(off, off + BLOCK);
    if (h.every((b) => b === 0)) break; // end of archive
    let sum = 0;
    for (let i = 0; i < BLOCK; i++) sum += i >= 148 && i < 156 ? 32 : h[i];
    if (parseInt(field(h, 148, 8).trim(), 8) !== sum) throw new Error('corrupt archive (bad header checksum)');
    const prefix = field(h, 345, 155);
    const name = prefix ? `${prefix}/${field(h, 0, 100)}` : field(h, 0, 100);
    const size = parseInt(field(h, 124, 12).trim() || '0', 8);
    const mode = parseInt(field(h, 100, 8).trim() || '644', 8);
    const type = field(h, 156, 1) || '0';
    off += BLOCK;
    if (type === '0') {
      if (off + size > tar.length) throw new Error('corrupt archive (truncated file)');
      const rel = safePath(name);
      if (seen.has(rel)) throw new Error(`duplicate path in archive: ${rel}`);
      seen.add(rel);
      files.push({ name: rel, mode, data: tar.subarray(off, off + size) });
    } else if (type !== '5') {
      throw new Error(`unsupported entry "${name}" (type ${type}); make archives with tools/pack.js`);
    }
    off += Math.ceil(size / BLOCK) * BLOCK;
  }
  return files;
}

// Writes unpacked files under dir, which must not exist yet.
function extract(files, dir) {
  fs.mkdirSync(dir, { recursive: false });
  for (const f of files) {
    const target = path.join(dir, ...f.name.split('/'));
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, f.data, { mode: f.mode & 0o111 ? 0o755 : 0o644 });
  }
}

module.exports = { listFiles, pack, unpack, extract, MAX_UNPACKED_BYTES };
