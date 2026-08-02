# verb

Dark cavernous stereo reverb — Space Master Deluxe skeleton with Greyhole rotation diffuser.

**Target:** Daisy Seed (STM32H750, 480 MHz Cortex-M7), 48 kHz stereo.
**Dev platform:** Linux desktop, WAV-in/WAV-out CLI (no audio device needed until stage 10).

Design documents: `daisy-reverb-plan.md` (what), `daisy-reverb-build-guide.md` (how).

---

## Directory structure

```
core/       zero platform dependencies — the DSP; never include libDaisy here
host/       CLI harness: in.wav + params.txt → out.wav
tests/      doctest suite (verb_tests)
firmware/   Daisy Seed port (stage 10)
ref/        reference renders for null tests (tagged per stage)
```

## Build

```bash
mkdir -p build && cd build && cmake .. && make
```

Produces `build/verb_cli` and `build/verb_tests`.

## Run

```bash
# Impulse response (most common during DSP development):
./build/verb_cli --impulse 5 params.txt out.wav

# Normal file:
./build/verb_cli in.wav params.txt out.wav

# White-noise soak:
./build/verb_cli --noise 60 params.txt out.wav

# Test suite:
./build/verb_tests
```

Stats (peak, RMS, nan/inf count) are printed to stderr on every render.

## Fast iteration loop

Edit `params.txt`, re-run. The params file is re-read on every invocation — no recompile needed for parameter changes. Suggested shell loop:

```bash
while inotifywait -e close_write params.txt; do
    ./build/verb_cli --impulse 5 params.txt /tmp/out.wav && play /tmp/out.wav
done
```

## Workflow preferences

- After every successful build: `git add -A && git commit -m "wip" && git push`
- Build command: `make -C build`
- Auto-build on every source file edit (hook configured in .claude/settings.json)

---

## Architecture

### Core (portable, zero allocations)

| File | Stage | Purpose |
|------|-------|---------|
| `core/params.h` | 1 | Parameter struct — single source of truth for both host and firmware |
| `core/util.h` | 2 | `pitch_to_samples`, `Smoother`, `clamp` |
| `core/delay.h` | 2 | `DelayLine` (Init over caller buffer), `Read4pt` (4-pt Lagrange) |
| `core/shelf.h` | 3 | `OnePole`, `Shelf` (low+high, provably ≤ 0 dB in-loop) |
| `core/allpass.h` | 4 | Damped Schroeder allpass (early diffuser) |
| `core/rotation.h` | 6 | 2×2 rotation stage (late diffuser, energy-preserving) |
| `core/early.h` | 5 | Early section: 3 allpasses ×2 channels, predelay, SzL/SzR |
| `core/late.h` | 7 | Late loop: 3 rotation stages + long delay, RT60 → fb_gain |
| `core/reverb.h` | 7 | Top level: predelay → early → late → xfades |

### Host

| File | Purpose |
|------|---------|
| `host/main.cpp` | CLI: arg parsing, WAV I/O, stats, passthrough → DSP |
| `host/dr_wav.h` | Single-header WAV library (dr_libs, public domain) |
| `host/analysis.py` | T60, EDC, spectrogram, echo density (tuning aid) |

### Tests

`tests/test_main.cpp` + `tests/doctest.h`. Tests are added stage by stage (see build guide §9).

---

## DSP design summary

```
In L/R → PreDelay (L/R independent) → Early (3 allpasses/ch, no feedback, no cross-feed)
       ↘                                                          ↘
         ──────────────────── Late (cross-coupled, modulated) ──────
                                                                   ↘
                             Xfade(E/L) → Output Shelf → Xfade(D/W) → Out
```

- **Early:** Schroeder allpass with embedded shelf, no rotation, no cross-feed
- **Late:** rotation-matrix diffuser (energy-preserving, no metallic ring under recirculation)
- **Sizing:** pitch-domain (MIDI pitch → samples via `pitch_to_samples`), scale-invariant irrational ratios
- **Stereo:** `SzL = Size − Sym`, `SzR = Size + Sym` at predelay, early, and late independently
- **Stability:** rotations are lossless → loop gain = `fb_gain` × shelf gains (both ≤ 0 dB in-loop)
- **Denormals:** FZ bit + `1e−20` belt-and-braces (firmware only)

## Key constants

- Sample rate: 48000 Hz, block size: 48 frames
- Late default `LtSz=24`: loop = 96.09 ms (4 delays at pitches 24/27/30/33)
- `fb_gain = 10^(−3D/T60)`, clamped to 0.995; `Bloom` default 3.14 (SMD character)
- In-loop shelf gains **must be ≤ 0 dB** — enforced by assertion

## Build stages

| # | Stage | Status |
|---|-------|--------|
| 1 | Host harness: passthrough WAV render, params.txt, --impulse/--noise | ✅ done |
| 2 | `pitch_to_samples`, Smoother, DelayLine + 4-pt interpolation | ✅ done |
| 3 | Two-band shelf (provably ≤ 0 dB) | ✅ done |
| 4 | Damped allpass diffuser | ✅ done |
| 5 | Early section (2-channel, SzL/SzR, predelay) | ✅ done |
| 6 | Rotation stage (unit test energy preservation) | ✅ done |
| 7 | Late loop, no modulation, low fb_gain | ✅ done |
| 8 | High fb_gain + 10-minute soak | ✅ done |
| 9 | Multi-phase LFO modulation | ✅ done |
| 10 | Daisy Seed port: SDRAM, FZ bit, knobs | ⬜ |

Do not flash the Seed until stage 10.
