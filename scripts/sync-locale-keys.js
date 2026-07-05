#!/usr/bin/env node
// =============================================================================
// scripts/sync-locale-keys.js
// -----------------------------------------------------------------------------
// Make sure every locale JSON has all keys that en.json has. Missing keys are
// copied from en.json so the UI never shows a raw key (it'll just be in
// English until a translator fills it in).
//
// Run after adding new strings to en.json:
//   node scripts/sync-locale-keys.js
//
// Idempotent — running multiple times has no effect once locales are in sync.
// =============================================================================
const fs   = require('node:fs');
const path = require('node:path');

const LOCALES_DIR = path.resolve(__dirname, '..', 'client', 'locales');
const SOURCE_LOCALE = 'en.json';

// The locale files are stored with a UTF-8 BOM, which JSON.parse rejects.
function readJson(p) {
  return JSON.parse(fs.readFileSync(p, 'utf8').replace(/^﻿/, ''));
}

const sourcePath = path.join(LOCALES_DIR, SOURCE_LOCALE);
const source = readJson(sourcePath);

// Recursively merge `src` into `dst`: for any key missing in `dst`, copy from
// `src`. Skip `_metadata` since that's locale-specific.
function mergeMissing(src, dst, pathStack = []) {
  let added = 0;
  for (const [k, v] of Object.entries(src)) {
    if (k === '_metadata') continue;
    if (!(k in dst)) {
      dst[k] = JSON.parse(JSON.stringify(v));
      added += countLeaves(v);
      console.log(`  + ${[...pathStack, k].join('.')}`);
      continue;
    }
    if (v && typeof v === 'object' && !Array.isArray(v) &&
        dst[k] && typeof dst[k] === 'object' && !Array.isArray(dst[k])) {
      added += mergeMissing(v, dst[k], [...pathStack, k]);
    }
  }
  return added;
}

function countLeaves(v) {
  if (v === null || typeof v !== 'object') return 1;
  if (Array.isArray(v)) return 1;
  let n = 0;
  for (const x of Object.values(v)) n += countLeaves(x);
  return n;
}

const files = fs.readdirSync(LOCALES_DIR).filter(f => f.endsWith('.json') && f !== SOURCE_LOCALE);
let totalAdded = 0;
for (const file of files) {
  const fullPath = path.join(LOCALES_DIR, file);
  const data = readJson(fullPath);
  console.log(`-- ${file} --`);
  const added = mergeMissing(source, data);
  if (added > 0) {
    fs.writeFileSync(fullPath, JSON.stringify(data, null, 2) + '\n', 'utf8');
    console.log(`  → added ${added} keys`);
    totalAdded += added;
  } else {
    console.log(`  (in sync)`);
  }
}

console.log(`\nDone. Added ${totalAdded} keys across ${files.length} locale files.`);
