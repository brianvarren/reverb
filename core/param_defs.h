#pragma once
#include <cmath>
#include "util.h"

// ─────────────────────────────────────────────────────────────────────────────
// THE parameter list — single source of truth.
//
// Everything that knows about parameters is generated from VERB_PARAM_LIST:
//   • the Params struct                (core/params.h)
//   • the params.txt parser            (host/main.cpp)
//   • range clamping / validation      (ParamsClamp)
//   • normalised knob mapping 0..1     (ParamMapNorm)  — stage 10
//   • --dump-params reference output   (host/main.cpp)
//
// To add a parameter: one line in VERB_PARAM_LIST. Nothing else.
//
// X(id, key, default, lo, hi, curve, unit, label)
//
// curve  controls the 0..1 → value mapping used by hardware knobs and any UI:
//   kLin    linear                       v = lo + x(hi−lo)
//   kExp    exponential (lo must be > 0) v = lo · (hi/lo)^x    — time, Hz
//   kSq     squared                      v = lo + (hi−lo)·x²   — depths
//   kPitch  linear in MIDI pitch — already exponential in delay time,
//           stored linear but flagged so a UI can label it in Hz or ms.
//
// Comments inside the macro must use /* */ style — // would consume the \.
// ─────────────────────────────────────────────────────────────────────────────

enum ParamCurve { kLin, kExp, kSq, kPitch };

