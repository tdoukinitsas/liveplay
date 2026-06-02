// Copies the canonical screenshots from the client app into the docs-site public/
// folder so they are served at <base>/screenshots/*. The images live in the client
// app (client/public/screenshots) and are the single source of truth; we copy them
// here at build time rather than committing duplicates. Runs before `nuxt generate`
// both locally and in CI (see package.json scripts and the deploy-docs workflow).
import { cp, mkdir, rm } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const src = resolve(here, '../../client/public/screenshots');
const dest = resolve(here, '../public/screenshots');

if (!existsSync(src)) {
  console.warn(`[copy-screenshots] source not found, skipping: ${src}`);
  process.exit(0);
}

await rm(dest, { recursive: true, force: true });
await mkdir(dest, { recursive: true });
await cp(src, dest, { recursive: true });
console.log(`[copy-screenshots] copied ${src} -> ${dest}`);
