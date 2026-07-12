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
if ! command -v ffplay >/dev/null; then
  echo "ffplay is required: brew install ffmpeg" >&2
  exit 1
fi

if [[ ! -x "$BUILD/genicam-live" ]]; then
  cmake -S "$ROOT" -B "$BUILD"
  cmake --build "$BUILD" --target genicam-live -j4
fi

echo "Opening $CAMERA_IP using GigE Vision/GenICam (exposure ${EXPOSURE_US} us)"
"$BUILD/genicam-live" "$CAMERA_IP" "$EXPOSURE_US" |
  ffplay -hide_banner -loglevel warning \
    -f rawvideo -pixel_format gray -video_size 2640x1968 -framerate 14 \
    -window_title "GenICam Live - Focus" -
