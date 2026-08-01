# Build Guide — Ten Stages
## Companion to `daisy-reverb-plan.md`

This is the how. The plan document is the what. Read §0 before starting stage 1; the repo discipline it describes is what makes stages 1–9 possible without hardware.

---

## 0. Ground rules

### 0.1 The portability rule

**`core/` must never include a libDaisy or CMSIS header.** Not once, not behind an `#ifdef`. The moment it does, you lose the ability to build and test on the desktop, and you will spend the rest of the project reflashing to hear a coefficient change.

Everything platform-specific lives in `host/` and `firmware/`. The core takes `fs` as a constructor argument and buffers as pointers it does not own.

```
verb/
├── core/                     # zero platform dependencies
│   ├── util.h                # pitch_to_samples, Smoother, clamp
│   ├── delay.h               # DelayLine, Read4pt
│   ├── shelf.h               # OnePole, Shelf
│   ├── allpass.h             # damped Schroeder allpass
│   ├── rotation.h            # 2x2 rotation stage
│   ├── early.h               # early section
│   ├── late.h                # late section
│   ├── reverb.h              # top level: predelay, early, late, xfades
│   └── params.h              # the parameter struct, one source of truth
├── host/
│   ├── main.cpp              # CLI: in.wav + params.txt -> out.wav
│   ├── wav.h                 # dr_wav.h, single header
│   └── analysis.py           # T60, EDC, spectrogram, echo density
├── tests/
│   └── test_*.cpp            # doctest
├── firmware/
│   └── main.cpp              # libDaisy, SDRAM, knobs
└── ref/                      # reference renders for null tests
```

### 0.2 Memory ownership

The core allocates nothing. Every delay line is constructed over a caller-supplied buffer:

```cpp
class DelayLine {
public:
    void Init(float* buf, size_t size_pow2);   // size must be a power of two
    ...
private:
    float* buf_; size_t mask_; size_t w_;
};
```

Host build: buffers are `std::vector<float>`. Firmware build: they're `DSY_SDRAM_BSS` or plain static arrays. The core never knows or cares. This is the whole trick — it costs you one `Init()` call per delay line and buys you the entire desktop workflow.

### 0.3 One parameter struct

```cpp
struct Params {
    float predelay_ms, pd_sym;
    float er_sz, er_sym, er_dffs;
    float lt_sz, lt_sym, lt_theta;
    float rt60_s, bloom;
    float mod_ms, mod_hz;
    float dmp_hf, dmp_hb, dmp_lf, dmp_lb;
    float eo_hf, eo_hb, eo_lf, eo_lb;
    float el_mix, dw_mix;
};
```

The host CLI parses this from a text file. The firmware fills it from knobs and constants. Both feed the identical `Reverb::SetParams()`. Never let the two builds diverge on parameter interpretation — that's how you get a thing that sounds right on the desktop and wrong on the Seed with no way to find the difference.

### 0.4 Git discipline

Tag every stage: `stage-01`, `stage-02`, … Keep a reference render in `ref/` at each tag. Stage 9's null test (§9.9 of the plan) compares against these. When something breaks four stages later, `git bisect` plus a null test finds it in minutes.

---

## Stage 1 — Host harness

**Objective:** `./verb in.wav params.txt out.wav` renders bit-identical passthrough. No DSP yet.

**Build it:**

- CMake with three targets: `verb_core` (INTERFACE or static, header-only is fine), `verb_cli`, `verb_tests`.
- Use `dr_wav.h` for WAV I/O — single header, public domain, handles float and int16 in and out. Don't write your own WAV parser.
- Convert everything to `float` in `[-1, 1]` at load, deinterleave to two `std::vector<float>`, and work in that domain throughout. Interleave and convert on the way out.
- Write out **32-bit float** WAV, not 16-bit. You will be measuring −90 dB tails and doing null tests at −120 dB; 16-bit quantization floor will mask both.
- Render with a tail: append `ceil(rt60 * bloom * 1.5)` seconds of silence to the input before processing, so the decay is fully captured.

**Directives:**

