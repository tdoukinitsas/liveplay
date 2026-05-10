## Context

`checkAndSetupFfmpeg()` in `electron/main.js:19-59` tries to load the bundled `@ffmpeg-installer/ffmpeg` package and verify it with `-version`. If that fails (e.g., missing native binary for the platform, asar packaging issue), the outer catch sets `ffmpegAvailable = false` with no further attempts. A system-installed ffmpeg is ignored entirely.

## Goals / Non-Goals

**Goals:**
- Fall back to system PATH ffmpeg/ffprobe when bundled binaries fail
- Work cross-platform (macOS/Linux: `which`, Windows: `where`)
- Maintain bundled binary as first preference

**Non-Goals:**
- Downloading ffmpeg automatically
- Prompting the user to install ffmpeg
- Changing how ffmpeg is used after setup

## Decisions

**1. Fallback in the existing catch block**
Add the system lookup inside the existing `catch (error)` block at line 54. This keeps the bundled-first preference and only triggers when bundled fails. Alternative: separate function — unnecessary complexity for a simple fallback.

**2. Use `which`/`where` via `execPromise`**
`execPromise` is already available at module scope (line 12). `which` works on macOS and Linux; `where` is the Windows equivalent. The command output gives the absolute path. Alternative: `require('which')` package — adds a dependency for no benefit.

**3. Verify with `-version` before accepting**
Run `"<path>" -version` on the discovered binary to confirm it's functional, same pattern the bundled check uses at line 30.

## Risks / Trade-offs

- **[`which` not found on exotic shells]** → Extremely unlikely on any supported Electron platform; acceptable risk.
- **[System ffmpeg too old]** → Out of scope; if it responds to `-version`, accept it. yt-dlp handles version compatibility separately.
