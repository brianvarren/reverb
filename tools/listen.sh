#!/usr/bin/env bash
# Rebuild the offline renderer, render an impulse response, and play it.
# All args are forwarded to `render` (e.g. --fb 0.8 --damp 0.3).
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --build build --target render >/dev/null

out="/tmp/reverb_ir.wav"
./build/render --out "$out" "$@"

if command -v aplay >/dev/null; then
    aplay -q "$out"
elif command -v paplay >/dev/null; then
    paplay "$out"
else
    ffplay -autoexit -nodisp -loglevel quiet "$out"
fi