- Add a `--impulse N` flag that ignores the input file and generates a single unit sample followed by N seconds of silence. You will use this constantly.
- Add `--noise SECONDS` for the soak test.
- Print peak, RMS, and a NaN/Inf count to stderr on every render. Free instrumentation, catches disasters immediately.

**Caveat:** make the params file re-read on every invocation, not cached. Then a shell loop like `while inotifywait params.txt; do ./verb in.wav params.txt out.wav && play out.wav; done` gives you a live tweaking loop. Set this up now, not at stage 10 when you actually need it.

**Exit:** passthrough render is sample-identical to input (verify with a null test against the source). Test binary runs and reports zero tests.

---

## Stage 2 — Pitch conversion, smoother, delay line

**Objective:** the three primitives everything else is built on.

### `pitch_to_samples`

```cpp
inline float pitch_to_samples(float p, float fs) {
    return (fs / 440.0f) * exp2f((69.0f - p) * (1.0f / 12.0f));
}
```

Call this **per control block, never per sample**. Seven calls per 48-sample block is free; 48× that is not.

### Smoother

```cpp
struct Smoother {
    float y = 0.f, a = 1.f;
    void SetTC(float tc_s, float block_dt) { a = 1.f - expf(-block_dt / tc_s); }
    float Process(float target) { y += a * (target - y); return y; }
    void Reset(float v) { y = v; }
};
```

**`Reset()` matters.** On `Init()`, snap every smoother to its target rather than ramping from zero — otherwise the first 50 ms after boot has all delay lines sweeping from 0 samples up to their target, which sounds like a dying UFO and can momentarily push read indices out of range.

### Delay line

Power-of-two buffer, masked index, write pointer:

```cpp
inline void Write(float x) { buf_[w_] = x; w_ = (w_ + 1) & mask_; }

inline float Read4pt(float d) const {
    d = clampf(d, 2.0f, float(mask_ - 2));
    int   i = int(d);
    float x = d - float(i);
    size_t base = (w_ - 1 - i) & mask_;          // delay of i samples
    float ym1 = buf_[(base + 1) & mask_];        // delay i-1 (more recent)
    float y0  = buf_[base];
    float y1  = buf_[(base - 1) & mask_];
    float y2  = buf_[(base - 2) & mask_];
    float c0 = y0;
    float c1 = y1 - (1.f/3.f)*ym1 - 0.5f*y0 - (1.f/6.f)*y2;
    float c2 = 0.5f*(ym1 + y1) - y0;
    float c3 = (1.f/6.f)*(y2 - ym1) + 0.5f*(y0 - y1);
    return ((c3*x + c2)*x + c1)*x + c0;
}
```

**Considerations:**

- **Read-then-write, always.** In a feedback loop, writing first then reading at delay `d` gives you `d−1` samples of actual delay and, at `d` near zero, a delay-free path. Fix the convention now and assert it in the test.
- The `clampf` to `[2, mask−2]` is what keeps 4-point interpolation from reading past either end. Do not remove it as an optimization.
- Sign conventions on the index arithmetic are the single most common source of "why does it sound like a comb filter" bugs. Test them before moving on.

**Exit criteria (plan §9.1, §9.2):**

- `pitch_to_samples(69, 48000) == 48000/440` within 1e−4.
- Octave property: `f(p) == 2·f(p+12)` for p in 12…60.
- Write a ramp `0,1,2,3,…`; `Read4pt(k)` for integer k returns exactly the right sample.
- `Read4pt(k + 0.5)` matches the analytic Lagrange cubic through the four neighbours.
- **Amplitude flatness sweep:** feed a 1 kHz sine, sweep the read delay slowly from 100.0 to 101.0 samples over a second, assert output amplitude stays within 0.5%. This is the test that catches accidental linear interpolation, which would droop to 0.637 at half-sample and put a tremolo in your tail.

---

## Stage 3 — The shelf

**Objective:** a two-band shelf with a provable magnitude bound.

**Do not** reach for the RBJ cookbook biquads here. Use this formulation instead:

