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
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Parameter reference
// ─────────────────────────────────────────────────────────────────────────────
//
// ── PREDELAY ──────────────────────────────────────────────────────────────────
//
// predelay_ms  [0 – 170 ms, linear]
//   A fixed delay inserted before both channels enter the diffusion network.
//   The gap between the direct sound and the first reflected energy is the
//   primary cue the auditory system uses to judge source distance and room
//   size: longer predelay reads as a larger, more distant space.  Below ~15 ms
//   the reverb merges with the source and the onset is lost.  Above ~60 ms it
//   separates into a distinct audible event — useful for vintage tape-delay
//   character, disorienting at long decay times.  The hard ceiling is 170 ms,
//   set by the predelay buffer size.
//
// pd_sym  [0 – 25 ms, squared]
//   Widens the stereo image by offsetting the two channels' predelay times in
//   opposite directions:  T_L = predelay_ms − pd_sym,  T_R = predelay_ms + pd_sym.
//   A real room produces stereo width for a centre source primarily through
//   inter-channel arrival-time differences; this replicates that cheaply and
//   statically, before any other processing.  A squared mapping keeps the
//   perceptually useful 0–5 ms range across the bottom half of the knob.
//   Values above ~8 ms push from "width" toward "ping-pong" — the channels
//   start to sound like two independent reverbs.
//
//
// ── EARLY SECTION ─────────────────────────────────────────────────────────────
//
//   Architecture: three Schroeder allpasses in series, per channel, followed by
//   a two-band shelf.  No cross-feed between L and R.  No feedback.  Stereo
//   width comes entirely from the SzL / SzR asymmetry controlled by er_sym.
//
// er_sz  [pitch 18 – 48, pitch curve]
//   Base delay length, in MIDI pitch units, for the early allpass chain.
//   Pitch → period: N(p) = fs / (440 · 2^((p−69)/12)).  The three allpasses
//   are placed at offsets +8, +4, and 0 semitones above er_sz, giving length
//   ratios 2^(8/12) : 2^(4/12) : 1 ≈ 1.587 : 1.260 : 1.  These are irrational,
//   so the echo trains from the three stages never realign at any er_sz — a
//   property that would be lost if the sizes were in small integer ratios.
//   Pitch 30 (default) → 1038 samples ≈ 21.6 ms at 48 kHz.
//   Sonically: smaller values produce tight, close-sounding early reflections
//   that read as a small or nearby space.  Larger values spread the early
//   echoes further apart, sounding like a large hall or cave.  The early
//   section shapes the first 50–100 ms of the reverb tail — the part that
//   most directly characterises the perceived room geometry.
//
// er_sym  [0 – 6 semitones, linear]
//   Detunes the L and R early chains by ±er_sym semitones:
//     SzL = er_sz − er_sym,   SzR = er_sz + er_sym.
//   Because sizes are pitches, ±N semitones scales the two channels by
//   reciprocal factors 2^(∓N/12) — the left chain is proportionally shorter,
//   the right proportionally longer, at any er_sz.  Even 0.5 semitones (~3%)
//   is enough to fully decorrelate the two channels' reflection patterns.
//   Keep below ~4 semitones; above that the channels start to sound like
//   different-sized spaces rather than one wide one.
//
// er_dffs  [0.45 – 0.80, linear]
//   The allpass coefficient g.  In the Schroeder structure:
//     w[n] = x[n] + g · shelf(w[n−M])
//     y[n] = shelf(w[n−M]) − g · w[n]
//   At g = 0 the allpass degenerates to a pure delay — you hear three discrete
//   echoes.  At g = √2/2 ≈ 0.707 the magnitude response is maximally flat
//   (all-pass in the strict frequency-domain sense) and the energy is spread
//   most evenly across time.  Approaching g → 1 the early tail grows denser
//   but begins to buzz: the feedback inside each allpass starts to resonate.
//   For a cavernous character, 0.65–0.75 is the right range — dense enough
//   that discrete echoes are gone, low enough to avoid harshness.
//
//
// ── LATE SECTION ──────────────────────────────────────────────────────────────
//
//   Architecture: cross-coupled stereo feedback loop containing three rotation-
//   diffuser stages, a chain shelf, and two long delay lines, plus an 8-phase
//   LFO modulating every tap.  This is the recirculating core that builds and
//   sustains the reverb tail.
//
// lt_sz  [pitch 12 – 42, pitch curve]
//   Base size for the late loop, same pitch-domain scheme as er_sz.  Four
//   delays per channel at pitches Sz+9, Sz+6, Sz+3, Sz, giving ratios
//   2^(9/12) : 2^(6/12) : 2^(3/12) : 1 ≈ 1.682 : 1.414 : 1.189 : 1.
//   At the default pitch 24: loop total ≈ 96 ms per channel.
//   This is the primary room-size control.  Smaller values produce a tighter,
//   more resonant late tail with audible modal colouration — individual
//   frequencies ring before fading.  Larger values space the delays further
//   apart, increasing echo density and reducing colouration, at the cost of
//   a more diffuse, "washy" character.  The two effects happen because a longer
//   loop takes more round-trips to reach a given echo density, while also
//   having more delay between resonances.
//
// lt_sym  [0 – 8 semitones, linear]
//   Same SzL / SzR trick as er_sym, applied to the late loop.  The two
//   cross-coupled feedback channels run at slightly different loop lengths,
//   so their resonant frequencies are offset — one channel never rings in
//   sympathy with the other, producing a wide, non-coloured late tail.
//   A small value (1–2 semitones) is usually sufficient; large values can
//   make the two channels sound tonally different from each other.
//
// lt_theta  [0 – π/4 radians, linear]
//   The rotation angle in the 2×2 orthogonal diffuser matrix placed before
//   each pair of delay reads:
//     [a', b'] = [a·cosθ − b·sinθ,  a·sinθ + b·cosθ]
//   At θ = 0: a' = a, b' = b — the matrix is the identity; the two rails are
//   completely decoupled.  At θ = π/4 (default): a' = (a−b)/√2, b' = (a+b)/√2
//   — maximum mixing, each output is an equal blend of both rails.
//   The matrix is orthogonal (RᵀR = I), so ‖Ra‖ = ‖a‖ at every sample; it
//   redistributes energy without creating or destroying any, making it safe
//   under recirculation even at high feedback gains.
//   Sonically: low angles produce a looser, more echo-y tail with audible
//   stereo separation between the two rails.  Higher angles scramble echoes
//   into density faster — the tail becomes smooth and diffuse more quickly.
//   π/4 gives the fastest build-up; reduce it for a more articulate tail.
//
//
// ── DECAY ─────────────────────────────────────────────────────────────────────
//
// rt60_s  [0.2 – 30 s, exponential]
//   Target reverberation time, defined as the time for the reverb tail to
//   decay by 60 dB.  The feedback gain is derived from it as:
//     fb_gain = 10^(−3 · loop_length_s / (rt60_s · bloom))
//   clamped to 0.995.  Exponential knob curve because decay time is
//   perceptually logarithmic — the difference between 0.5 s and 1.0 s sounds
//   the same as the difference between 2 s and 4 s.
//   Note: the in-loop shelves absorb high-frequency energy on every pass, so
//   measured T60 at high frequencies is substantially shorter than rt60_s
//   predicts.  rt60_s targets the mid-frequency decay.  This is normal
//   reverberator behaviour — real rooms have frequency-dependent decay too.
//
// bloom  [1 – 6×, linear]
//   Multiplier folded into the RT60 formula:
//     effective T60 = rt60_s · bloom
//   so fb_gain is computed for a longer target than rt60_s alone specifies.
//   The result is that at high fb_gain the tail initially grows in apparent
//   level before it decays — an audible "bloom" or swelling effect that is
//   the characteristic sound of the Space Master Deluxe.  At bloom = 1 the
//   formula is the standard Schroeder/Moorer result and the tail falls
//   immediately.  At the default (π ≈ 3.14), fb_gain is set to sustain
//   a decay roughly three times longer than the shelves can actually support,
//   producing a rich, late-building tail rather than an immediate fade.
//   Bloom interacts strongly with rt60_s and the dmp_hb/dmp_lb shelf gains —
//   reduce bloom if the tail sounds unstable or overly swollen.
//
//
// ── MODULATION ────────────────────────────────────────────────────────────────
//
// mod_ms  [0 – 6 ms, squared]
//   Peak amplitude of the sinusoidal pitch modulation applied to all eight
//   delay taps (four per channel: three diffuser stages + one long delay).
//   Each tap is individually modulated:
//     tap_offset[k] = mod_depth_samples · sin(2π · (phase + k/8))
//   so the eight modulators are equally spaced in phase — they never all peak
//   simultaneously.  Modulation breaks the late loop's resonant modes: without
//   it, a long decay at high feedback gain produces audible pitched artefacts
//   as individual loop frequencies sustain while others fade.  Small values
//   (0.3–1 ms) are inaudible as pitch but sufficient to smooth spectral density.
//   Values above ~2 ms introduce audible shimmer or chorus; above 4 ms the
//   modulation itself becomes the dominant timbral character.  A squared
//   mapping keeps the useful 0.2–1.5 ms zone across the bottom half of the
//   knob.
//
// mod_hz  [0.02 – 5 Hz, exponential]
//   Rate of the LFO driving all eight modulators.  The equally-spaced phases
//   ensure there is always 8-fold coverage of the modulation cycle regardless
//   of rate.  Very slow rates (< 0.2 Hz) produce a barely-perceptible,
//   breathing quality — the reverb tail slowly pulses in density.  The default
//   (0.7 Hz) is below the threshold of obvious chorus but fast enough to
//   prevent spectral freezing on long decays.  Rates above 2 Hz produce
//   obvious pitch vibrato in the tail; above 4 Hz it becomes a thick flutter.
//   Exponential mapping because useful rates span two decades.
//
//
// ── IN-LOOP SHELVES (dmp_*) ───────────────────────────────────────────────────
//
//   These two-band shelves live inside the feedback loop: one per rotation
//   diffuser stage (× 3) plus one chain shelf, giving four filter passes per
//   loop round-trip per channel.  Because they are inside the loop, both gains
//   are hard-capped at 0 dB (linear ≤ 1.0).  Any gain above unity would push
//   the loop gain above fb_gain, which could cause instability regardless of
//   the RT60 formula.  The assertion in Shelf<true>::SetParams enforces this
//   structurally.
//
// dmp_hf  [pitch 60 – 120, pitch curve]
//   Corner frequency of the in-loop high shelf, in MIDI pitch units.
//   pitch_to_hz(60) ≈ 261 Hz, pitch_to_hz(120) ≈ 8372 Hz.
//   This controls where high-frequency roll-off begins.  Lower values start
//   the roll-off earlier (darker, warmer), higher values let more high
//   content into the loop (brighter).  The corner is the −3 dB point of a
//   one-pole lowpass that shapes the "air" content of the tail.
//
// dmp_hb  [−24 – 0 dB, linear]
//   Gain of the in-loop high shelf.  The primary tonal character control —
//   think of it as "Dark".  At 0 dB (flat) the reverb is full-bandwidth;
//   highs decay at the same rate as mids.  At −7 dB (default), each loop
//   pass attenuates the high band by 7 dB, so over N passes the high content
//   has decayed N × 7 dB more than mid content.  This is what makes a reverb
//   sound "dark" or "warm" — highs fade first, leaving a progressively fuller,
//   lower-frequency tail.  At −24 dB, high-frequency content is nearly
//   eliminated within the first few loop passes.
//
// dmp_lf  [pitch 24 – 84, pitch curve]
//   Corner frequency of the in-loop low shelf.
//   pitch_to_hz(24) ≈ 33 Hz, pitch_to_hz(84) ≈ 1047 Hz.
//   Controls where low-frequency attenuation begins.  Higher values push the
//   corner into the midrange, thinning out more of the bass on each pass.
//
// dmp_lb  [−24 – 0 dB, linear]
//   Gain of the in-loop low shelf.  "Thin" — reduces bass accumulation in the
//   tail.  At 0 dB (default) bass circulates freely; the tail can become thick
//   or muddy on bass-heavy material.  Cutting here (−6 to −12 dB) clears low-
//   end buildup without changing the mid/high character.  Essential when the
//   reverb is used on full mixes or bass instruments.  Above ~−3 dB the effect
//   is subtle; below −18 dB the tail becomes noticeably thin even on dry
//   material.
//
//
// ── OUTPUT SHELF (eo_*) ───────────────────────────────────────────────────────
//
//   A two-band shelf applied to the wet signal AFTER the loop and BEFORE the
//   dry/wet crossfade.  Because it is outside the feedback path there is no
//   stability constraint — boost is allowed.  It does not affect decay time,
//   only the tonal colour of the wet output.
//
// eo_hf  [pitch 60 – 120, pitch curve]
//   Corner frequency of the output high shelf.  Same pitch-to-Hz conversion as
//   dmp_hf.  Set higher than dmp_hf to boost only the very top of the spectrum
//   ("air") without affecting the main body of the reverb.
//
// eo_hb  [−18 – +12 dB, linear]
//   Gain of the output high shelf.  Positive values add "air" or sparkle to
//   the wet signal — useful to restore brightness that the in-loop damping
//   removed, or to intentionally brighten a dark room character.  Negative
//   values further dull the reverb output, useful for sub-mixing under a lead.
//   Because this is outside the loop, boosting here does not risk instability.
//
// eo_lf  [pitch 24 – 84, pitch curve]
//   Corner frequency of the output low shelf.
//
// eo_lb  [−18 – +12 dB, linear]
//   Gain of the output low shelf.  A small positive value (default +3 dB)
//   compensates for the bass that dmp_lb removes from the loop, restoring some
//   low-end body to the wet signal without the muddiness of unattenuated bass
//   recirculation.  Cutting here thins the overall reverb; boosting adds warmth
//   or weight.
//
//
// ── CROSSFADES ────────────────────────────────────────────────────────────────
//
//   Both crossfades use equal-power (cos/sin) mixing.  Linear mixing would
//   produce a 3 dB level dip at the midpoint when the two signals are
//   decorrelated — which they always are here.
//
// el_mix  [0 – 1, linear]
//   Blend between the early section output and the late loop output.
//   At 0: only early — discrete taps, strong transient detail, no sustained
//   tail.  At 1: only late — smooth, sustained tail with no initial echo
//   structure.  The early section shapes perceived room character; the late
//   section provides sustain and density.  The default (0.85, mostly late)
//   gives a large reverb that still has enough early detail to sound like a
//   space rather than pure wash.  Lower values (0.4–0.6) emphasise room
//   geometry; higher values approach a pure ambient texture.
//
// dw_mix  [0 – 1, linear]
//   Dry/wet blend.  At 0: dry signal only, no reverb.  At 1: wet only,
//   no dry — the correct setting for a send/return configuration.  For insert
//   use on a single instrument, blend to taste.  Because the crossfade is
//   equal-power, the dry signal does not change level as the wet is faded in.
// ─────────────────────────────────────────────────────────────────────────────

