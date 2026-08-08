#pragma once
#include <cstddef>

// Shared delay-line buffer sizes — used by CLI, TUI, and Seed firmware.
// All must be powers of two (DelayLine uses bitmask addressing).
//
// Sizing rationale (@ 48 kHz):
//   kBufEarly  16384 = 341 ms  — covers predelay up to ~300 ms (size=1)
//   kBufDiff    8192 = 170 ms  — covers lt_sz diff taps at szL = lt_sz − 2.5
//   kBufLong   16384 = 341 ms  — covers lt_sz long delay + full lt_sym range
static constexpr size_t kBufEarly = 16384;
static constexpr size_t kBufDiff  =  8192;
static constexpr size_t kBufLong  = 16384;
