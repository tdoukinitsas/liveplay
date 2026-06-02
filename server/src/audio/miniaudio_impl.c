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
 *   - Linux:   ALSA + PulseAudio are compiled in; miniaudio picks at runtime.
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

/* We don't need miniaudio's runtime linking on Linux — ALSA/Pulse are linked
 * via CMake. This trims the binary slightly. */
#if !defined(_WIN32) && !defined(__APPLE__)
    #define MA_NO_RUNTIME_LINKING
#endif

/* Implementation. Must appear in exactly one TU per binary. */
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
