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
node server/tests/e2e/comp-e2e.js       4500 /tmp/liveplay-test-signal.wav

# The stereo-image checks need their OWN signal: the one above is identical on
# both lanes, so it has no side content for a width control to act on.
node server/tests/e2e/gen-wide-signal.js /tmp/liveplay-wide-signal.wav
node server/tests/e2e/width-e2e.js      4500 /tmp/liveplay-wide-signal.wav
node server/tests/e2e/reroute-e2e.js    4500 /tmp/liveplay-test-signal.wav
node server/tests/e2e/save-churn.js     4500 /tmp/liveplay-test-signal.wav <projDir> <serverLog>
node server/tests/e2e/materialise-skip.js 4500
```

The server holds the wav open while a project referencing it is loaded, so **stop the server
before regenerating the signal** or the write fails with EBUSY and the old file is used silently.

Exit code is non-zero if anything failed; each assertion prints PASS/FAIL with the levels it
measured, so a failure says *how far* out it was rather than just that it was.

## The latency probes

`latency-probe.js` and `control-latency-probe.js` are **measurements, not pass/fail harnesses** —
they print numbers and exit 0. They exist because the engine's output latency is not readable from
its configuration: the ring's depth is the latency, the device overrides the period it was asked
for, and whether any of it is safe depends on how long a render block actually takes.

```sh
# Output latency and headroom, ramping up to 8 files, then a 90 s soak.
node server/tests/e2e/latency-probe.js 4500 /tmp/liveplay-test-signal.wav 8 90

# The control path: one REST call per pointer event, at 60/s for 3 s.
node server/tests/e2e/control-latency-probe.js 4500 60 3
```

`ui-churn-probe.js` asks a different question: does editing item properties disturb playing audio?

```sh
# 6 edits/s for 8 s per window, against a 60-item project, one file looping.
node server/tests/e2e/ui-churn-probe.js 4500 /tmp/liveplay-test-signal.wav 6 8 60
```

It watches for both failure modes, which is the lesson from writing it. The first version tracked
only the *maximum* master peak, looking for the overshoot a step discontinuity makes — and reported
everything clean. **A dropout is silence, and silence is not loud.** It now tracks the minimum
per-frame peak too, so a gap is as visible as a click. Any probe for "did the audio glitch" that
only looks upward is blind to half of it.

Both read `GET /api/engine/stats`, which reports queued frames, the device's *actual* period,
render-block time against its budget, and underruns. `?reset=1` clears the peak so a probe can
bound a window.

Two things to know before reading the output. **The ramp's per-step window is far too short to
justify a latency default** — a stall that happens once a minute will not appear in two seconds, so
use the soak for that. And **the shallowest queue depth is the number that matters**, not the
average: it is how close the device came to running dry, which an average hides completely.

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
- **Wait out the PROCESSOR's own time constants too, not just the meter's.** Only transitions
  *out* of gain reduction are slow — attack is milliseconds — so a step down from 9 dB of
  reduction with a 1000 ms release was still 0.6 dB down when a 2 s settle expired. That read as a
  hard knee doing 0.7 dB of work it should not have been doing. Either settle for several time
  constants or set a release the harness can afford to wait for.
- **`peak_db` is not the peak a dynamics detector sees.** It is ballistically released, so a
  threshold compared against it is being compared against the wrong quantity; `peak_max_db` is the
  raw sample maximum and cannot miss a transient however slowly the harness polls. On a
  constant-amplitude signal the two agree and it does not matter — which is exactly why it is
  worth fixing before a test uses a signal where they do not.
- **Measure a control where it is actually applied.** Width is per-sample DSP inside the strip, so
  it shows on the BUS meter; balance is nothing but the two sends to the master, so it shows on the
  MASTER meter and not on the bus meter at all. Checking either in the other's place reads as the
  control doing nothing.
- **Relative assertions can all shift together.** Every balance check in `width-e2e.js` was
  measured against the centre reading, so swapping in the pan law moved the whole set 3 dB down
  and only one assertion noticed. One absolute check — the bus's own peak against the house — pins
  the law rather than its symmetry.
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
