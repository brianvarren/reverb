#pragma once
#include <cstddef>

// Shared delay-line buffer sizes — used by CLI, TUI, and Seed firmware.
// All must be powers of two (DelayLine uses bitmask addressing).
//
// Sizing rationale (@ 48 kHz):
//   kBufEarly  16384 = 341 ms  — covers predelay up to ~47 ms (size=1) + pd_sym
//   kBufDiff   32768 = 682 ms  — lt_sz diffuser taps at szL+3 = -23.5 → ~23k samples
//   kBufLong   32768 = 682 ms  — lt_sz long delay at szL = -26.5 → ~27k samples
static constexpr size_t kBufEarly = 16384;
static constexpr size_t kBufDiff  = 32768;
static constexpr size_t kBufLong  = 32768;
static constexpr size_t kBufInj   = 1024;   // injection delay: covers 2×mod_ms up to 5 ms @ 48 kHz
