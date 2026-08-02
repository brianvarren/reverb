// X(id, key, default, lo, hi, curve, unit, label)
//
// This file is #included multiple times with different definitions of X.
// It is not a standalone header — no include guards, no #pragma once.

// ── PREDELAY ──────────────────────────────────────────────────────────────────

// predelay_ms  [0 – 170 ms, linear]
// A fixed delay before both channels enter the diffusion network.
// The gap between the direct sound and the first reflected energy is the
// primary cue the ear uses to judge source distance and room size: longer
// predelay reads as a larger, more distant space.  Below ~15 ms the reverb
// merges with the source.  Above ~60 ms it separates into a distinct audible
// event — useful for tape-delay character, disorienting at long decay times.
// The ceiling is 170 ms, set by the predelay buffer size.
X(predelay_ms, "predelay_ms",      40.f,    0.f,  170.f,  kLin,   "ms",   "Pre-delay"        )

// pd_sym  [0 – 25 ms, squared]
// Offsets L and R predelay times in opposite directions:
//   T_L = predelay_ms − pd_sym,  T_R = predelay_ms + pd_sym
// Real rooms create stereo width for a centre source primarily through
// inter-channel arrival-time differences; this replicates that cheaply and
// statically, before any diffusion.  Squared mapping keeps the useful 0–5 ms
// range across the bottom half of the knob.  Above ~8 ms the channels start
// to feel like two independent reverbs rather than one wide one.
X(pd_sym,      "pd_sym",            0.3f,   0.f,   25.f,  kSq,    "ms",   "Pre-delay spread" )

// ── EARLY SECTION ─────────────────────────────────────────────────────────────
// Three Schroeder allpasses in series per channel, followed by a two-band
// shelf.  No cross-feed between L and R.  No feedback.  Stereo width comes
// entirely from the SzL / SzR asymmetry set by er_sym.

// er_sz  [pitch 18 – 48, pitch curve]
// Base delay length for the early allpass chain, in MIDI pitch units.
// N(p) = fs / (440 · 2^((p−69)/12)).  The three stages sit at offsets
// +8, +4, 0 semitones above er_sz, giving irrational length ratios
// 2^(8/12) : 2^(4/12) : 1 ≈ 1.587 : 1.260 : 1 — the echo trains never
// realign at any er_sz.  Pitch 30 (default) → 1038 samples ≈ 21.6 ms.
// Smaller: tight, close reflections, reads as a small space.
// Larger: echoes further apart, reads as a large hall or cave.
// This shapes the first 50–100 ms — the part that defines room geometry.
X(er_sz,       "er_sz",            30.f,   18.f,   48.f,  kPitch, "pitch","Early size"       )

// er_sym  [0 – 6 semitones, linear]
// Detunes L and R early chains by ±er_sym semitones:
//   SzL = er_sz − er_sym,  SzR = er_sz + er_sym
// ±N semitones scales each channel by reciprocal factors 2^(∓N/12),
// keeping the two chains proportionally offset at any er_sz.  Even 0.5
// semitones (~3%) fully decorrelates the two channels' reflection patterns.
// Keep below ~4 semi; above that the channels sound like different rooms.
X(er_sym,      "er_sym",            1.f,    0.f,    6.f,  kLin,   "semi", "Early width"      )

// er_dffs  [0.45 – 0.80, linear]
// Schroeder allpass coefficient g.  In the allpass structure:
//   w[n] = x[n] + g · shelf(w[n−M]),  y[n] = shelf(w[n−M]) − g · w[n]
// At g = 0: degenerates to a pure delay — three discrete echoes.
// At g ≈ 0.707: magnitude response is maximally flat and energy is spread
// most evenly across time.  Approaching 1: denser but buzzy — the internal
// feedback starts to resonate.
// 0.65–0.75 is the sweet spot: diffuse without harshness.
X(er_dffs,     "er_dffs",           0.68f,  0.45f,  0.80f, kLin,  "",     "Early diffusion"  )

// ── LATE SECTION ──────────────────────────────────────────────────────────────
// Cross-coupled stereo feedback loop: three rotation-diffuser stages, a chain
// shelf, two long delay lines, and an 8-phase LFO modulating every tap.
// This is the recirculating core that builds and sustains the reverb tail.

// lt_sz  [pitch 12 – 42, pitch curve]
// Base size for the late loop, same pitch-domain scheme as er_sz.  Four
// delays per channel at Sz+9, Sz+6, Sz+3, Sz give ratios
// 2^(9/12) : 2^(6/12) : 2^(3/12) : 1 ≈ 1.682 : 1.414 : 1.189 : 1.
// At default pitch 24: loop total ≈ 96 ms.
// Primary room-size control.  Smaller: tighter, more resonant, modal
// colouration audible (individual frequencies ring before fading).  Larger:
// sparser echoes, more diffuse and washy, less colouration but also less
// sense of a defined space.
X(lt_sz,       "lt_sz",            24.f,   12.f,   42.f,  kPitch, "pitch","Late size"        )

