#!/usr/bin/env node
// =============================================================================
// scripts/version.js
// -----------------------------------------------------------------------------
// Manages version updates across the monorepo. Supports:
//   npm run version -- 2.0.0          (set specific version)
//   npm run bump -- major|minor|patch (bump from current version)
// =============================================================================
const fs = require('node:fs');
const path = require('node:path');

const REPO_ROOT = path.resolve(__dirname, '..');

const FILES_TO_UPDATE = [
  { path: 'package.json', key: 'version', type: 'json' },
  { path: 'client/package.json', key: 'version', type: 'json' },
  { path: 'docs-site/package.json', key: 'version', type: 'json' },
  { path: 'server/vcpkg.json', key: 'version-string', type: 'json' },
  { path: 'docs-site/app/app.vue', key: null, type: 'vue' }
];

function readVersion() {
  const packageJsonPath = path.join(REPO_ROOT, 'package.json');
  const pkg = JSON.parse(fs.readFileSync(packageJsonPath, 'utf8'));
  return pkg.version;
}

function parseVersion(versionStr) {
  const match = versionStr.match(/^(\d+)\.(\d+)\.(\d+)$/);
  if (!match) {
    throw new Error(`Invalid version format: ${versionStr}. Expected X.Y.Z`);
  }
  return {
    major: parseInt(match[1]),
    minor: parseInt(match[2]),
    patch: parseInt(match[3])
  };
}

function bumpVersion(currentVersion, bumpType) {
  const ver = parseVersion(currentVersion);

  switch (bumpType) {
    case 'major':
      ver.major++;
      ver.minor = 0;
      ver.patch = 0;
      break;
    case 'minor':
      ver.minor++;
      ver.patch = 0;
      break;
    case 'patch':
      ver.patch++;
      break;
    default:
      throw new Error(`Invalid bump type: ${bumpType}. Expected major|minor|patch`);
  }

  return `${ver.major}.${ver.minor}.${ver.patch}`;
}

function updateJsonFile(filePath, key, newVersion) {
  const fullPath = path.join(REPO_ROOT, filePath);
  const content = JSON.parse(fs.readFileSync(fullPath, 'utf8'));
  content[key] = newVersion;
  fs.writeFileSync(fullPath, JSON.stringify(content, null, 2) + '\n', 'utf8');
  console.log(`✓ Updated ${filePath}`);
}

function updateVueFile(filePath, newVersion) {
  const fullPath = path.join(REPO_ROOT, filePath);
  let content = fs.readFileSync(fullPath, 'utf8');

  content = content.replace(
    /const version = ref\(['"][\d.]+['"]\)/,
    `const version = ref('${newVersion}')`
  );

  fs.writeFileSync(fullPath, content, 'utf8');
  console.log(`✓ Updated ${filePath}`);
}

function setVersion(newVersion) {
  parseVersion(newVersion);

  for (const file of FILES_TO_UPDATE) {
    if (file.type === 'json') {
      updateJsonFile(file.path, file.key, newVersion);
    } else if (file.type === 'vue') {
      updateVueFile(file.path, newVersion);
    }
  }

  console.log(`\n✨ Version set to ${newVersion}`);
}

function bumpVersionAcross(bumpType) {
  const currentVersion = readVersion();
  const newVersion = bumpVersion(currentVersion, bumpType);

  console.log(`Bumping ${bumpType}: ${currentVersion} → ${newVersion}\n`);
  setVersion(newVersion);
}

const args = process.argv.slice(2);

if (args.length === 0) {
  console.error('Usage:');
  console.error('  npm run version -- 2.0.0');
  console.error('  npm run bump -- major|minor|patch');
  process.exit(1);
}

if (args[0] === 'major' || args[0] === 'minor' || args[0] === 'patch') {
  bumpVersionAcross(args[0]);
} else {
  setVersion(args[0]);
}
