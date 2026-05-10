## 1. System ffmpeg fallback

- [x] 1.1 In the catch block of `checkAndSetupFfmpeg()` (line 54), add system PATH lookup using `execPromise` with `which ffmpeg` (Unix) or `where ffmpeg` (Windows)
- [x] 1.2 On success, trim the output to get the path, verify with `"<path>" -version`, set `ffmpegPath` and `ffmpegAvailable = true`
- [x] 1.3 Log which path is being used (`console.log('Using system ffmpeg:', path)`)

## 2. System ffprobe fallback

- [x] 2.1 In the inner catch block for ffprobe (line 43), after the existing adjacent-directory fallback fails, add system PATH lookup using `which ffprobe` / `where ffprobe`
- [x] 2.2 On success, call `ffmpeg.setFfprobePath()` with the discovered path and log it

## 3. Verification

- [ ] 3.1 Test with bundled ffmpeg removed/broken to confirm system fallback activates (manual)
- [ ] 3.2 Test on target platform that YouTube download works with system ffmpeg (manual)