// lt_sym  [0 – 8 semitones, linear]
// SzL / SzR detune for the late loop.  The two cross-coupled channels run at
// slightly different loop lengths, offsetting their resonant frequencies so
// neither rings in sympathy with the other — a wide, tonally neutral tail.
// 1–2 semi is usually sufficient; large values make the channels sound
// tonally dissimilar.
X(lt_sym,      "lt_sym",            1.2f,   0.f,    8.f,  kLin,   "semi", "Late width"       )

// lt_theta  [0 – π/4 radians, linear]
// Rotation angle in the 2×2 orthogonal diffuser before each delay read:
//   [a', b'] = [a·cosθ − b·sinθ,  a·sinθ + b·cosθ]
// θ = 0: identity matrix, rails fully decoupled — echoes, no diffusion.
// θ = π/4 (default): maximum mixing, each output is equal parts of both.
// The matrix is orthogonal (RᵀR = I), so ‖Ra‖ = ‖a‖: energy is
// redistributed but never created — safe under recirculation at any gain.
// Low θ: loose, echo-y tail, clear stereo separation between rails.
// High θ: density builds faster, smoother and more diffuse.
// Reduce from π/4 for a more articulate, less washy character.
X(lt_theta,    "lt_theta",          0.7853982f, 0.f, 0.7853982f, kLin, "rad", "Late scatter" )

// ── DECAY ─────────────────────────────────────────────────────────────────────

// rt60_s  [0.2 – 30 s, exponential]
// Target reverberation time — time for the tail to decay 60 dB.
// Feedback gain is derived as:
//   fb_gain = 10^(−3 · loop_length_s / (rt60_s · bloom)),  clamped to 0.995
// Exponential knob curve: decay is perceptually logarithmic, so 0.5→1 s
// sounds the same distance as 2→4 s.
// Note: in-loop shelves absorb HF on every pass, so measured T60 at high
// frequencies is shorter than this value — rt60_s targets mid-band decay.
// This is normal; real rooms have frequency-dependent decay too.
X(rt60_s,      "rt60_s",            4.f,    0.2f,  30.f,  kExp,   "s",    "Decay"            )

// bloom  [1 – 6×, linear]
// Multiplier on the effective RT60 used in the fb_gain formula:
//   fb_gain targets a decay of  rt60_s · bloom  seconds
// This sets fb_gain higher than the in-loop shelves can sustain, so the tail
// initially grows in apparent level before it fades — the audible "bloom" or
// swell that is the Space Master Deluxe's characteristic sound.  At bloom = 1
// the formula is the standard Schroeder result: the tail falls immediately
// from onset.  At the default (π ≈ 3.14) fb_gain targets ~3× the realised
// decay — rich, late-building.
// Reduce bloom if the tail sounds unstable or overly swollen.
X(bloom,       "bloom",             3.14f,  1.f,    6.f,  kLin,   "x",    "Bloom"            )

// ── MODULATION ────────────────────────────────────────────────────────────────

// mod_ms  [0 – 6 ms, squared]
// Peak amplitude of sinusoidal pitch modulation on all eight delay taps
// (4 per channel: 3 diffuser stages + 1 long delay).  Each tap gets a unique
// phase: tap_offset[k] = depth_samp · sin(2π·(phase + k/8)), so the eight
// modulators never all peak at once.
// Modulation breaks the loop's resonant modes: without it, a long decay at
// high fb_gain produces pitched artefacts as individual frequencies sustain
// while others fade.  0.3–1 ms is inaudible as pitch but enough to smooth
// spectral density.  Above ~2 ms: shimmer or chorus.  Above 4 ms the
// modulation itself dominates the timbral character.
// Squared mapping keeps 0.2–1.5 ms across the bottom half of the knob.
X(mod_ms,      "mod_ms",            0.6f,   0.f,    6.f,  kSq,    "ms",   "Motion depth"     )

// mod_hz  [0.02 – 5 Hz, exponential]
// LFO rate driving all eight modulators.  Equally-spaced phases provide
// 8-fold coverage of the modulation cycle at any rate.
// < 0.2 Hz: barely perceptible breathing — tail slowly pulses in density.
// 0.7 Hz (default): below obvious chorus, fast enough to prevent spectral
// freezing on long decays.
// > 2 Hz: audible pitch vibrato in the tail.  > 4 Hz: thick flutter.
// Exponential mapping because useful rates span two decades.
X(mod_hz,      "mod_hz",            0.7f,   0.02f,  5.f,  kExp,   "Hz",   "Motion rate"      )

// ── IN-LOOP SHELVES (dmp_*) ───────────────────────────────────────────────────
// Two-band shelf inside the feedback loop: one per rotation-diffuser stage
// (×3) plus one chain shelf = four filter passes per round-trip per channel.
// Gains are hard-capped at 0 dB — any gain above unity pushes loop gain
// above fb_gain, risking instability.  Shelf<true>::SetParams asserts this.