```cpp
struct OnePole {
    float z = 0.f, a = 0.f;
    void SetCutoff(float fc, float fs) { a = 1.f - expf(-2.f*float(M_PI)*fc/fs); }
    float LP(float x) { z += a * (x - z); return z; }
};

// A = linear gain (10^(dB/20))
// High shelf: unity at DC, gain A at Nyquist
float HighShelf(float x) { return A * x + (1.f - A) * lp.LP(x); }
// Low shelf: gain A at DC, unity at Nyquist
float LowShelf(float x)  { float l = lp.LP(x); return A * l + (1.f - A) * (x - l); }
```

**Why this form, specifically.** Both are of the shape `H = A + (1−A)·F` where `F` is a one-pole lowpass or its complement, and both `|LP| ≤ 1` and `|1−LP| ≤ 1` for a one-pole. So by the triangle inequality, for `A ≤ 1`:

```
|H(ω)| ≤ A + (1−A)·|F(ω)| ≤ A + (1−A) = 1
```

**Guaranteed ≤ 0 dB at every frequency, for every cutoff, with a one-line proof.** That is exactly the stability precondition the late loop needs (plan §6.1), and here it's enforced structurally by `A ≤ 1` rather than by testing a coefficient table. For the *output* shelf, where boost is allowed, `A > 1` and the bound doesn't hold — which is fine, because the output shelf isn't in a loop.

The cost is that first-order shelves have a gentle 6 dB/oct transition. For this application that's a feature: you want air absorption, not a tone control.

**Directives:**

- One `Shelf` object = one low shelf + one high shelf in series, four parameters.
- Recompute coefficients per control block only.
- **Assert `A ≤ 1.0` in the constructor for any shelf instance destined for the loop.** Make it a compile-time or debug-time trap, not a comment.

**Exit (plan §9.3):** sweep sine at 20 log-spaced frequencies from 20 Hz to 20 kHz, measure steady-state gain, compare to the analytic `|A + (1−A)F(ω)|`. Within 0.1 dB. Separately, for 100 random in-loop parameter sets, assert `max|H(ω)| ≤ 1.0` across a 512-point frequency grid.

---

## Stage 4 — Damped allpass diffuser

**Objective:** the early section's building block.

```cpp
float Process(float x) {
    float v = shelf_.Process(dl_.Read4pt(d_));   // or Read() — see below
    float w = x + g_ * v;
    dl_.Write(w);
    return v - g_ * w;
}
```

**Considerations:**

- The shelf sits **inside** the allpass loop, on the delay output, before both the feedback sum and the output. This matches SMD's `DiffDel`, which takes all four shelf parameters as inputs. The alternative (shelf only in the feedback path) is also defensible; if you try it, do so as an A/B at stage 5, not now.
- With the shelf active this is no longer strictly allpass — magnitude drops where the shelf cuts. That's intended. **Test flatness with the shelf bypassed** (`A = 1` both bands), where it must be exactly allpass.
- Early diffusers are **unmodulated**, so `Read4pt` is overkill — use integer `Read()`. But keep the delay time smoothed, because Size still moves. The 50 ms smoother plus an integer read means Size sweeps produce a soft stepping rather than a pitch bend in the early section. That's acceptable and much cheaper. If it bothers you on fast Size sweeps, upgrade early to `Read4pt` — you have the CPU.
- `g` range 0.6–0.75, hard-clamped to `|g| < 0.95`.

**Exit (plan §9.4):** with shelves bypassed, `|H(ω)| == 1` within 0.01 dB across 20 Hz–20 kHz. Impulse response shows the expected decaying tap train at spacing `M`. With shelves active, magnitude is monotonically ≤ 1.

---

## Stage 5 — Early section

**Objective:** two independent per-channel diffuser chains with size asymmetry. First stage you can meaningfully listen to.

```
SzL = ErSz − ErSym                    SzR = ErSz + ErSym
L: AP(SzL+8) → AP(SzL+4) → AP(SzL) → Shelf
R: AP(SzR+8) → AP(SzR+4) → AP(SzR) → Shelf
```

**Directives:**

