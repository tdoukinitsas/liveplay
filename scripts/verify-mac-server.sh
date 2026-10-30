#!/usr/bin/env bash
# =============================================================================
# scripts/verify-mac-server.sh <liveplay-server binary> <x86_64|arm64>
# -----------------------------------------------------------------------------
# Static checks on a macOS server binary, run by CI before anything ships:
#   * it is a single-arch Mach-O of the expected architecture (issue #81: the
#     Intel DMG had shipped an arm64 server since 2.0.5);
#   * its minimum OS is no newer than the 13.3 floor in server/CMakeLists.txt;
#   * it imports no libc++ symbol that only newer macOS dylibs export.
#     __hash_memory is the one that crashed 2.5.x before main() on macOS < 26.
# A --help run on an older macOS (see the workflows) covers what this can't.
# =============================================================================
set -euo pipefail

bin="${1:?usage: verify-mac-server.sh <binary> <x86_64|arm64>}"
want_arch="${2:?usage: verify-mac-server.sh <binary> <x86_64|arm64>}"
max_minos="13.3"

[ -f "$bin" ] || { echo "::error::server binary not found: $bin"; exit 1; }

archs="$(lipo -archs "$bin")"
echo "archs:  $archs"
if [ "$archs" != "$want_arch" ]; then
  echo "::error::$bin is '$archs', expected '$want_arch'"
  exit 1
fi

minos="$(otool -l "$bin" | awk '/LC_BUILD_VERSION/{f=1} f&&$1=="minos"{print $2; exit}')"
echo "minos:  ${minos:-unknown}"
if [ -z "$minos" ]; then
  echo "::error::no LC_BUILD_VERSION minos in $bin"
  exit 1
fi
# Version compare via sort -V: the floor must sort last (or tie).
if [ "$(printf '%s\n%s\n' "$minos" "$max_minos" | sort -V | tail -n1)" != "$max_minos" ]; then
  echo "::error::$bin needs macOS $minos, newer than the $max_minos floor"
  exit 1
fi

if nm -m "$bin" | grep -q '__hash_memory'; then
  echo "::error::$bin imports std::__hash_memory, which macOS < 26 lacks (issue #81)"
  nm -m "$bin" | grep '__hash_memory' || true
  exit 1
fi

echo "OK: $bin ($archs, macOS >= $minos)"
