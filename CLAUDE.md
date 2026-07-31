# reverb

C++ reverb effect, targeting Linux (RtAudio) for development, Daisy Seed (libDaisy/DaisySP) eventually.

## Build

```bash
sudo apt install librtaudio-dev cmake build-essential
mkdir -p build && cd build && cmake .. && make
```

Binary: `build/reverb`

## Run

```bash
./build/reverb
# i  → fire impulse (hear reverb tail)
# b  → 100ms 440 Hz beep (hear tonal response)
# q  → quit
```

## Fast iteration (tuning DSP params)

`build/render` is an offline renderer with no RtAudio/audio-device dependency:
pushes an impulse through `Schroeder`, writes a WAV, exits. ~10ms to render a
few seconds of tail. All Schroeder params (fb, damp, allpass gain, delay
times) are CLI flags — most tuning tweaks need no recompile.

```bash
./tools/listen.sh --fb 0.8 --damp 0.3      # rebuild + render + play, one command
./build/render --help                       # see all flags and current defaults
```

Use this loop instead of `build/reverb` while tuning; reach for `build/reverb`
only to confirm the realtime path still works.

## Workflow preferences

- After every successful build: `git add -A && git commit -m "wip" && git push`
- Build command: `make -C build`
- Auto-build on every source file edit (hook configured in .claude/settings.json)

## Architecture

All DSP is in `src/` as headers (no dynamic allocation — Daisy Seed compatible):

| File | Purpose |
|------|---------|
| `src/dsp.h` | Primitives: `DelayLine<N>`, `CombFilter<N>`, `AllPassFilter<N>` |
| `src/schroeder.h` | Schroeder topology: 4 parallel combs → 2 series all-passes |
| `src/main.cpp` | RtAudio scaffold, `State` struct, audio callback, CLI |

## DSP notes

- Sample rate: 48000 Hz, buffer: 256 frames, stereo float32
- `State` struct is passed as `userData` to RtAudio callback (maps cleanly to Daisy Seed pattern)
- Atomics used for main→audio thread signaling (`fireImpulse`, `startBeep`); all other audio state is audio-thread-only
- No heap allocation in audio callback

## Roadmap

- [x] RtAudio scaffold
- [x] Impulse generator (`i`)
- [x] Beep generator (`b`, 440 Hz, 100ms, faded)
- [x] `DelayLine`, `CombFilter`, `AllPassFilter` primitives
- [x] Schroeder topology (4 combs + 2 all-passes)
- [ ] Tune Schroeder (delay times, feedback, damping)
- [ ] Second topology (FDN / Freeverb or Dattorro plate)
- [ ] Daisy Seed port