#define VERB_PARAM_LIST(X)                                                     \
                                                                               \
    /* ── PREDELAY ──────────────────────────────────────────────────────── */ \
                                                                               \
    /* predelay_ms  [0 – 170 ms, linear]                                    */ \
    /* A fixed delay before both channels enter the diffusion network.       */ \
    /* The gap between the direct sound and the first reflected energy is    */ \
    /* the primary cue the ear uses to judge source distance and room size:  */ \
    /* longer predelay reads as a larger, more distant space.  Below ~15 ms  */ \
    /* the reverb merges with the source.  Above ~60 ms it separates into a  */ \
    /* distinct audible event — useful for tape-delay character, disorienting */ \
    /* at long decay times.  The ceiling is 170 ms, set by the buffer size.  */ \
    X(predelay_ms, "predelay_ms",      40.f,    0.f,  300.f,  kLin,   "ms",   "Pre-delay"          ) \
                                                                               \
    /* pd_sym  [0 – 25 ms, squared]                                         */ \
    /* Offsets L and R predelay times in opposite directions:                */ \
    /*   T_L = predelay_ms − pd_sym,  T_R = predelay_ms + pd_sym            */ \
    /* Real rooms create stereo width for a centre source primarily through  */ \
    /* inter-channel arrival-time differences; this replicates that cheaply  */ \
    /* and statically, before any diffusion.  Squared mapping keeps the      */ \
    /* useful 0–5 ms range across the bottom half of the knob.  Above ~8 ms  */ \
    /* the channels start to feel like two independent reverbs.              */ \
    X(pd_sym,      "pd_sym",            0.3f,   0.f,  100.f,  kSq,    "ms",   "Pre-delay spread"   ) \
                                                                               \
    /* ── EARLY SECTION ─────────────────────────────────────────────────── */ \
    /* Three Schroeder allpasses in series, per channel, followed by a       */ \
    /* two-band shelf.  No cross-feed between L and R.  No feedback.  Stereo */ \
    /* width comes entirely from the SzL / SzR asymmetry set by er_sym.     */ \
                                                                               \
    /* er_sz  [pitch 18 – 48, pitch curve]                                  */ \
    /* Base delay length for the early allpass chain, in MIDI pitch units.   */ \
    /* N(p) = fs / (440 · 2^((p−69)/12)).  The three stages are placed at   */ \
    /* offsets +8, +4, 0 semitones above er_sz, giving irrational length     */ \
    /* ratios 2^(8/12) : 2^(4/12) : 1 ≈ 1.587 : 1.260 : 1 — the echo       */ \
    /* trains never realign at any er_sz.  Pitch 30 → 1038 samples ≈ 21.6 ms.*/ \
    /* Smaller values: tight, close reflections, reads as a small space.     */ \
    /* Larger values: echoes further apart, reads as a large hall or cave.   */ \
    /* This shapes the first 50–100 ms — the part that defines room geometry.*/ \
    X(er_sz,       "er_sz",            30.f,    6.f,   66.f,  kPitch, "pitch","Early size"         ) \
                                                                               \
    /* er_sym  [0 – 6 semitones, linear]                                    */ \
    /* Detunes L and R early chains by ±er_sym semitones:                   */ \
    /*   SzL = er_sz − er_sym,  SzR = er_sz + er_sym                        */ \
    /* ±N semitones scales each channel by reciprocal factors 2^(∓N/12),    */ \
    /* so the two chains stay proportionally offset at any er_sz.  Even      */ \
    /* 0.5 semitones (~3%) fully decorrelates the two reflection patterns.   */ \
    /* Keep below ~4 semi; above that the channels sound like different rooms.*/ \
    X(er_sym,      "er_sym",            1.f,    0.f,   12.f,  kLin,   "semi", "Early width"        ) \
                                                                               \
    /* er_dffs  [0.45 – 0.80, linear]                                       */ \
    /* Schroeder allpass coefficient g.  In the allpass structure:           */ \
    /*   w[n] = x[n] + g · shelf(w[n−M]),  y[n] = shelf(w[n−M]) − g · w[n] */ \
    /* At g = 0: degenerates to a pure delay, you hear three discrete echoes.*/ \
    /* At g ≈ 0.707: magnitude response is maximally flat and energy is      */ \
    /* spread most evenly across time.  Approaching 1: denser but buzzy —   */ \
    /* the allpass internal feedback starts to resonate.                     */ \
    /* 0.65–0.75 is the sweet spot: diffuse without harshness.              */ \
    X(er_dffs,     "er_dffs",           0.68f,  0.0f,   0.98f,kLin,   "",     "Early diffusion"    ) \
                                                                               \
    /* ── LATE SECTION ──────────────────────────────────────────────────── */ \
    /* Cross-coupled stereo feedback loop: three rotation-diffuser stages,   */ \
    /* a chain shelf, two long delay lines, and an 8-phase LFO on every tap.*/ \
    /* This is the recirculating core that builds and sustains the tail.     */ \
                                                                               \
    /* lt_sz  [pitch 12 – 42, pitch curve]                                  */ \
    /* Base size for the late loop, same pitch-domain scheme as er_sz.       */ \
    /* Four delays per channel at Sz+9, Sz+6, Sz+3, Sz give ratios          */ \
    /* 2^(9/12) : 2^(6/12) : 2^(3/12) : 1 ≈ 1.682 : 1.414 : 1.189 : 1.    */ \
    /* At default pitch 24: loop total ≈ 96 ms.                              */ \
    /* Primary room-size control.  Smaller: tighter tail, more resonant,     */ \
    /* audible modal colouration (individual frequencies ring before fading). */ \
    /* Larger: sparser echoes, more diffuse, "washy" — less colouration but  */ \
    /* also less sense of a defined space.                                   */ \
    X(lt_sz,       "lt_sz",            24.f,    6.f,   54.f,  kPitch, "pitch","Late size"          ) \
                                                                               \
    /* lt_sym  [0 – 8 semitones, linear]                                    */ \
    /* SzL / SzR detune for the late loop.  The two cross-coupled channels  */ \
    /* run at slightly different loop lengths, offsetting their resonant     */ \
    /* frequencies so neither rings in sympathy with the other.  Result:     */ \
    /* a wide, tonally neutral late tail.  1–2 semi is usually enough;       */ \
    /* large values make the two channels sound tonally dissimilar.          */ \
    X(lt_sym,      "lt_sym",            1.2f,   0.f,   12.f,  kLin,   "semi", "Late width"         ) \
                                                                               \
    /* lt_theta  [0 – π/4 radians, linear]                                  */ \
    /* Rotation angle in the 2×2 orthogonal diffuser before each delay read: */ \
    /*   [a', b'] = [a·cosθ − b·sinθ,  a·sinθ + b·cosθ]                     */ \
    /* θ = 0: identity matrix, rails fully decoupled — echoes, no diffusion. */ \
    /* θ = π/4 (default): maximum mixing, each output is equal parts of both.*/ \
    /* The matrix is orthogonal (RᵀR = I), so ‖Ra‖ = ‖a‖: energy is         */ \
    /* redistributed but never created, safe under recirculation.            */ \
    /* Low angles: loose, echo-y tail, clear stereo separation between rails. */ \
    /* High angles: density builds faster, smoother and more diffuse tail.   */ \
    /* Reduce from π/4 for a more articulate, less washy character.          */ \
    X(lt_theta,    "lt_theta",          0.7853982f, 0.f, 0.7853982f, kLin, "rad", "Late scatter"   ) \
                                                                               \
    /* ── DECAY ─────────────────────────────────────────────────────────── */ \
                                                                               \
    /* rt60_s  [0.2 – 30 s, exponential]                                    */ \
    /* Target reverberation time — time for the tail to decay 60 dB.         */ \
    /* Feedback gain is derived as:                                          */ \
    /*   fb_gain = 10^(−3 · loop_length_s / (rt60_s · bloom)),  max 0.995   */ \
    /* Exponential knob curve: decay is perceptually logarithmic, so 0.5→1 s */ \
    /* sounds the same distance as 2→4 s.                                    */ \
    /* Note: in-loop shelves absorb HF on every pass, so measured T60 at     */ \
    /* high frequencies is shorter than this value — rt60_s targets mids.    */ \
    /* This is normal; real rooms have frequency-dependent decay too.         */ \
    X(rt60_s,      "rt60_s",            4.f,    0.05f, 120.f,  kExp,   "s",    "Decay"              ) \
                                                                               \
    /* bloom  [1 – 6×, linear]                                              */ \
    /* Multiplier on the effective RT60 used in the fb_gain formula:         */ \
    /*   fb_gain targets a decay of  rt60_s · bloom  seconds                 */ \
    /* This sets fb_gain higher than the in-loop shelves can sustain, so the */ \
    /* tail initially grows in apparent level before it fades — the audible  */ \
    /* "bloom" or swell that is the Space Master Deluxe's characteristic     */ \
    /* sound.  At bloom = 1 the formula is the standard Schroeder result and */ \
    /* the tail falls immediately from the onset.  At the default (π ≈ 3.14) */ \
    /* fb_gain targets ~3× the realised decay — rich, late-building.        */ \
    /* Reduce bloom if the tail sounds unstable or overly swollen.           */ \
    X(bloom,       "bloom",             3.14f,  0.25f,  12.f,  kLin,   "x",    "Bloom"              ) \
                                                                               \
    /* ── MODULATION ────────────────────────────────────────────────────── */ \
                                                                               \
    /* mod_ms  [0 – 6 ms, squared]                                          */ \
    /* Peak amplitude of sinusoidal pitch modulation on all eight delay taps */ \
    /* (4 per channel: 3 diffuser stages + 1 long delay).  Each tap gets a   */ \
    /* unique phase: tap_offset[k] = depth_samp · sin(2π·(phase + k/8)),    */ \
    /* so the eight modulators never all peak at once.                       */ \
    /* Modulation breaks the loop's resonant modes: without it, a long decay */ \
    /* at high fb_gain produces pitched artefacts as individual frequencies   */ \
    /* sustain while others fade.  0.3–1 ms is inaudible as pitch but enough */ \
    /* to smooth spectral density.  Above ~2 ms: shimmer or chorus.          */ \
    /* Above 4 ms the modulation itself dominates the timbral character.     */ \
    /* Squared mapping keeps 0.2–1.5 ms across the bottom half of the knob. */ \
    X(mod_ms,      "mod_ms",            0.6f,   0.f,   20.f,  kSq,    "ms",   "Motion depth"       ) \
                                                                               \
    /* mod_hz  [0.02 – 5 Hz, exponential]                                   */ \
    /* LFO rate driving all eight modulators.  Equally-spaced phases provide */ \
    /* 8-fold coverage of the modulation cycle at any rate.                  */ \
    /* < 0.2 Hz: barely perceptible breathing — tail slowly pulses in density.*/ \
    /* 0.7 Hz (default): below obvious chorus, fast enough to prevent        */ \
    /* spectral freezing on long decays.                                     */ \
    /* > 2 Hz: audible pitch vibrato in the tail.  > 4 Hz: thick flutter.   */ \
    /* Exponential mapping because useful rates span two decades.            */ \
    X(mod_hz,      "mod_hz",            0.7f,   0.005f, 20.f,  kExp,   "Hz",   "Motion rate"        ) \
                                                                               \
    /* ── IN-LOOP SHELVES (dmp_*) ───────────────────────────────────────── */ \
    /* Two-band shelf inside the feedback loop: one per rotation-diffuser    */ \
    /* stage (×3) plus one chain shelf = four filter passes per round-trip.  */ \
    /* Gains are hard-capped at 0 dB — any gain above unity would push loop  */ \
    /* gain above fb_gain and risk instability.  Shelf<true>::SetParams       */ \
    /* asserts this structurally.                                            */ \
                                                                               \
    /* dmp_hf  [pitch 60 – 120, pitch curve]                                */ \
    /* Corner frequency of the in-loop high shelf.                           */ \
    /* pitch_to_hz(60) ≈ 261 Hz,  pitch_to_hz(120) ≈ 8372 Hz.              */ \
    /* Controls where HF roll-off begins each loop pass.  Lower = earlier    */ \
    /* roll-off, darker and warmer tail.  Higher = roll-off starts later,    */ \
    /* more high content circulates before being absorbed.                   */ \
    X(dmp_hf,      "dmp_hf",           96.f,   24.f,  127.f,  kPitch, "pitch","Damp HF corner"     ) \
                                                                               \
    /* dmp_hb  [−24 – 0 dB, linear]                                         */ \
    /* Gain of the in-loop high shelf.  The primary "Dark" control.          */ \
    /* At 0 dB: flat — highs decay at the same rate as mids.                */ \
    /* At −7 dB (default): each loop pass removes 7 dB of high content, so  */ \
    /* over N passes highs have decayed N×7 dB more than mids.  This is what */ \
    /* makes a reverb "dark" — highs fade first, leaving a progressively      */ \
    /* lower-frequency tail.  At −24 dB: HF is nearly gone within the first  */ \
    /* few passes.                                                           */ \
    X(dmp_hb,      "dmp_hb",           -7.f,  -48.f,    0.f,  kLin,   "dB",   "Dark"               ) \
                                                                               \
    /* dmp_lf  [pitch 24 – 84, pitch curve]                                 */ \
    /* Corner frequency of the in-loop low shelf.                            */ \
    /* pitch_to_hz(24) ≈ 33 Hz,  pitch_to_hz(84) ≈ 1047 Hz.               */ \
    /* Higher values push the corner into the midrange, thinning more bass   */ \
    /* on each pass.                                                         */ \
    X(dmp_lf,      "dmp_lf",           60.f,   12.f,   96.f,  kPitch, "pitch","Damp LF corner"     ) \
                                                                               \
    /* dmp_lb  [−24 – 0 dB, linear]                                         */ \
    /* Gain of the in-loop low shelf.  "Thin" — reduces bass accumulation.   */ \
    /* At 0 dB (default): bass circulates freely; the tail can become thick  */ \
    /* or muddy on bass-heavy material.  −6 to −12 dB clears low-end buildup */ \
    /* without changing the mid/high character.  Essential on full mixes or  */ \
    /* bass instruments.  Below −18 dB the tail sounds noticeably thin.      */ \
    X(dmp_lb,      "dmp_lb",            0.f,  -48.f,    0.f,  kLin,   "dB",   "Thin"               ) \
                                                                               \
    /* ── OUTPUT SHELF (eo_*) ───────────────────────────────────────────── */ \
    /* Applied to the wet signal after the loop, before the D/W crossfade.   */ \
    /* Outside the feedback path — no stability constraint, boost is allowed. */ \
    /* Does not affect decay time, only the tonal colour of the wet output.  */ \
                                                                               \
    /* eo_hf  [pitch 60 – 120, pitch curve]                                 */ \
    /* Corner of the output high shelf.  Same pitch-to-Hz as dmp_hf.         */ \
    /* Setting this higher than dmp_hf lets you boost only the very top      */ \
    /* ("air") without affecting the main body of the reverb.                */ \
    X(eo_hf,       "eo_hf",           100.f,   24.f,  127.f,  kPitch, "pitch","Out HF corner"      ) \
                                                                               \
    /* eo_hb  [−18 – +12 dB, linear]                                        */ \
    /* Gain of the output high shelf.  Positive: adds air or sparkle —       */ \
    /* restores brightness the in-loop damping removed, or intentionally      */ \
    /* brightens a dark room.  Negative: further dulls the wet signal.       */ \
    /* Because this is outside the loop, boosting here is safe.              */ \
    X(eo_hb,       "eo_hb",            -3.f,  -24.f,   24.f,  kLin,   "dB",   "Out HF gain"        ) \
                                                                               \
    /* eo_lf  [pitch 24 – 84, pitch curve]                                  */ \
    /* Corner of the output low shelf.  Same pitch-to-Hz as dmp_lf.          */ \
    X(eo_lf,       "eo_lf",            65.f,   12.f,   96.f,  kPitch, "pitch","Out LF corner"      ) \
                                                                               \
    /* eo_lb  [−18 – +12 dB, linear]                                        */ \
    /* Gain of the output low shelf.  Default +3 dB compensates for the bass */ \
    /* dmp_lb removes from the loop — restores some low-end body to the wet  */ \
    /* signal without the muddiness of free bass recirculation.              */ \
    /* Cut to thin the reverb; boost to add warmth or weight.               */ \
    X(eo_lb,       "eo_lb",             3.f,  -24.f,   24.f,  kLin,   "dB",   "Out LF gain"        ) \
                                                                               \
    /* ── CROSSFADES ────────────────────────────────────────────────────── */ \
    /* Both use equal-power (cos/sin) mixing.  Linear mixing creates a 3 dB  */ \
    /* hole at the midpoint when the two signals are decorrelated, as they    */ \
    /* always are here.                                                      */ \
                                                                               \
    /* el_mix  [0 – 1, linear]                                              */ \
    /* Blend between early section and late loop output.                     */ \
    /* 0: early only — discrete taps, transient detail, no sustained tail.   */ \
    /* 1: late only — smooth, sustained tail, no initial echo structure.     */ \
    /* Early shapes perceived room geometry; late provides sustain.          */ \
    /* Default 0.85 (mostly late) gives a large reverb that still sounds     */ \
    /* like a space.  Lower (0.4–0.6): more room character.  Higher:         */ \
    /* more ambient texture, less defined geometry.                          */ \
    X(el_mix,      "el_mix",            0.85f,  0.f,    1.f,  kLin,   "",     "Early/Late"         ) \
                                                                               \
    /* dw_mix  [0 – 1, linear]                                              */ \
    /* Dry/wet blend.  0: dry only.  1: wet only — correct for send/return.  */ \
    /* For insert use, blend to taste.  Equal-power crossfade means the dry  */ \
    /* signal does not change level as the wet is faded in.                  */ \
    X(dw_mix,      "dw_mix",            1.f,    0.f,    1.f,  kLin,   "",     "Dry/Wet"            )

