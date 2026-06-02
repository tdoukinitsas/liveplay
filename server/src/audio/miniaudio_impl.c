/*
 * miniaudio_impl.c
 * ----------------------------------------------------------------------------
 * Single translation unit that compiles miniaudio's implementation. Every
 * other file in the project includes <miniaudio.h> as a normal header — only
 * this file defines MINIAUDIO_IMPLEMENTATION, so the library is built exactly
 * once.
 *
 * Cross-platform notes:
 *   - Windows: WASAPI is the default backend (lowest latency on Win10+).
 *   - macOS:   Core Audio is the default backend.
 *   - Linux:   ALSA, PulseAudio and JACK are compiled in. miniaudio dlopen()s
 *              whichever are present at runtime, so we only link -ldl — no
 *              libpulse-dev / libjack-dev needed on the build machine.
 *
 * Decoder configuration here covers the formats LivePlay needs out of the
 * gate: WAV, FLAC, MP3, and (via a built-in stb_vorbis fork) OGG/Vorbis.
 * ----------------------------------------------------------------------------
 */

/* Enable the built-in decoders. Order matters for some platforms — keep these
 * before MINIAUDIO_IMPLEMENTATION. */
#define MA_ENABLE_WAV
#define MA_ENABLE_FLAC
#define MA_ENABLE_MP3

/* On Linux, leave miniaudio's runtime linking ON (the default). It compiles
 * in the ALSA, PulseAudio and JACK backends and dlopen()s whichever are
 * available at runtime. Defining MA_NO_RUNTIME_LINKING would force every
 * backend's symbols (jack_*, pa_*) to be resolved at link time, which fails
 * unless libjack-dev / libpulse-dev are linked via CMake. */

/* Implementation. Must appear in exactly one TU per binary. */
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
