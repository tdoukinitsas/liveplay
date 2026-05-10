## Why

YouTube download fails with "missing ffmpeg" even when ffmpeg is installed system-wide, because `checkAndSetupFfmpeg()` only checks the bundled `@ffmpeg-installer/ffmpeg` binary and never falls back to the system PATH.

## What Changes

- Add system PATH fallback to `checkAndSetupFfmpeg()` when the bundled ffmpeg binary fails
- Use `which ffmpeg` (Unix) / `where ffmpeg` (Windows) to locate system binary
- Apply the same fallback pattern for ffprobe
- Verify discovered system binaries with `-version` before accepting them

## Capabilities

### New Capabilities

_None — this is a bugfix to existing behavior._

### Modified Capabilities

_None — no existing specs exist and no spec-level requirements are changing._

## Impact

- **Code**: `electron/main.js`, function `checkAndSetupFfmpeg()` (lines 19-59)
- **Dependencies**: No new dependencies
- **Platforms**: Must work on macOS, Linux, and Windows (different `which`/`where` commands)