// ── generated: the struct ────────────────────────────────────────────────────

struct Params {
#define X(id, key, def, lo, hi, curve, unit, label) float id = def;
    VERB_PARAM_LIST(X)
#undef X
};

// ── generated: the descriptor table ──────────────────────────────────────────

struct ParamDesc {
    const char* key;
    float       def, lo, hi;
    ParamCurve  curve;
    const char* unit;
    const char* label;
};

enum ParamId {
#define X(id, key, def, lo, hi, curve, unit, label) kP_##id,
    VERB_PARAM_LIST(X)
#undef X
    kParamCount
};

inline const ParamDesc* ParamTable() {
    static const ParamDesc t[kParamCount] = {
#define X(id, key, def, lo, hi, curve, unit, label) { key, def, lo, hi, curve, unit, label },
        VERB_PARAM_LIST(X)
#undef X
    };
    return t;
}

// Pointer-to-member access so generic code can read/write by index.
inline float* ParamSlot(Params& p, int i) {
    float* base[kParamCount] = {
#define X(id, key, def, lo, hi, curve, unit, label) &p.id,
        VERB_PARAM_LIST(X)
#undef X
    };
    return base[i];
}

// ── range clamping ───────────────────────────────────────────────────────────
// Call once after loading params from any source (file, knobs, presets).
// Returns the number of values that were out of range.

