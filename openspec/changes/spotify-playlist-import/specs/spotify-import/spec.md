## ADDED Requirements

### Requirement: Fetch Spotify playlist metadata from URL
The system SHALL accept a Spotify playlist URL and extract track metadata (title, artist, album, duration, album art URL) using the `spotify-url-info` package in the main process.

#### Scenario: Valid public playlist URL
- **WHEN** the user submits a valid Spotify playlist URL (e.g., `https://open.spotify.com/playlist/...`)
- **THEN** the system returns the playlist name and an array of tracks with title, artist, album, duration, and album art URL

#### Scenario: Invalid or private playlist URL
- **WHEN** the user submits an invalid URL or a private playlist URL
- **THEN** the system displays a clear error message indicating the URL could not be read

#### Scenario: Empty playlist
- **WHEN** the user submits a URL for a playlist with no tracks
- **THEN** the system displays a message that the playlist is empty

### Requirement: Search YouTube for each track
The system SHALL search YouTube for each Spotify track using yt-dlp's `ytsearch:` with the query `"artist - title"` and return the top match metadata (video ID, title, channel, duration, thumbnail URL).

#### Scenario: Successful match found
- **WHEN** YouTube search returns results for a track
- **THEN** the system displays the top match with video title, channel name, duration, and thumbnail

#### Scenario: No match found
- **WHEN** YouTube search returns no results for a track
- **THEN** the track row displays "No match found" and the track is not selectable for download

#### Scenario: Progressive search results
- **WHEN** searching YouTube for a playlist with many tracks
- **THEN** the system displays results progressively as each search completes, without waiting for all searches to finish

### Requirement: Interactive track review and selection
The system SHALL display all Spotify tracks with their YouTube matches in a scrollable list. Each track row SHALL show the Spotify track info (title, artist) alongside the YouTube match info (video title, duration). Users SHALL be able to select or deselect individual tracks for download.

#### Scenario: Select all tracks
- **WHEN** the user clicks "Select All"
- **THEN** all tracks with valid YouTube matches are selected for download

#### Scenario: Deselect individual track
- **WHEN** the user unchecks a specific track
- **THEN** that track is excluded from the download batch

#### Scenario: Select none
- **WHEN** no tracks are selected
- **THEN** the "Download" button is disabled

### Requirement: Batch download selected tracks
The system SHALL download all selected tracks sequentially using the existing `download-youtube-audio` IPC handler. The modal SHALL display overall progress (N of M tracks) and per-track download progress.

#### Scenario: Successful batch download
- **WHEN** the user clicks "Download Selected" with N tracks selected
- **THEN** each track is downloaded sequentially, progress updates are shown per-track, and completed tracks are added to the project playlist

#### Scenario: Download failure for one track
- **WHEN** a single track fails to download
- **THEN** the system marks that track as failed, continues with remaining tracks, and shows a summary of successes and failures at completion

#### Scenario: Cancel download
- **WHEN** the user clicks "Cancel" during batch download
- **THEN** the current download is aborted, remaining tracks are skipped, and already-downloaded tracks remain in the playlist

### Requirement: Add downloaded tracks to playlist
Each successfully downloaded track SHALL be added to the project playlist as an `AudioItem` using the existing `addItem()` flow from `useProject`, with waveform generation triggered automatically.

#### Scenario: Track added after download
- **WHEN** a track finishes downloading
- **THEN** the audio file is added to the playlist with the Spotify track name as the display name, and waveform generation begins

### Requirement: Multi-step modal UI
The system SHALL present the Spotify import as a modal with three steps: (1) URL input, (2) track review and selection, (3) download progress. The modal SHALL be accessible from a button in the PlaylistView header.

#### Scenario: Opening the modal
- **WHEN** the user clicks the "Import from Spotify" button in the playlist header
- **THEN** the modal opens showing the URL input step

#### Scenario: Navigating between steps
- **WHEN** the user submits a valid URL in step 1
- **THEN** the modal transitions to step 2 (track review) and begins searching YouTube for matches

#### Scenario: Closing the modal
- **WHEN** the user closes the modal after downloading some tracks
- **THEN** the modal closes and all downloaded tracks remain in the playlist
