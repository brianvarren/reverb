# A Dark Cavernous Stereo Reverb for Daisy Seed
## Revision 3 — Space Master Deluxe skeleton, Greyhole diffusion primitive

**Status:** design locked, ready to build
**Target:** Daisy Seed (STM32H750, 480 MHz Cortex-M7, hard FPU), 48 kHz, stereo in/out
**Character goal:** dark, cavernous, immense. Not a room simulator. Not pristine.

---

## 0. What changed and why

Revision 1 proposed a Dattorro figure-eight plate. Revision 2 moved to a Greyhole-style single diffused feedback loop. Both were wrong in the same way: they used **one** recirculating network to do all the work.

The Space Master Deluxe structure view settles it. SMD splits the problem in two:

- an **early** section — short, unmodulated, **no feedback at all**, two fully independent channels
- a **late** section — modulated, cross-coupled, recirculating

…and crossfades between them (`E/L`). The early section builds the initial density and the stereo image; the late section builds the tail. Because early has no feedback, it can use cheap unmodulated diffusers without ringing. Because late is the only recirculating part, that's the only place that needs 4-point interpolation, modulation, and careful stability work.

That separation is the single most useful thing in the whole ensemble. It is what this revision is built around.

**Final architecture:** SMD's skeleton and parameter scheme, with Greyhole's rotation-matrix diffuser substituted into the late loop only.

| Element | Source | Rationale |
|---|---|---|
| Early/late split with crossfade | SMD | Decouples density-building from tail-building |
| Pitch-domain delay sizing (P2T) | SMD | Irrational, scale-invariant length ratios |
| `SzL = Size − Sym`, `SzR = Size + Sym` | SMD | Stereo width from one knob, no cross-feed |
| Per-stage embedded low+high shelf | SMD | Distributed gentle damping ("creamy") |
| Cross-coupled feedback, late only | SMD | Width in the tail without smearing early image |
| N-phase LFO, one phase per delay | SMD | Decorrelated modulation |
| 4-point interpolation on modulated lines only | SMD | Don't pay for interpolation you don't need |
| `Smooth(0.05)` on every delay time | SMD | Survives Size sweeps without zipper |
| RT60 → feedback gain | SMD | Standard, but see §6.3 |
| **Rotation-matrix diffuser (late)** | **Greyhole** | Energy-preserving, no metallic ring under recirculation |
| Serial allpass diffuser (early) | SMD | Correct choice where there's no feedback — see §4.2 |

