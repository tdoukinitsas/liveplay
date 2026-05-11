## Why

Users have no way to bulk-import tracks from an existing Spotify playlist into LivePlay. Building a show playlist requires manually finding and importing each track individually, which is tedious when working from a pre-existing Spotify setlist with dozens of tracks.

## What Changes

- Add "Import from Spotify" menu item in the playlist header that opens a multi-step modal workflow
- Scrape Spotify playlist metadata (track names, artists, album art) from a pasted URL using `spotify-url-info` (zero-config, no API credentials needed)
- For each track, auto-search YouTube using yt-dlp's `ytsearch:` capability to find the best audio match
- Interactive review UI where users can see all tracks with their YouTube match, preview matches, cherry-pick which tracks to import, and skip unwanted ones
- Download selected tracks via the existing yt-dlp integration and add them to the project playlist
- Progress tracking for the batch download with ability to cancel

## Capabilities

### New Capabilities
- `spotify-import`: Spotify playlist URL scraping, YouTube match searching, interactive track review and selection, batch download and playlist insertion

### Modified Capabilities
<!-- None — this is a new standalone feature that uses existing yt-dlp download and playlist infrastructure without changing their requirements -->

## Impact

- **New dependency:** `spotify-url-info` npm package (scrapes Spotify embed pages, no API key required)
- **Existing code reused:** yt-dlp download pipeline from `electron/main.js`, playlist item creation from `useProject`
- **New components:** SpotifyImportModal (multi-step), SpotifyTrackRow (per-track review)
- **New main-process IPC:** YouTube search handler (yt-dlp `--dump-json ytsearch:"query"`)
- **Risk:** `spotify-url-info` scraping approach may break if Spotify changes their embed page format; can be upgraded to OAuth PKCE flow later