inline int ParamsClamp(Params& p) {
    const ParamDesc* t = ParamTable();
    int n_clamped = 0;
    for (int i = 0; i < kParamCount; ++i) {
        float* v = ParamSlot(p, i);
        float  c = clampf(*v, t[i].lo, t[i].hi);
        if (c != *v) { *v = c; ++n_clamped; }
    }
    return n_clamped;
}

// ── normalised knob mapping ──────────────────────────────────────────────────
// x in [0,1] → parameter value, honouring the curve. This is the function the
// Daisy firmware calls on every ADC read, and it is the reason the curve lives
// in the table rather than being buried in the knob-reading code.

inline float ParamMapNorm(int i, float x) {
    const ParamDesc& d = ParamTable()[i];
    x = clampf(x, 0.f, 1.f);
    switch (d.curve) {
        case kExp: return d.lo * powf(d.hi / d.lo, x);
        case kSq:  return d.lo + (d.hi - d.lo) * x * x;
        case kLin:
        case kPitch:
        default:   return d.lo + (d.hi - d.lo) * x;
    }
}

// Inverse, for displaying a knob position from a stored value.
inline float ParamNormOf(int i, float v) {
    const ParamDesc& d = ParamTable()[i];
    v = clampf(v, d.lo, d.hi);
    switch (d.curve) {
        case kExp: return logf(v / d.lo) / logf(d.hi / d.lo);
        case kSq:  return sqrtf((v - d.lo) / (d.hi - d.lo));
        case kLin:
        case kPitch:
        default:   return (v - d.lo) / (d.hi - d.lo);
    }
}
