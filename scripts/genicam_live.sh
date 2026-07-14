#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build-standard"
CAMERA_IP="${GENICAM_CAMERA_IP:-192.168.9.104}"
EXPOSURE_US="${1:-2000}"

if ! command -v pkg-config >/dev/null || ! pkg-config --exists aravis-0.8; then
  echo "Aravis is required: brew install aravis" >&2
  exit 1
fi
if ! pkg-config --exists sdl2; then
  echo "SDL2 is required: brew install sdl2" >&2
  exit 1
fi

if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
  cmake -S "$ROOT" -B "$BUILD"
fi
cmake --build "$BUILD" --target genicam-live -j4

echo "Opening $CAMERA_IP using GigE Vision/GenICam (exposure ${EXPOSURE_US} us)"
"$BUILD/genicam-live" "$CAMERA_IP" "$EXPOSURE_US"