- **No cross-feed. None.** Not a little bit for "width." The width comes from `ErSym`. If you add cross-feed here you will smear the stereo image and then spend a week wondering why the reverb sounds mono-ish and vague.
- Buffer sizes: at the low end of the useful `ErSz` range (say pitch 18 → 2076 samples for the base stage), the longest early line is `N(18) = 2076`. Round to 4096 per line, six lines, 98 KB. Sized once, statically.
- Add the predelay in front now (§0.3 `predelay_ms`, `pd_sym`) — it's just two more delay lines and it's easier to test the whole front end at once.

**What you should hear:** a short, dense, bright burst. No tail. It should sound like a slapback with the slap smeared out, and it should be obviously wide when `ErSym > 0` and obviously narrow when `ErSym = 0`.

**Caveat:** it will sound thin and unimpressive. That's correct. The early section is 15% of the final sound. Resist the urge to tune it now.

**Exit:** impulse response shows a dense, non-periodic tap pattern that decays to nothing within ~150 ms. L/R correlation coefficient drops measurably as `ErSym` rises from 0 to 2. Autocorrelation of the ER burst shows no peak above 0.3 outside lag 0.

---

## Stage 6 — Rotation stage

**Objective:** the late section's diffuser. Test it in isolation before it goes anywhere near a feedback loop.

```cpp
struct Rotation {
    float c = 0.70710678f, s = 0.70710678f;
    void SetAngle(float th) { c = cosf(th); s = sinf(th); }
    void Process(float& a, float& b) {
        float a2 = c*a - s*b;
        float b2 = s*a + c*b;
        a = a2; b = b2;
    }
};
```

The stage as used = rotate, then read both rails' delay lines, then write the rotated values:

```cpp
Process(a, b);
float ra = dlA.Read4pt(dA + modA);  dlA.Write(a);
float rb = dlB.Read4pt(dB + modB);  dlB.Write(b);
a = shelfA.Process(ra);
b = shelfB.Process(rb);
```

**Considerations:**

- `θ = π/4` is the default and probably the final value. Expose it only if you want a "diffusion" knob; `θ = 0` degenerates to two independent delays, which is a useful diagnostic mode — put it behind a debug flag.
- Recompute `c`/`s` per block, never per sample.
- The two rails **are** the L and R sides of the late network. Their delay lengths differ by `LtSym`. Don't add a second asymmetry mechanism on top.

**Exit (plan §9.6):** for `θ ∈ {0, π/6, π/4, π/3, π/2}` and 1000 random 2-vectors, `‖Rx‖² == ‖x‖²` within 1e−6, and `RᵀR == I` within 1e−6. Trivial test, ~10 lines, catches an entire class of catastrophic bugs.

---

## Stage 7 — Late loop, no modulation, low feedback

**Objective:** the recirculating network, running conservatively. The first stage where a mistake can blow up.

**Set `fb_gain = 0.5` by hand and leave it there for the whole stage.** Do not wire up the RT60 formula yet. You want a loop that is obviously, boringly stable while you get the structure right.

**The cross-coupling and the `z⁻¹`:**

```cpp
float aL = eOutL + fbR_;     // fbR_ and fbL_ hold LAST sample's values
float aR = eOutR + fbL_;
... three rotation stages, output shelf ...
float outL = longL.Read4pt(dL3); longL.Write(aL);
float outR = longR.Read4pt(dR3); longR.Write(aR);
fbL_ = outL * fb_gain;       // stored for next sample
fbR_ = outR * fb_gain;
```

The cross-coupling `L' = eL + FBR` creates a delay-free loop unless the feedback is read from the previous sample. SMD draws the `z⁻¹` explicitly. **This is not optional and it is not a detail** — omit it and you have an algebraic loop that either won't compile as written or will silently read a stale value in an order-dependent way.

**Directives:**