Dropped from Rev 2: the Dattorro tank, the JPverb 3-band filterbank (SMD's two-shelf pair does the same job for a third of the cost), prime-number delay tables, and Greyhole's single-loop topology.

---

## 1. Signal flow

```
                     ┌──────────────────────────────────────┐
  In L ──┬──────────►│ PreDel   T = f(PreDel, PDSym)         │──► eL ──┐
  In R ──┤           │ (independent L/R, no feedback)        │──► eR ──┤
         │           └──────────────────────────────────────┘         │
         │                                                            │
         │   ┌────────────────────── EARLY ──────────────────────┐    │
         └──►│  per channel, SzL / SzR, NO cross-feed:           │◄───┘
             │    AP(Sz+8) → AP(Sz+4) → AP(Sz) → Shelf          │
             └───────────────────────────────────────────────────┘
                              │ eOutL, eOutR
                              ▼
             ┌────────────────────── LATE ───────────────────────┐
             │  L' = eOutL + FBR      R' = eOutR + FBL           │
             │                                                   │
             │  ROT(Sz+9,M1) → ROT(Sz+6,M2) → ROT(Sz+3,M3)       │
             │       → Shelf → Delay(Sz+M4) → ×g(RT60) → FB      │
             │                                                   │
             │  8-phase LFO → M1..M4 (L), M5..M8 (R)             │
             └───────────────────────────────────────────────────┘
                              │ lOutL, lOutR
                              ▼
             Xfade(E/L)  →  Output Shelf (EOHF/EOHB/EOLF/EOLB)
                              ▼
                        Xfade(D/W)  →  Out L/R
```

Predelay, early, and late each receive their own size and their own symmetry offset. That's three independent opportunities for L/R decorrelation before a single sample of cross-feed occurs.

---

## 2. Pitch-domain sizing — the core trick

Every delay time is stored as a **MIDI pitch number**, converted to a period at use time:

```
f(p) = 440 · 2^((p − 69)/12)          [Hz]
T(p) = 1 / f(p)                        [seconds]
N(p) = fs / f(p)                       [samples]
```

Stage offsets are fixed musical intervals from the section's base size:

- **Early:** `Sz+8`, `Sz+4`, `Sz` → ratios 2^(8/12) : 2^(4/12) : 1 = **1.587 : 1.260 : 1**
- **Late:** `Sz+9`, `Sz+6`, `Sz+3`, `Sz` → ratios 2^(9/12) : 2^(6/12) : 2^(3/12) : 1 = **1.682 : 1.414 : 1.189 : 1**

**Why this beats prime sample counts.** Every one of those ratios is irrational (2^(k/12) is irrational for k not a multiple of 12). Two delay lines whose lengths are in irrational ratio can never share a common period — their echo trains never realign, at any Size. Prime-number tables lose this the moment you scale them: multiply 1447 and 1861 by 1.6 and round, and you get 2315 and 2978, which share a factor of 2. SMD's scheme is **scale-invariant by construction**. Size becomes one exponential knob and the network stays correctly proportioned across its whole range.

Secondary benefit: pitch is perceptually linear in the thing you care about. A Size knob in semitones sweeps room scale evenly; a Size knob in milliseconds is bunched at the small end.

### 2.1 Lookup table (48 kHz)

| Pitch | Freq (Hz) | Samples | ms |
|---|---|---|---|
| 12 | 16.352 | 2935.5 | 61.16 |
| 15 | 19.445 | 2468.4 | 51.43 |
| 18 | 23.125 | 2075.7 | 43.24 |
| 21 | 27.500 | 1745.5 | 36.36 |
| 24 | 32.703 | 1467.7 | 30.58 |
| 27 | 38.891 | 1234.2 | 25.71 |
| 30 | 46.249 | 1037.9 | 21.62 |
| 33 | 55.000 | 872.7 | 18.18 |
| 36 | 65.406 | 733.9 | 15.29 |
| 39 | 77.782 | 617.1 | 12.86 |
| 42 | 92.499 | 518.9 | 10.81 |
| 45 | 110.000 | 436.4 | 9.09 |
| 48 | 130.813 | 366.9 | 7.65 |
| 51 | 155.563 | 308.6 | 6.43 |

**Default late section, `LtSz = 24`:** 872.7 / 1037.9 / 1234.2 / 1467.7 samples → 18.18 / 21.62 / 25.71 / 30.58 ms. **Loop total 4612.5 samples = 96.09 ms.**

**Default early section, `ErSz = 30`:** 653.8 / 823.7 / 1037.9 samples → 13.62 / 17.16 / 21.62 ms. **Total 2515.4 samples = 52.4 ms.**

### 2.2 Implementation

Don't call `powf` per sample. Per control block (every 48 samples is plenty):

```c
static inline float pitch_to_samples(float p, float fs) {
    return fs * 0.0022727273f * exp2f((69.0f - p) * (1.0f/12.0f));  // fs/440 · 2^((69−p)/12)
}
```

`exp2f` on the M7 is ~20–40 cycles. Seven of them per block is nothing. If you want it free, tabulate 128 semitones and interpolate — but measure first, you almost certainly won't need to.

---

## 3. Stereo: the Sym trick

```
SzL = Size − Sym
SzR = Size + Sym
```

Applied independently at predelay (`PDSym`), early (`ERSym`), and late (`LtSym`).

Because sizes are pitches, `±Sym` is a **detune in semitones**, so it scales both channels' networks by reciprocal factors 2^(∓Sym/12). At `Sym = 1.0`, left runs 5.9% long and right 5.9% short — the two channels' echo patterns diverge immediately and never re-converge.

This is a better decorrelation mechanism than what Greyhole uses (sine vs. cosine LFO phase on the loop modulators), because it's static, costs nothing at runtime, and doesn't rely on modulation being switched on. Set `ModA = 0` and SMD is still wide. Set Greyhole's `modDepth = 0` and it collapses toward mono.

Keep `Sym` small — 0.5 to 2.0 semitones. Above ~4 the channels start to sound like two different reverbs rather than one wide one.

---

## 4. Core blocks

### 4.1 Smoothed delay time

Every delay time passes through a one-pole smoother with a **50 ms** time constant before it reaches the delay line:

```c
// per control block, dt_block = block_size / fs
float a = 1.0f - expf(-dt_block / 0.05f);
smoothed += a * (target - smoothed);
```

This is not optional. Size is a per-sample-relevant quantity feeding an interpolating delay; step-changing it produces a loud click and, in the late loop, a transient burst of gain. 50 ms also means a fast Size sweep produces a pleasant pitch-bend rather than a stutter — which is a usable performance gesture.

### 4.2 Early diffuser — Schroeder allpass with embedded shelf

SMD's `DiffDel` takes `In`, `T`, `Dffs`, and all four shelf parameters. The shelf lives **inside** the allpass loop, not after it.

```
w[n] = x[n] + g · shelf(w[n−M])
y[n] = shelf(w[n−M]) − g · w[n]
```

with `g = Dffs`, `|g| < 1`, and `shelf()` the two-band filter of §4.4.

**Why keep a plain allpass here rather than a rotation diffuser.** A rotation diffuser is inherently a 2-rail structure — it mixes two signals. Using one in the early section would cross-couple L and R, which is exactly what SMD deliberately avoids there. And the ringing problem that motivates rotation diffusers is a *recirculation* problem: a cascade of short allpasses sounds metallic when its output is fed back into itself many times. The early section has no feedback at all. Three allpasses in series, heard once, do not ring.

So: allpass in early, rotation in late. This is a real design decision, not a compromise — it's cheaper *and* structurally correct.

Coefficient: `Dffs = 0.6` to `0.75`. Below 0.5 the early section sounds like three discrete taps; above 0.8 it starts to buzz.

### 4.3 Late diffuser — 2×2 rotation with 4-point interpolation

The rotation matrix:

```
R(θ) = ⎡ cos θ   −sin θ ⎤
       ⎣ sin θ    cos θ ⎦
```

**Orthogonality:** `RᵀR = I`, since `cos²θ + sin²θ = 1`. **Energy preservation:** for any input vector **x**, `‖R x‖² = xᵀRᵀR x = xᵀx = ‖x‖²`. The transform redistributes energy between the two rails without creating or destroying any. `det R = 1`, `R⁻¹ = Rᵀ`.

One stage, operating on rails `(a, b)` with delay lengths `Ma`, `Mb`:

```
      ┌─ ×cos θ ──┬─ z^−Ma ──► a'
a ────┤           │
      └─ ×sin θ ──┼───────────┐
                  │           │
b ────┬─ ×(−sin θ)┘           ├─ z^−Mb ──► b'
      └─ ×cos θ ──────────────┘
```

In code:

```c
float a2 = c*a - s*b;
float b2 = s*a + c*b;
a = delayA.Read(); delayA.Write(a2);
b = delayB.Read(); delayB.Write(b2);
```

The two rails are the L and R sides of the late network, so `Ma = N(SzL + k)` and `Mb = N(SzR + k)` — the Sym asymmetry is already baked in.

**Why this survives recirculation where allpasses don't.** A one-multiplier allpass governs both its feedforward and feedback path with a single coefficient `g`. Frequencies near the delay's resonances dwell in the line and exit late; run that through a feedback loop 50 times and those resonances become an audible metallic pitch. A rotation instead mixes two lines with a coefficient *pair* constrained to the unit circle. There is no single resonant coefficient to accumulate. Energy scatters between rails rather than dwelling in one.

**Cascade losslessness.** A product of rotations is a rotation (orthogonal × orthogonal = orthogonal). Pure delays are unit-magnitude on the unit circle. Therefore a cascade of rotations with interleaved delays has operator norm exactly 1 — it is lossless. This matters enormously for §6: it means **the diffuser contributes no gain to the feedback loop**, so loop stability is governed entirely by the explicit feedback multiplier and the shelves. You can reason about stability with a single number.

Angle: `θ = π/4` gives equal mixing (cos = sin = 0.7071) and the fastest density build-up. Map the `Dffs` control to `θ ∈ [0, π/4]` if you want it variable; `θ = 0` is bypass (two independent delays), `π/4` is full scatter. Fixing `θ = π/4` and exposing nothing is a defensible simplification.

**Interpolation.** These are the modulated lines, so they need fractional reads. Use 4-point Lagrange (SMD's `DiffDel(4pt)`):

```c
// x = fractional part, y[-1..2] the four samples around the read point
float c0 = y0;
float c1 = y1 - (1.0f/3.0f)*ym1 - 0.5f*y0 - (1.0f/6.0f)*y2;
float c2 = 0.5f*(ym1 + y1) - y0;
float c3 = (1.0f/6.0f)*(y2 - ym1) + 0.5f*(y0 - y1);
return ((c3*x + c2)*x + c1)*x + c0;
```

Linear interpolation here is audible: its gain droops to 0.637 at half-sample offset, so an LFO sweeping the fractional part amplitude-modulates the loop at the mod rate. In a recirculating path that becomes a tremolo on the tail. Don't.

### 4.4 The shelf — low + high, frequency in pitch, gain in dB

SMD's `Shelf` takes `HF/HB/LF/LB`: high frequency + high gain, low frequency + low gain. Frequencies arrive via `P2F` (pitch → Hz), gains via `dB2AF` (dB → linear).

Two first-order shelves in series. High shelf:

```
       1 + A·K        A − 1
H(z):  b0 = ─────── , with K = tan(π·fc/fs), A = 10^(gain_dB/40)
```

Concretely, a first-order high shelf:

```c
// A = 10^(dB/40), K = tanf(M_PI * fc / fs)
float norm = 1.0f / (1.0f + K/A);
b0 = (1.0f + K*A) * norm... 
```

Rather than transcribe a table you'll get wrong, use the standard RBJ/Zölzer first-order shelf forms and **unit-test the magnitude response** (§9.3). What matters architecturally:

- **`HB` and `LB` are cuts, not boosts, for a dark voice.** `HB = −6 to −18 dB`, `LB = 0 to +3 dB`.
- **Hard constraint for anything inside the loop: shelf gain must be ≤ 0 dB at every frequency.** A `+2 dB` low shelf inside the late loop multiplies by 1.26 per pass; at 96 ms per pass that's runaway in under a second. Put low-frequency *boost* only in the **output** shelf (`EOLB`), never in the loop shelf (`LB`). This is the single easiest way to blow up this design.

**Distributed damping.** SMD embeds a shelf in *every* diffuser plus one after the chain — four shelf instances per section per channel. This is why it sounds creamy rather than filtered: many gentle stages instead of one steep one. A single `−12 dB` shelf sounds like a tone control; four `−3 dB` shelves in series sound like air absorption. Keep this. It's cheap.

### 4.5 Multi-phase LFO

One LFO, N equally-spaced phase taps. SMD uses 8 (four per channel, one per late delay).

```c
phase += freq * dt;  if (phase >= 1.0f) phase -= 1.0f;
for (int k = 0; k < 8; k++)
    mod[k] = depth * sinf(2.0f*M_PI*(phase + k*0.125f));
```

Output is a **delay-time offset in samples**, added after smoothing. Modulate the diffuser delays and the long delay alike.

Compute per control block, not per sample. Linearly interpolate the eight values across the block, or just step them — at 1 Hz and 48-sample blocks, stepping is inaudible.

**Depth:** SMD scales `ModA` by 0.001, implying a milliseconds→seconds conversion, so the knob is in ms. Start at **0.3–1.0 ms** (14–48 samples at 48 kHz). **Rate:** 0.3–1.5 Hz. Above ~2 Hz with meaningful depth it stops sounding like a room and starts sounding like a chorus pedal — which may be what you want for the Dead Medium voice, so leave the range open, but default low.

### 4.6 RT60 → feedback gain

```
g = 10^(−3 · D / T60)
```

where `D` is the **total loop delay in seconds** and `T60` the target decay. Standard and correct.

---

## 5. Full late-loop pseudocode

```c
// ---- per control block ----
float szL = LtSz - LtSym, szR = LtSz + LtSym;
for (int k = 0; k < 3; k++) {                 // diffuser stages, offsets +9,+6,+3
    tgtL[k] = pitch_to_samples(szL + 9 - 3*k, fs);
    tgtR[k] = pitch_to_samples(szR + 9 - 3*k, fs);
}
tgtL[3] = pitch_to_samples(szL, fs);          // long delay
tgtR[3] = pitch_to_samples(szR, fs);
for (int k = 0; k < 4; k++) {
    smL[k] += a_smooth * (tgtL[k] - smL[k]);
    smR[k] += a_smooth * (tgtR[k] - smR[k]);
}
float D = 0.0f;
for (int k = 0; k < 4; k++) D += 0.5f*(smL[k] + smR[k]);   // mean loop length, samples
fb_gain = powf(10.0f, -3.0f * (D / fs) / rt60_seconds);
fb_gain = fminf(fb_gain, 0.995f);
update_lfo_phases();
update_shelf_coeffs();

// ---- per sample ----
float aL = eOutL + fbR;      // cross-coupled
float aR = eOutR + fbL;

for (int k = 0; k < 3; k++) {
    float m = aL*C - aR*S;   // C = cosf(theta), S = sinf(theta)
    float n = aL*S + aR*C;
    aL = dlL[k].Read4pt(smL[k] + mod[k]);      dlL[k].Write(m);
    aR = dlR[k].Read4pt(smR[k] + mod[k+4]);    dlR[k].Write(n);
    aL = shelfL[k].Process(aL);
    aR = shelfR[k].Process(aR);
}
aL = shelfLout.Process(aL);
aR = shelfRout.Process(aR);

float outL = longL.Read4pt(smL[3] + mod[3]);   longL.Write(aL);
float outR = longR.Read4pt(smR[3] + mod[7]);   longR.Write(aR);

fbL = dcL.Process(outL) * fb_gain;             // DC block before feedback
fbR = dcR.Process(outR) * fb_gain;
lOutL = outL; lOutR = outR;
```

Note the `z⁻¹` on the feedback path — SMD shows it explicitly, and it's required: the cross-coupling `L' = eL + FBR` creates a delay-free loop otherwise. Reading `fbL`/`fbR` from the *previous* sample resolves it.

---

## 6. Stability

### 6.1 Loop gain budget

The late loop's per-pass gain is the product of:

| Element | Gain | Notes |
|---|---|---|
| 3 rotation stages | **exactly 1.0** | Orthogonal — proven lossless, §4.3 |
| 4 delay lines | ≤ 1.0 | 4-pt Lagrange is ≤ 1 everywhere, marginally under at fractional offsets |
| 4 shelves (3 embedded + 1 chain) | ≤ 1.0 **required** | Enforce max gain ≤ 0 dB |
| `fb_gain` | `10^(−3D/T60)` | Clamp to 0.995 |

Because the diffusers are provably lossless, **the loop is a strict contraction iff `fb_gain < 1` and no shelf exceeds 0 dB.** That's the entire stability condition. This is the payoff for using rotations instead of allpasses — with allpass diffusers you'd need to bound each stage's transient gain separately.

### 6.2 Modulation

Time-varying delay is a time-varying allpass — it adds no energy. Constraints:

1. Read index must never go negative or exceed the buffer: clamp `smoothed + mod` to `[4, MAX−4]` (4-point interpolation needs 2 samples of margin each side).
2. Rate of change of delay length must stay well under 1 sample/sample or the interpolator aliases. At 1 Hz and 1 ms depth: `dD/dt = 2π·1·48 ≈ 302 samples/s = 0.0063 samples/sample`. Two orders of magnitude of headroom. Fine.

### 6.3 The RT60 undercount — read this one

In SMD's `LateDiff`, the `RT60` macro appears to take its `DT` input from the **`Delay H` time only** (the `Sz` line), not from the sum of all four delays. If that reading is right, the computed feedback gain assumes a 30.58 ms loop when the actual loop is **96.09 ms — a factor of 3.143**.

The consequence: actual decay time is roughly **3.14× the value the knob claims**. Set 2 seconds, get about 6.3.

I suspect this is not a bug but the reason SMD sounds immense — the tail is always three times longer than the user expects, and users tune by ear, so the knob calibration never gets questioned.

**Recommendation:** implement it *correctly* (sum all four delays, as in the §5 pseudocode) and add a separate `Bloom` multiplier on `T60`, default 3.14, range 1–6. You get SMD's character on the default, honest calibration underneath, and a knob that makes the tail enormous. Verify the real decay with Schroeder integration (§9.5) rather than trusting either formula.

### 6.4 Denormals and DC

- **Set FZ (flush-to-zero) in FPSCR at init.** Decaying tails drive filter states subnormal; on Cortex-M7 subnormal handling costs tens of cycles per operation and will blow your budget in the exact moment the reverb is idle.
- **DC blocker on the feedback path** (`y[n] = x[n] − x[n−1] + 0.995·y[n−1]`). With `fb_gain` near 0.995 and asymmetric shelves, DC integrates. One per channel, in the loop, before the gain.
- Optional belt-and-braces: add `1e−20` to the feedback sample each pass.

---

## 7. Daisy implementation

### 7.1 Memory

At `fs = 48000`, worst case `Size` at the low end of its range (pitch 12 → 2936 samples):

| Buffer | Count | Max samples | Bytes |
|---|---|---|---|
| Predelay | 2 | 96000 (2 s) | 768 KB |
| Early diffusers | 6 | 4096 | 98 KB |
| Late diffusers | 6 | 8192 | 197 KB |
| Late long delay | 2 | 8192 | 66 KB |
| | | **Total** | **~1.1 MB** |

Round each buffer up to a power of two and use masked indexing rather than a modulo.

**Placement:**
- Predelay → **SDRAM** (`DSY_SDRAM_BSS`). It's read once per sample, sequentially, no modulation. Ideal SDRAM access pattern.
- Early + late delay lines → **internal SRAM**. Together ~360 KB, which fits the ~512 KB of internal RAM with room to spare. These are the hot buffers: 4-point interpolated reads touch four adjacent words at a jittering offset, which is the worst pattern for SDRAM latency.
- All filter state, coefficients, LFO phases → default `.bss` (internal).

**Critical:** SDRAM contents are undefined at boot and C++ constructors run before SDRAM is initialized. Explicitly zero the predelay buffer in `Init()` before the first read, and don't put anything with a meaningful constructor in SDRAM.

### 7.2 CPU

Per sample, per channel, late section: 3 rotations (4 mul + 2 add each), 4 four-point interpolated reads (~8 mul + 7 add each), 4 delay writes, 4 two-band shelves (~8 mul + 8 add each), 1 DC blocker. Roughly 200 flops/sample/channel. Early section is cheaper — no interpolation, no rotation. Call it **550 flops/sample** for the whole thing, ~26 Mflop/s at 48 kHz.

The M7 at 480 MHz with single-cycle FMA has enormous headroom for this. **The bottleneck will be memory access patterns, not arithmetic.** Profile with the actual delay lines in place before optimizing anything.

Block size: **48 samples** (1 ms). Below 16, per-block overhead dominates; above ~64 you're adding latency for no gain.

### 7.3 Parameter smoothing at the boundary

Everything the user can turn must be smoothed: delay times at 50 ms (§4.1), `fb_gain` at ~20 ms, shelf coefficients recomputed per block (the filters themselves smooth the transition adequately), `E/L` and `D/W` crossfades at ~10 ms. Use `fonepole()`.

---

## 8. Build order

Ten stages. Each is independently testable and each produces something you can listen to. Do not proceed to the next until the current one's tests pass.

| # | Stage | Done when |
|---|---|---|
| 1 | Host harness: portable DSP core, no libDaisy, renders WAV from CLI | `./verb in.wav out.wav` produces bit-identical passthrough |
| 2 | `pitch_to_samples`, smoother, delay line w/ 4-pt interpolation | §9.1, §9.2 pass |
| 3 | Two-band shelf | §9.3 passes; magnitude matches analytic within 0.1 dB |
| 4 | Single allpass diffuser w/ embedded shelf | §9.4 passes |
| 5 | Early section: 3 allpasses ×2ch, SzL/SzR, output shelf | Impulse shows dense uncorrelated ER pattern; L/R decorrelate as `Sym` rises |
| 6 | Rotation stage | §9.6 passes — energy preserved to 1e−6 |
| 7 | Late loop, **no modulation**, low `fb_gain` (0.5) | Stable; §9.5 measured T60 tracks formula within 5% |
| 8 | Late loop at `fb_gain = 0.995` | §9.7 soak passes: 10 min noise, no NaN, bounded, no DC drift |
| 9 | Multi-phase LFO on late delays | Tail thickens; no tremolo artifact (that would mean linear interp crept in) |
| 10 | Port to Seed: SDRAM predelay, FZ bit, knob mapping, profiling | Runs at <50% CPU with headroom for Size at minimum |

Stages 1–9 happen entirely on the desktop. You should not flash the Seed until stage 10.

---

## 9. Test suite

Host-side, doctest or Catch2. All of these are fast enough to run on every build.

**9.1 Pitch conversion.** `pitch_to_samples(69, 48000)` == `48000/440` ± 1e−4. Assert the octave property: `pitch_to_samples(p) == 2 · pitch_to_samples(p+12)` for p ∈ [12, 60]. Assert monotonic decreasing.

**9.2 Fractional delay.** Write a known ramp; read at integer offsets and assert exact match. Read at `k + 0.5` and assert the Lagrange result matches the analytic cubic through those four points. Sweep a sine through a modulated read and assert output amplitude stays within 0.5% — this is the test that catches accidental linear interpolation.

**9.3 Shelf magnitude.** Sweep sine, measure steady-state gain at 20 log-spaced frequencies, compare to the analytic transfer function. Within 0.1 dB. Separately assert `max|H(ω)| ≤ 1.0` for every parameter set the loop is allowed to use — this is the §6.1 stability precondition, enforced as a test.

**9.4 Allpass flatness.** With the embedded shelf bypassed, assert `|H(ω)| == 1` within 0.01 dB across 20 Hz–20 kHz. Catches sign errors in the diffuser.

**9.5 T60 via Schroeder integration.** Impulse in, capture 10 s of tail. Compute the backward-integrated energy decay curve, fit a line over the −5 to −35 dB region, extrapolate to −60. Assert measured T60 matches the target within 5%. Run at `T60` ∈ {0.5, 2, 8} s. This is the test that tells you whether §6.3 is real.

**9.6 Rotation orthogonality.** For θ ∈ {0, π/6, π/4, π/3, π/2} and 1000 random input vectors, assert `‖R x‖² == ‖x‖²` to 1e−6. Assert `RᵀR == I`. Trivial test, catches a whole class of catastrophic bugs.

**9.7 Stability soak.** 10 minutes of full-scale white noise, then 10 minutes of silence, at `fb_gain = 0.995`, `T60 = 60 s`, all shelves flat, `Sym` and `Size` sweeping. Assert: no NaN or Inf ever; `|out| < 4.0` always; running mean over the final 10 s is `< 1e−4` (DC check); final RMS during the silence tail decays monotonically.

**9.8 Echo density.** Count zero-crossing-adjacent peaks per 10 ms window in the impulse response. Assert monotonic increase through the early section and continued increase for the first ~500 ms of the late tail. Also compute the autocorrelation of the tail and assert no peak above 0.3 outside lag 0 — a strong peak means periodicity, i.e. metallic ring.

**9.9 Null test.** Keep a reference build. After any refactor, render the same input through both and assert the difference is below −120 dBFS.

---

## 10. Tuning roadmap

Tune in this order. Each phase assumes the previous is frozen.

**Phase 1 — structural, freeze before listening seriously.** Number of diffuser stages (3 early, 3 late), the interval offsets (`+8/+4/0` and `+9/+6/+3/0`), rotation angle (π/4), interpolation order. These define the grain of the thing. Changing them later invalidates all your ear-tuning.

**Phase 2 — the dark voice.** In-loop shelf `HF` (start pitch ~96, ≈2 kHz) and `HB` (start −6 dB). This is where "cavernous" lives. Push `HB` toward −12 dB and `HF` down toward 1 kHz for full Dead Medium murk. Then `LF`/`LB` — keep `LB` at 0 dB or slightly negative in the loop (§4.4 warning), and put any low-end weight in the **output** shelf instead: `EOLF` ≈ 350 Hz, `EOLB` ≈ +3 dB.

**Phase 3 — scale.** `LtSz` (late size) is the single most character-defining knob. 24 is the default; go down to 18 for a genuinely enormous space, up to 33 for something tighter. Then `RT60` / `Bloom`. Then `ErSz` — early size mostly affects the sense of proximity; keep it well above late size (30 vs 24) so early reads as "close" and late as "far."

**Phase 4 — width.** `ERSym` and `LtSym` at 0.5–2.0. Do this after scale, because the perceptually correct amount of symmetry offset depends on how big the network is. `PDSym` last, and small.

**Phase 5 — motion.** `ModA` 0.3–1.0 ms, `ModF` 0.3–1.5 Hz. Genuinely last. Modulation masks problems — if you tune it early you'll paper over a diffuser that's ringing and never find out.

**Phase 6 — balance.** `E/L` crossfade and `D/W`. For dark ambient, `E/L` heavily toward late (0.8–0.9) and `D/W` often fully wet.

### Live tweaking

Build stage 1's host harness to read parameters from a plain text file re-read on each render, then keep a shell loop that re-renders and plays on file change. You will iterate 50× faster than reflashing. Lock the constants there, then hard-code them for the Seed build and map only the handful you actually want on knobs.

For the Seed itself: 8 knobs is the realistic budget. Suggested mapping — `LtSz` (Size), `RT60`, `HB` (Dark), `ModA` (Motion), `LtSym` (Width), `E/L`, `PreDel`, `D/W`. Everything else becomes a compile-time constant.

---

## 11. Open questions to settle on hardware

1. **Is the §6.3 RT60 undercount real?** Verify by measuring your own implementation's T60 with and without summing all four delays. If SMD's character depends on it, the `Bloom` default of 3.14 is right; if not, drop `Bloom` to 1.0.
2. **Does `LT` in the SMD top level carry audio or a modulation signal?** It routes from `LateDiff`'s `M1` and reaches a separate output. Reads more like a metering/mod tap than an audio path. Ignore it unless you find a use.
3. **How does the `RT60` knob scale?** The `dB2AF → ×0.001` chain suggests an exponential knob in dB, where 60 dB → 1 s and 80 dB → 10 s. Inference, not confirmed. Use a plain exponential seconds mapping (0.2–30 s) unless you find otherwise.
4. **Is 3 late diffuser stages enough?** SMD uses 3 + the long delay. If the tail sounds grainy at large `Size`, add a fourth at `Sz+12` (which is exactly one octave — the *only* offset that would break the irrational-ratio property, so use `Sz+11` or `Sz+13` instead).
5. **Does the early section want its own predelay tap?** SMD feeds both sections from the same predelay. Splitting them would let early stay tight while late sits further back. Cheap to try, might be the difference between "big room" and "vast".

---

## Appendix: default parameter set

```c
constexpr float kPreDel      = 40.0f;   // ms
constexpr float kPDSym       = 0.3f;    // semitones equivalent offset
constexpr float kErSz        = 30.0f;   // pitch
constexpr float kERSym       = 1.0f;    // semitones
constexpr float kErDffs      = 0.68f;
constexpr float kLtSz        = 24.0f;   // pitch
constexpr float kLtSym       = 1.2f;    // semitones
constexpr float kLtTheta     = 0.7853982f;  // pi/4
constexpr float kRT60        = 4.0f;    // seconds
constexpr float kBloom       = 3.14f;
constexpr float kModA        = 0.6f;    // ms
constexpr float kModF        = 0.7f;    // Hz
constexpr float kDmpHF       = 96.0f;   // pitch -> ~2093 Hz
constexpr float kDmpHB       = -7.0f;   // dB   (in-loop: must be <= 0)
constexpr float kDmpLF       = 60.0f;   // pitch -> ~262 Hz
constexpr float kDmpLB       = 0.0f;    // dB   (in-loop: must be <= 0)
constexpr float kEOHF        = 100.0f;  // pitch -> ~2637 Hz
constexpr float kEOHB        = -3.0f;   // dB   (output: boost allowed)
constexpr float kEOLF        = 65.0f;   // pitch -> ~349 Hz
constexpr float kEOLB        = 3.0f;    // dB
constexpr float kEL          = 0.85f;   // 0 = early, 1 = late
constexpr float kDW          = 1.0f;    // fully wet
constexpr float kFBCeiling   = 0.995f;
constexpr float kSmoothTC    = 0.05f;   // seconds
```
