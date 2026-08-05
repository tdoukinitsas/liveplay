# End-to-end audio checks

Unlike the other test binaries here, these drive a **running server** over REST and WebSocket and
assert on what the meters read. They exist because the claims they check are behavioural — "PFL
never reaches the house", "the tap is pre-fader" — and nothing short of real audio through the
real render loop can confirm them. Both times PFL leaked into the house on this branch, the code
read correctly and the meters did not (see `BUS_ARCHITECTURE.md` §0.2b).

They need an audio device. There is no null backend, so on a machine with no playback device the
render thread idles and every level assertion reads silence.

## Running

```sh
# 1. Build the server.
npm run server:build

# 2. Generate the test signal (~23 MB, gitignored — do not commit it).
node server/tests/e2e/gen-signal.js /tmp/liveplay-test-signal.wav

# 3. Start a server on a spare port, then drive it.
server/build/Release/liveplay-server.exe --port 4500 &
node server/tests/e2e/pfl-e2e.js        4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/filters-e2e.js    4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/gate-e2e.js       4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/reroute-e2e.js    4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/save-churn.js     4500 /tmp/liveplay-test-signal.wav <projDir> <serverLog>
node server/tests/e2e/materialise-skip.js 4500
```

The server holds the wav open while a project referencing it is loaded, so **stop the server
before regenerating the signal** or the write fails with EBUSY and the old file is used silently.

Exit code is non-zero if anything failed; each assertion prints PASS/FAIL with the levels it
measured, so a failure says *how far* out it was rather than just that it was.

## Measuring DSP, specifically

Every "the processor is broken" result in this directory so far has turned out to be the
measurement. In order of how much time each one cost:

- **Average POWER, never decibels.** A mean of dB readings is a geometric mean and weights quiet
  frames far too heavily. Two identical flat chains read 0.4 dB apart until this was fixed.
- **Wait 1.5 s after a change before believing the meter.** The engine ramps coefficients over
  ~340 ms and the RMS meter integrates on top of that. At 700 ms the window still caught the tail
  of a +12 dB boost and read 0.3 dB hot — which looks exactly like a band failing to flatten.
- **The signal is a 1-second triangle sweep for a reason.** A whole number of sweep periods covers
  identical spectral content wherever the window starts; at five seconds a 1.2 s window sampled a
  different slice each time. It is a triangle rather than a sawtooth because a sawtooth's
  2 kHz → 200 Hz wrap is a broadband click a window can catch.
- **Choose thresholds with margin.** The gate closes 3 dB below its threshold, so a threshold of
  −3 dB against a −6 dBFS signal puts the close point exactly on the signal level, inside the
  hysteresis window. The gate correctly held open; the test called it a failure to gate.
- **Pick a corner that actually puts the signal in the stopband.** A 1 kHz low-pass leaves 44% of
  a 200 Hz–2 kHz sweep in the passband and takes about 2 dB off the total, which is the right
  answer and a poor test.
- **`POST /api/buses/<id>/dsp` does not persist.** It is the in-gesture path: it merges onto the
  *stored* bus and writes no document. Send the whole section, and `PATCH` first if a later
  assertion depends on the value being stored.

## Things worth knowing before adding assertions

- **"The house" is masters 0/1 specifically.** Maxing over every master channel silently stops
  meaning the house once Monitor is bound, because the reserved pair at the top of the bus is a
  master channel too. `Meters.housePeak()` and `Meters.monitorOutPeak()` are separate for that
  reason.
- **Master and strip meters fall back slowly (~4 dB/s).** Assert on level *changes* over a settle,
  not on an absolute floor a decaying meter will not reach inside the window. Running the script
  twice in a row leaves the previous run's tail on the reserved pair for tens of seconds.
- **The signal is a sweep, not a fixed tone.** The "PFL and pre-listen sum" assertion feeds the
  same file into the monitor twice; with a fixed tone the two are perfectly correlated and their
  sum depends on the arbitrary phase between two independently-started playbacks — it measured
  +3.9 dB one run and +0.6 dB the next. A sweep puts the two playback positions at different
  frequencies. The peak is unchanged, so every level assertion still reads −6 dBFS.
- **The script mutates server config.** `PUT /api/outputs` persists to `outputs.json` next to the
  binary, so the script puts the map back at the end. Without that, the second run disagrees with
  the first for reasons that have nothing to do with the code.
- **Prove a new assertion can fail.** Break the thing deliberately, rebuild, and watch it go red
  before trusting it. Every safety assertion here was confirmed that way.
