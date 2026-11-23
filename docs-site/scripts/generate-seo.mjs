// Generates public/sitemap.xml, public/llms.txt and public/llms-full.txt from the
// site's English copy, so what search engines and AI assistants read can't drift
// from the pages. Runs before dev and generate; the output is not committed.
import { execSync } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const json = file => JSON.parse(readFileSync(resolve(root, file), 'utf8'));
const en = json('public/locales/en.json');
const seo = json('app/data/seo.json');
const contributors = Object.values(json('public/contributors.json').contributors);
const { version } = json('public/package.json');
const { site, pages } = seo;

const releases = Object.keys(en.whatsNew).filter(k => /^v\d{2,}$/.test(k)).map((key) => {
  const slug = `${key[1]}-${key.slice(2)}`;
  return { key, slug, version: `${key[1]}.${key.slice(2)}`, copy: en.whatsNew[key], page: pages[slug] };
});
for (const r of releases) {
  if (!r.page) throw new Error(`app/data/seo.json has no page entry for release ${r.slug}`);
}
for (const page of Object.values(pages)) {
  if (!existsSync(resolve(root, `public/og/${page.og}.jpg`))) throw new Error(`Missing social card: public/og/${page.og}.jpg`);
}

// ---- Sitemap ------------------------------------------------------------------
let lastmod = '';
try {
  lastmod = execSync('git log -1 --format=%cs', { cwd: root, stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim();
} catch { /* no git history: the date is optional */ }

const urls = [
  site.url,
  site.url + pages.whatsNew.path,
  ...releases.map(r => site.url + r.page.path),
  `${site.url}api/`,
];
writeFileSync(resolve(root, 'public/sitemap.xml'), `<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
${urls.map(u => `  <url><loc>${u}</loc>${lastmod ? `<lastmod>${lastmod}</lastmod>` : ''}</url>`).join('\n')}
</urlset>
`);

// ---- llms.txt -----------------------------------------------------------------
const people = [site.author.name, ...contributors.map(c => c.name)].join(', ');
const facts = `- Current version: ${version}
- Price: free, with no paid tiers
- Licence: GNU AGPL-3.0
- Platforms: 64-bit Windows 10 and 11, macOS 13.3 or later (Apple Silicon and Intel), 64-bit Linux
- Audio formats: WAV, AIFF, FLAC, MP3
- Interface languages: 21
- Created by: ${site.author.name}
- Contributors: ${people}`;

writeFileSync(resolve(root, 'public/llms.txt'), `# ${site.name}

> ${site.summary}

${facts}

## Product

- [Feature tour](${site.url}): Everything LivePlay does, written for someone new to it
- [Download](${site.url}#download): Installers for Windows, macOS and Linux
- [Questions and answers](${site.url}#faq): Price, platforms, formats, show control and networking
- [Complete product description](${site.url}llms-full.txt): All features and release notes as plain text

## Release notes

- [What's new](${site.url}${pages.whatsNew.path}): Every release, newest first
${releases.map(r => `- [LivePlay ${r.version}](${site.url}${r.page.path}): ${r.page.description}`).join('\n')}

## Developers

- [API reference](${site.url}api/): HTTP endpoints, WebSocket commands, events and integration examples
- [API catalog (JSON)](${site.url}api/reference.json): Machine-readable contract for the API
- [Source code](${site.repo}): GitHub repository

## Optional

- [All releases](${site.repo}/releases): Downloads and full changelogs on GitHub
`);

// ---- llms-full.txt ------------------------------------------------------------
// Chapters and their features, in the order the home page shows them.
const tour = readFileSync(resolve(root, 'app/data/tour.ts'), 'utf8');
const chapters = [];
for (const [, group, key] of tour.matchAll(/group: '(\w+)'|key: '(\w+)'/g)) {
  if (group) chapters.push({ group, keys: [] });
  else if (key && chapters.length) chapters.at(-1).keys.push(key);
}
const featureText = chapters.map(({ group, keys }) => `### ${en.features[group].title}

${en.features[group].intro}

${keys.map(k => `#### ${en.features[k].title}

${en.features[k].description}`).join('\n\n')}`).join('\n\n');

const releaseText = releases.map(({ version: v, copy }) => `### LivePlay ${v}: ${copy.title}

${copy.lead}

${Object.values(copy.sections).map(s => `#### ${s.kicker}: ${s.title}

${s.text}

${s.points.map(p => `- ${p}`).join('\n')}`).join('\n\n')}

#### More in this release

${Object.values(copy.more).map(m => `- ${m.title}: ${m.text}`).join('\n')}

Also fixed: ${copy.fixes}`).join('\n\n');

const earlier = Object.entries(en.whatsNew.timeline).map(([k, e]) =>
  `- LivePlay ${k[1]}.${k.slice(2)}, ${e.title}: ${e.text}`).join('\n');

writeFileSync(resolve(root, 'public/llms-full.txt'), `# ${site.name}

> ${site.summary}

Website: ${site.url}
Source: ${site.repo}
Download: ${site.releases}

${facts}

## Questions and answers

${seo.faq.map(({ q, a }) => `### ${q}\n\n${a}`).join('\n\n')}

## Features

${featureText}

## Release notes

${releaseText}

### Earlier releases

${earlier}

## Network ports

- ${en.ports.control.port}: ${en.ports.control.description}
- ${en.ports.discovery.port}: ${en.ports.discovery.description}

${en.ports.note}

## API

${en.api.description}

Reference: ${site.url}api/
`);

console.log(`[generate-seo] sitemap (${urls.length} urls), llms.txt, llms-full.txt`);