- Order within the loop: sum → rotate/delay/shelf ×3 → chain shelf → long delay → DC block → gain → store. Freeze this order; it's what the plan's stability argument assumes.
- Put the DC blocker in now, even at `fb_gain = 0.5` where it doesn't matter. You will not remember at stage 8.
- Buffer sizes: longest late line at `LtSz = 12` is `N(12+9) = 1745` for the diffusers and `N(12) = 2936` for the long delay. Round diffusers to 4096 and the long delay to 8192. Six diffuser lines + two long = 197 KB + 66 KB.
- Now wire the RT60 formula, but sum **all four** delay lengths (three diffusers + long delay) as the loop length `D`, per plan §6.3. Add the `Bloom` multiplier separately.

**What you should hear:** a real reverb tail, short and a bit dry at `fb_gain = 0.5`, but recognizably a reverb, and noticeably smoother than the early section alone.

**Exit (plan §9.5):** measured T60 via Schroeder backward integration matches `−3D/log₁₀(fb_gain)` within 5%, at three different `LtSz` values. Then re-derive `fb_gain` from a target T60 and confirm round-trip agreement. **This is the test that tells you whether the §6.3 undercount analysis is right** — measure it, don't assume it.

---

## Stage 8 — High feedback and the soak

**Objective:** prove the loop is stable at the settings you actually want to use.

Raise `fb_gain` toward the 0.995 ceiling. Set `T60 = 60 s`. Run.

**The soak test (plan §9.7), in full:**

1. 10 minutes of full-scale white noise, then 10 minutes of silence.
2. `fb_gain = 0.995`, all shelves flat (`A = 1.0`), `LtSym` and `LtSz` sweeping continuously across their full ranges.
3. Assert: no NaN or Inf, ever. `|out| < 4.0` at every sample. Running mean over the final 10 seconds `< 1e−4`. RMS during the silence tail decays monotonically over 10-second windows.

**Run it with shelves flat.** Flat shelves are the worst case — any damping is free stability margin, and you want to know the loop is stable without it.

**Common failure modes at this stage:**

| Symptom | Cause |
|---|---|
| Slow ramp to clipping over seconds | An in-loop shelf with `A > 1`. Check the constructor assertion from stage 3. |
| Output creeps to a large constant | Missing or misplaced DC blocker. It must be inside the loop, before the gain. |
| Sudden NaN after minutes of silence | Denormals. Harmless on the host; will wreck you on the M7. Structure the fix now (§10). |
| Ringing at one pitch | A rotation stage isn't rotating — check `θ`, check that both rails' outputs are actually being swapped back in. |
| Clicks on Size sweep | Smoother not applied, or applied after the mod offset instead of before. |

**Directive:** if the soak fails, **do not add damping to make it pass.** Find the actual cause. A loop that needs damping to be stable will bite you the moment a user turns Dark down.

**Exit:** soak passes clean, three times, with different RNG seeds.

---

## Stage 9 — Modulation

**Objective:** the last DSP stage. Adds the motion that makes it sound alive.

```cpp
phase_ += mod_hz * block_dt;
if (phase_ >= 1.f) phase_ -= 1.f;
for (int k = 0; k < 8; ++k)
    mod_[k] = depth_samples * sinf(2.f*float(M_PI)*(phase_ + k*0.125f));
```

Eight phases: `mod_[0..3]` for L's three diffusers and long delay, `mod_[4..7]` for R's.

**Considerations:**