// dmp_hf  [pitch 60 – 120, pitch curve]
// Corner frequency of the in-loop high shelf.
// pitch_to_hz(60) ≈ 261 Hz,  pitch_to_hz(120) ≈ 8372 Hz.
// Controls where HF roll-off begins on each loop pass.
// Lower: roll-off starts earlier — darker, warmer tail.
// Higher: more high content circulates before being absorbed — brighter.
X(dmp_hf,      "dmp_hf",           96.f,   60.f,  120.f,  kPitch, "pitch","Damp HF corner"  )

// dmp_hb  [−24 – 0 dB, linear]
// Gain of the in-loop high shelf.  The primary "Dark" control.
// At 0 dB: flat — highs decay at the same rate as mids.
// At −7 dB (default): each loop pass removes 7 dB of high content, so over
// N passes highs have decayed N×7 dB more than mids.  This is what makes a
// reverb dark — highs fade first, leaving a progressively lower-frequency
// tail.  At −24 dB: HF is nearly gone within the first few passes.
X(dmp_hb,      "dmp_hb",           -7.f,  -24.f,    0.f,  kLin,   "dB",   "Dark"             )

// dmp_lf  [pitch 24 – 84, pitch curve]
// Corner frequency of the in-loop low shelf.
// pitch_to_hz(24) ≈ 33 Hz,  pitch_to_hz(84) ≈ 1047 Hz.
// Higher values push the corner into the midrange, thinning more bass on
// each pass.
X(dmp_lf,      "dmp_lf",           60.f,   24.f,   84.f,  kPitch, "pitch","Damp LF corner"  )

// dmp_lb  [−24 – 0 dB, linear]
// Gain of the in-loop low shelf.  "Thin" — reduces bass accumulation.
// At 0 dB (default): bass circulates freely; the tail can become thick or
// muddy on bass-heavy material.  −6 to −12 dB clears low-end buildup without
// touching the mid/high character.  Essential on full mixes or bass
// instruments.  Below −18 dB the tail sounds noticeably thin.
X(dmp_lb,      "dmp_lb",            0.f,  -24.f,    0.f,  kLin,   "dB",   "Thin"             )

// ── OUTPUT SHELF (eo_*) ───────────────────────────────────────────────────────
// Applied to the wet signal after the loop, before the D/W crossfade.
// Outside the feedback path — no stability constraint, boost is allowed.
// Does not affect decay time, only the tonal colour of the wet output.

// eo_hf  [pitch 60 – 120, pitch curve]
// Corner of the output high shelf.  Same pitch-to-Hz as dmp_hf.
// Setting this higher than dmp_hf lets you boost only the very top ("air")
// without affecting the main body of the reverb.
X(eo_hf,       "eo_hf",           100.f,   60.f,  120.f,  kPitch, "pitch","Out HF corner"   )

// eo_hb  [−18 – +12 dB, linear]
// Gain of the output high shelf.  Positive: adds air or sparkle — restores
// brightness the in-loop damping removed, or intentionally brightens a dark
// room.  Negative: further dulls the wet signal, useful for sub-mixing under
// a lead.  Safe to boost here; this shelf is outside the loop.
X(eo_hb,       "eo_hb",            -3.f,  -18.f,   12.f,  kLin,   "dB",   "Out HF gain"     )

// eo_lf  [pitch 24 – 84, pitch curve]
// Corner of the output low shelf.  Same pitch-to-Hz as dmp_lf.
X(eo_lf,       "eo_lf",            65.f,   24.f,   84.f,  kPitch, "pitch","Out LF corner"   )

// eo_lb  [−18 – +12 dB, linear]
// Gain of the output low shelf.  Default +3 dB compensates for the bass
// dmp_lb removes from the loop — restores low-end body without the muddiness
// of free bass recirculation.  Cut to thin the reverb; boost to add warmth.
X(eo_lb,       "eo_lb",             3.f,  -18.f,   12.f,  kLin,   "dB",   "Out LF gain"     )

// ── CROSSFADES ────────────────────────────────────────────────────────────────
// Both use equal-power (cos/sin) mixing.  Linear mixing creates a 3 dB hole
// at the midpoint when the two signals are decorrelated — which they always
// are here.

// el_mix  [0 – 1, linear]
// Blend between early section output and late loop output.
// 0: early only — discrete taps, transient detail, no sustained tail.
// 1: late only — smooth sustained tail, no initial echo structure.
// Early shapes perceived room geometry; late provides sustain and density.
// Default 0.85 (mostly late) gives a large reverb that still sounds like a
// space.  Lower (0.4–0.6): more room character.  Higher: ambient texture.
X(el_mix,      "el_mix",            0.85f,  0.f,    1.f,  kLin,   "",     "Early/Late"       )

// dw_mix  [0 – 1, linear]
// Dry/wet blend.  0: dry only.  1: wet only — correct for send/return use.
// For insert use on a single instrument, blend to taste.  Equal-power
// crossfade: dry level does not change as wet is faded in.
X(dw_mix,      "dw_mix",            1.f,    0.f,    1.f,  kLin,   "",     "Dry/Wet"          )
