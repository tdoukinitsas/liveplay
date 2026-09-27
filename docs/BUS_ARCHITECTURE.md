# Bus architecture

The server renders a directed audio graph. Project buses describe the show; the server output map connects logical names to physical channels.

[API bus/DSP schemas](https://tdoukinitsas.github.io/liveplay/api/#schema-bus) · [Ownership](OWNERSHIP_MODEL.md) · [Audio checks](../server/tests/e2e/README.md)

## Signal path

```text
audio items → bus sum → HPF → LPF → EQ → gate → compressor → stereo width
                                                              ↓
                              post-processing / pre-fader PFL and pre sends
                                                              ↓
                                             fader + mute → post sends
                                                              ↓
                                  pan/balance → bus or hardware output
```

Bus routes and auxiliary sends are ordered topologically so a downstream bus consumes its inputs within the render block. The graph is validated for cycles on the control side; topology construction also handles invalid edges defensively.

## Assignment and identity

- A cue's explicit bus assignment wins; otherwise it inherits the nearest ancestor group's assignment, then the master-role bus.
- Project bus IDs are stable document identity. Engine mixer IDs are runtime resources.
- Buses are mono or stereo. Pan/balance and lane mapping are applied by the engine; a stereo-to-mono fold uses the engine downmix law.
- A bus can output to another bus, a logical hardware output, or nowhere.
- Auxiliary sends target other buses and tap before or after the fader/mute.
- A bus with no main output can still feed auxiliary sends.
- Persist routes, DSP and sends in the project. PFL and mono-check are live monitoring state.

See the API schemas for exact route objects, gains, limits, fallback behavior and mutation responses.

## Master and Preview roles

Master and Preview are roles assigned to ordinary buses. Each role has one holder. A bus cannot hold both roles, and a role holder must route directly to an output.

| Role | Behavior |
|---|---|
| Master | Default destination for unassigned cues; owns the main output pair |
| Preview | Receives cue pre-listen and bus PFL; owns the reserved preview pair |

The Preview bus cannot receive ordinary bus routes or sends and cannot send to other buses. Role holders cannot simply be deleted or have their role cleared; move the role to another valid bus.

PFL is post-DSP and pre-fader/pre-mute. It lets the operator listen to a muted bus. Multiple buses can be PFL'd together without altering their program output. Preview has its own fader and output mapping.

## Hardware binding

| Logical output condition | Engine behavior |
|---|---|
| Main Out without a mapping | May bind to the default device |
| Preview Out without a mapping | Silent |
| Explicit mapping to present hardware | Bind configured channels |
| Mapping or direct device name with no matching hardware | Unbound/silent |
| Bus output reaches another bus | Binding reflects the terminal route |
| No output | No hardware destination |

Mappings live in server-owned `outputs.json`; bus output names live in the project. Hardware mapping is edited in **Settings → Outputs**. Bus output choices are edited in the mixer.

LTC also resolves an output name. An unresolved LTC destination is silent. Do not infer that every unbound output falls back to the main sound card.

## DSP and meters

The fixed DSP chain has up to 32 EQ bands, high/low-pass filters, gate/expander, compressor and stereo width/bass-mono processing. Gate and compressor detection are stereo-linked. The bus compressor has no lookahead; the master limiter is a separate processor.

DSP coefficients are calculated on the control side, published through double-buffered parameter slots and read by the render thread. Filter coefficient ramps reduce discontinuities. Bypass/flat chains can avoid unnecessary processing.

Live pan/DSP controls update the engine without persisting the edit. Durable changes use a bus patch. Each live DSP request merges with stored bus DSP, so clients must not assume independent unpersisted partial requests accumulate.

Meters expose lane and aggregate values, peak/RMS/true-peak data, loudness energy, gain reduction and correlation. The authoritative field units and message layout are in [WebSocket metering](https://tdoukinitsas.github.io/liveplay/api/#event-meters).

## Scheduling and output devices

- Graph topology is prepared away from the render thread and published as a snapshot.
- Mixer accumulators and device buffers are allocated outside the audio callback.
- The device carrying the main pair is the output clock reference; other devices use drift compensation.
- Render block size and output ring depth affect latency. Measurements depend on hardware/backend, so this guide does not promise one fixed end-to-end latency.
- A device must be available for integration tests to exercise real rendering.

## Implementation map

| Source | Role |
|---|---|
| `server/src/core/project_state.cpp` | Bus validation, persistence, inheritance and engine materialization |
| `server/src/audio/engine.cpp` | Graph render order, sends, routing and device rings |
| `server/include/liveplay/audio/channel_dsp.hpp` | Fixed DSP chain and coefficient publication |
| `server/include/liveplay/audio/biquad.hpp` | Filter design and processing |
| `server/include/liveplay/audio/dynamics.hpp` | Gate and compressor |
| `server/include/liveplay/audio/stereo_width.hpp` | Width, bass mono and correlation |
| `client/app/components/MixerPanel.vue` | Mixer workspace |
| `client/app/components/MixerEqPanel.vue` | EQ controls and curve |
| `client/app/components/MixerDynamicsPanel.vue` | Dynamics controls and display |
| `client/app/components/StereoMeter.vue` | Shared meter UI |

The plugin rack UI is a placeholder, not an implemented third-party plugin host.