enum ParamCurve { kLin, kExp, kSq, kPitch };

#define VERB_PARAM_LIST(X)                                                                            \
    /*   id            key              def      lo      hi    curve   unit    label            */    \
    X(predelay_ms, "predelay_ms",      40.f,    0.f,  170.f,  kLin,   "ms",   "Pre-delay"          ) \
    X(pd_sym,      "pd_sym",            0.3f,   0.f,   25.f,  kSq,    "ms",   "Pre-delay spread"   ) \
                                                                                                      \
    X(er_sz,       "er_sz",            30.f,   18.f,   48.f,  kPitch, "pitch","Early size"         ) \
    X(er_sym,      "er_sym",            1.f,    0.f,    6.f,  kLin,   "semi", "Early width"        ) \
    X(er_dffs,     "er_dffs",           0.68f,  0.45f,  0.80f,kLin,   "",     "Early diffusion"    ) \
                                                                                                      \
    X(lt_sz,       "lt_sz",            24.f,   12.f,   42.f,  kPitch, "pitch","Late size"          ) \
    X(lt_sym,      "lt_sym",            1.2f,   0.f,    8.f,  kLin,   "semi", "Late width"         ) \
    X(lt_theta,    "lt_theta",          0.7853982f, 0.f, 0.7853982f, kLin, "rad", "Late scatter"   ) \
                                                                                                      \
    X(rt60_s,      "rt60_s",            4.f,    0.2f,  30.f,  kExp,   "s",    "Decay"              ) \
    X(bloom,       "bloom",             3.14f,  1.f,    6.f,  kLin,   "x",    "Bloom"              ) \
                                                                                                      \
    X(mod_ms,      "mod_ms",            0.6f,   0.f,    6.f,  kSq,    "ms",   "Motion depth"       ) \
    X(mod_hz,      "mod_hz",            0.7f,   0.02f,  5.f,  kExp,   "Hz",   "Motion rate"        ) \
                                                                                                      \
    /* in-loop shelves — gains hard-capped at 0 dB, stability requirement */                          \
    X(dmp_hf,      "dmp_hf",           96.f,   60.f,  120.f,  kPitch, "pitch","Damp HF corner"     ) \
    X(dmp_hb,      "dmp_hb",           -7.f,  -24.f,    0.f,  kLin,   "dB",   "Dark"               ) \
    X(dmp_lf,      "dmp_lf",           60.f,   24.f,   84.f,  kPitch, "pitch","Damp LF corner"     ) \
    X(dmp_lb,      "dmp_lb",            0.f,  -24.f,    0.f,  kLin,   "dB",   "Thin"               ) \
                                                                                                      \
    /* output shelf — outside the loop, boost allowed */                                              \
    X(eo_hf,       "eo_hf",           100.f,   60.f,  120.f,  kPitch, "pitch","Out HF corner"      ) \
    X(eo_hb,       "eo_hb",            -3.f,  -18.f,   12.f,  kLin,   "dB",   "Out HF gain"        ) \
    X(eo_lf,       "eo_lf",            65.f,   24.f,   84.f,  kPitch, "pitch","Out LF corner"      ) \
    X(eo_lb,       "eo_lb",             3.f,  -18.f,   12.f,  kLin,   "dB",   "Out LF gain"        ) \
                                                                                                      \
    X(el_mix,      "el_mix",            0.85f,  0.f,    1.f,  kLin,   "",     "Early/Late"         ) \
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