- Compute per control block. At 1 Hz and 48-sample blocks the stepping is inaudible; interpolate across the block only if you can hear it (you won't).
- Depth is in **samples**, converted from the `mod_ms` parameter: `depth_samples = mod_ms * 0.001f * fs`.
- Add the mod offset **after** smoothing, never before. Smoothing the modulated value would low-pass your LFO into nothing.
- Re-clamp the final read index. Stage 2's `clampf` inside `Read4pt` handles it, but confirm the clamp isn't being hit in normal operation — if it is, your buffer is too small.

**Exit:** the tail should thicken and gain a slow shimmer. Then re-run the stage 8 soak with modulation active — it must still pass. Then the specific artifact check: **listen for tremolo at the mod rate.** If the tail pulses in amplitude, linear interpolation has crept in somewhere. Go back to §9.2.

**Directive:** tune modulation last, and tune it low. `mod_ms = 0.6`, `mod_hz = 0.7` as a starting point. Modulation masks structural problems — if you tune it before stage 8 passes clean, you'll paper over a ringing diffuser and never find it.

---

## Stage 10 — Port to hardware

**Objective:** the same core, on the Seed, with knobs.

By now `core/` is done and tested. This stage should be a day, not a week, if §0.1 was respected.

### 10.1 Memory placement

```cpp
// SDRAM: predelay only. Sequential, unmodulated, integer reads.
static float DSY_SDRAM_BSS predelay_buf[2][131072];   // 2^17 = 2.73 s

// Internal SRAM: everything interpolated.
static float early_buf[6][4096];
static float late_buf[6][8192];
static float long_buf[2][8192];
```

**Zero the SDRAM buffer explicitly in `Init()`.** SDRAM contents are undefined at boot and C++ constructors run before the SDRAM controller is up. Skipping this gives you a lovely burst of garbage on the first pass through the predelay, and in a feedback path that garbage never fully leaves.

Do not put anything with a nontrivial constructor in SDRAM.

### 10.2 Denormals

Set flush-to-zero in `FPSCR` at the top of `main()`, before `StartAudio`:

```cpp
__set_FPSCR(__get_FPSCR() | (1u << 24));   // FZ bit
```

On Cortex-M7 subnormal operands cost tens of cycles each. A reverb tail decaying toward silence generates them across every filter state simultaneously — meaning your CPU spike arrives precisely when the reverb is idle and you're least likely to be watching. Belt-and-braces: add `1e-20f` to each feedback sample.

### 10.3 Audio config

```cpp
hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
hw.SetAudioBlockSize(48);
```

Non-interleaved stereo callback. Do all block-rate work (pitch conversion, smoothing, shelf coefficients, LFO phases, `fb_gain`) at the top of the callback, then run a tight per-sample loop.

### 10.4 Knobs

Eight is the realistic budget: `LtSz` (Size), `RT60`, `DmpHB` (Dark), `ModA` (Motion), `LtSym` (Width), `E/L`, `PreDel`, `D/W`. Everything else becomes a `constexpr` from your tuned parameter file.

- Read ADCs once per block, not per sample.
- Smooth every knob with `fonepole()` at ~20 ms **before** it enters the parameter struct. Daisy's ADC is noisy enough to dither the bottom bits, and a jittering `LtSz` means a jittering delay time means a constant faint warble.
- Map `LtSz` and `RT60` exponentially. `LtSz` is already a pitch, so linear-in-pitch is correct. `RT60` should be exponential in seconds (0.2 → 30 s).

### 10.5 Profiling

```cpp
hw.SetLed(true);  /* audio callback body */  hw.SetLed(false);
```

Scope the LED pin, or use `System::GetUs()` around the callback and report the max over a second via serial. Target under 50% at the worst-case setting, which is `LtSz` at its minimum (longest delays, most SDRAM pressure) with modulation at full depth.

**If you're over budget,** in order: (1) drop early diffusers to integer reads if you upgraded them, (2) move the output shelf to block rate, (3) drop the late diffusers from three stages to two. Do not drop to linear interpolation — that's the one thing you'll hear.

### 10.6 The final check

Render the same test file through the host build and through the Seed (record its output). Null-test them. They won't be bit-identical — different `expf` implementations, different rounding — but the difference should be below −80 dBFS. If it's above −40, something structural diverged, and the parameter struct (§0.3) is the first place to look.

---

## Appendix: pacing

Stages 1–4 are a weekend if you're focused — they're small, well-defined, and heavily testable. Stage 5 gives you the first listenable output. Stage 7 is where it becomes a reverb. Stage 8 is the one that takes longest and feels least productive, and is the one most worth not rushing; every hour spent there is an hour not spent debugging an intermittent instability on hardware with no visibility.

Stages 1–9 are all desktop. Do not flash the Seed until stage 10. The temptation to "just try it on hardware" after stage 7 is strong and should be resisted — you'll lose the test suite, the null tests, and the fast iteration loop, and gain nothing except the knowledge that it makes sound, which you already have from the WAV renders.
