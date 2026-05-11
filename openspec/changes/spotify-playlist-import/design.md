## Context

LivePlay already has a YouTube import flow: `YouTubeImportModal.vue` lets users search YouTube, preview results, and download audio via yt-dlp. The modal is mounted in `PlaylistView.vue`. The main process exposes `search-youtube` (via `youtube-search-api`) and `download-youtube-audio` IPC handlers. Downloaded files are added to the playlist via `useProject().addItem()`.

The Spotify import feature builds on this infrastructure — it reuses yt-dlp for downloading and the same `addItem()` flow for playlist insertion. The new part is scraping a Spotify playlist URL to get track metadata, then auto-searching YouTube for each track.

## Goals / Non-Goals

**Goals:**
- Import tracks from a Spotify playlist URL into a LivePlay project
- Interactive review: user sees all tracks with YouTube matches, can cherry-pick which to download
- Batch download with progress tracking and cancel support
- Zero-config: no Spotify API credentials required from the user

**Non-Goals:**
- Spotify OAuth login or private playlist access (future upgrade path)
- MIDI/gamepad input or other non-related features
- Spotify album or artist page import (only playlists)
- Audio preview/playback within the import modal before downloading

## Decisions

### 1. Use `spotify-url-info` for playlist scraping

**Decision:** Use the `spotify-url-info` npm package to extract track metadata from a Spotify playlist URL.

**Rationale:** Zero-config — no API keys, no user login. It scrapes Spotify's embed/oEmbed pages. The package is mature and widely used.

**Alternatives considered:** Spotify Web API with client credentials (requires user to create a Spotify developer account — too much friction); Spotify OAuth PKCE (requires app registration and a client ID baked into the app — good upgrade path but overkill for v1); raw HTTP scraping (fragile, reinventing the wheel).

**Risk:** Scraping can break when Spotify changes their embed format. Mitigation: the feature degrades gracefully with a clear error message, and can be upgraded to OAuth later.

### 2. YouTube search via yt-dlp `ytsearch:`

**Decision:** Use yt-dlp's built-in `ytsearch:` prefix with `--dump-json` to find YouTube matches, rather than the existing `youtube-search-api` package.

**Rationale:** yt-dlp's search is more reliable for finding audio content and returns richer metadata (duration, view count). It also avoids an additional dependency. The search query will be `"artist - title"` which reliably finds the right track.

**Alternatives considered:** Reusing the existing `search-youtube` IPC handler (returns video results but the search quality for "artist - title" queries is equivalent; either works). Using YouTube Data API (requires API key — adds friction).

### 3. Multi-step modal UI in PlaylistView

**Decision:** Create `SpotifyImportModal.vue` mounted in `PlaylistView.vue` alongside the existing `YouTubeImportModal`. The modal has three steps: (1) paste URL, (2) review matches, (3) download progress.

**Rationale:** Follows the existing pattern. Multi-step keeps each screen focused and manageable.

### 4. New IPC handlers in main process

**Decision:** Add two new IPC handlers:
- `fetch-spotify-playlist`: takes a Spotify URL, returns `{ name, tracks: [{ title, artist, album, duration, albumArt }] }`
- `search-youtube-for-track`: takes a search query string, returns the top YouTube match metadata via `yt-dlp --dump-json "ytsearch:query"`

Download reuses the existing `download-youtube-audio` handler.

**Rationale:** Spotify scraping must run in the main process (Node.js) since `spotify-url-info` uses `fetch`. YouTube search via yt-dlp also needs the main process. Keeping them as separate handlers allows independent testing and reuse.

### 5. Batch download with existing infrastructure

**Decision:** Reuse the existing `download-youtube-audio` IPC handler and progress channel for each selected track. Downloads run sequentially (not parallel) to avoid overwhelming yt-dlp/YouTube.

**Rationale:** Sequential downloads are simpler, more reliable, and avoid rate limiting. The existing progress reporting works per-video, so the modal tracks overall progress (N of M tracks) plus per-track progress.

### 6. Spotify scraping runs in main process

**Decision:** `spotify-url-info` runs in the Electron main process, exposed via IPC.

**Rationale:** The package uses Node.js `fetch` and can't run in the renderer's sandboxed context. This matches the existing pattern where yt-dlp operations are main-process IPC handlers.

## Risks / Trade-offs

- **[Scraping fragility]** → `spotify-url-info` may break on Spotify changes. Mitigation: clear error messaging; upgrade path to OAuth PKCE documented in proposal.
- **[Search quality]** → YouTube search may return wrong matches for some tracks (covers, remixes, karaoke versions). Mitigation: interactive review lets users skip or manually search alternatives.
- **[Large playlists]** → Searching YouTube for 100+ tracks takes time (~1-2s per search). Mitigation: show progressive results as they arrive; allow user to start selecting before all searches complete.
- **[Rate limiting]** → yt-dlp YouTube searches or downloads may get rate-limited. Mitigation: sequential execution with delays; clear error handling per track.
