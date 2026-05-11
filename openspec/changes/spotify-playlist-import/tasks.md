## 1. Dependencies & IPC

- [ ] 1.1 Install `spotify-url-info` package
- [ ] 1.2 Add `fetch-spotify-playlist` IPC handler in `electron/main.js` — accept Spotify playlist URL, return `{ name, tracks: [{ title, artist, album, duration, albumArt }] }`
- [ ] 1.3 Add `search-youtube-for-track` IPC handler in `electron/main.js` — accept query string, run `yt-dlp --dump-json "ytsearch:query"`, return `{ videoId, title, channel, duration, thumbnail }`
- [ ] 1.4 Expose both new IPC channels in `preload.js` and `electronAPI` type declarations

## 2. SpotifyImportModal Component

- [ ] 2.1 Create `components/SpotifyImportModal.vue` with three-step layout (URL input → track review → download progress)
- [ ] 2.2 Step 1: URL input with paste field and "Fetch Playlist" button; show loading state and error handling
- [ ] 2.3 Step 2: Track review list — show Spotify info (title, artist) alongside YouTube match (video title, duration, thumbnail); checkboxes for selection; "Select All" / "Deselect All" buttons; progressive loading as YouTube searches complete
- [ ] 2.4 Step 3: Download progress — overall progress (N/M tracks), per-track progress bar, cancel button
- [ ] 2.5 Wire download to existing `download-youtube-audio` IPC; add downloaded tracks to playlist via `addItem()`

## 3. Integration

- [ ] 3.1 Add "Import from Spotify" button to `PlaylistView.vue` header alongside existing YouTube import button
- [ ] 3.2 Mount `SpotifyImportModal` in `PlaylistView.vue` with `v-if` pattern matching `YouTubeImportModal`

## 4. Localization

- [ ] 4.1 Add Spotify import strings to `locales/en.json` (modal title, step labels, button text, error messages)

## 5. Testing

- [ ] 5.1 Test with a valid public Spotify playlist URL (manual)
- [ ] 5.2 Test with invalid/private playlist URL — verify error message (manual)
- [ ] 5.3 Test cherry-picking tracks and downloading subset (manual)
- [ ] 5.4 Test cancelling mid-download — verify partial downloads remain (manual)
- [ ] 5.5 Test large playlist (50+ tracks) — verify progressive search results (manual)
