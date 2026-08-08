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
// X(id, key, default, lo, hi, curve, unit, label, hint)
//
// hint   one-line plain-English description of what the parameter does to the
//        sound; displayed in the TUI while the parameter is selected.
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
    /* ── SIZE ───────────────────────────────────────────────────────────── */ \
    /* Single macro parameter that derives predelay, early size/sym/dffs,    */ \
    /* late size, RT60, injection diffusion, and modulation rate/depth.      */ \
    /* Call Params::Derive() after changing this field.                      */ \
    /* sqrt curves give perceptually even control; zones:                    */ \
    /*   0.00–0.05: slap/chamber  0.05–0.2: room/hall                       */ \
    /*   0.2–0.6: large/cathedral  0.6–1.0: vast/infinite drone             */ \
    X(size,        "size",             0.033f,  0.f,    1.f,  kLin,   "",     "Size",               "Room size — slap echo to infinite drone; all geometry params follow; call Derive() after change"   ) \
                                                                               \
    /* ── PREDELAY ──────────────────────────────────────────────────────── */ \
                                                                               \
    /* pd_sym  [0 – 25 ms, squared]                                         */ \
    /* Offsets L and R predelay times in opposite directions:                */ \
    /*   T_L = predelay_ms − pd_sym,  T_R = predelay_ms + pd_sym            */ \
    /* Real rooms create stereo width for a centre source primarily through  */ \
    /* inter-channel arrival-time differences; this replicates that cheaply  */ \
    /* and statically, before any diffusion.  Squared mapping keeps the      */ \
    /* useful 0–5 ms range across the bottom half of the knob.  Above ~8 ms  */ \
    /* the channels start to feel like two independent reverbs.              */ \
    X(pd_sym,      "pd_sym",            0.3f,   0.f,  100.f,  kSq,    "ms",   "Pre-delay spread",   "Offsets L/R arrival times for stereo width; above ~8 ms feels like two separate rooms"            ) \
                                                                               \
    /* ── EARLY / LATE GEOMETRY (derived from size) ──────────────────────── */ \
    /* er_sz, er_sym, er_dffs, ij_dffs, lt_sz are computed by Params::Derive.*/ \
                                                                               \
    /* ── LATE SECTION ──────────────────────────────────────────────────── */ \
    /* Cross-coupled stereo feedback loop: three rotation-diffuser stages,   */ \
    /* a chain shelf, two long delay lines, and an 8-phase LFO on every tap.*/ \
    /* This is the recirculating core that builds and sustains the tail.     */ \
                                                                               \
    /* lt_sym  [0 – 8 semitones, linear]                                    */ \
    /* SzL / SzR detune for the late loop.  The two cross-coupled channels  */ \
    /* run at slightly different loop lengths, offsetting their resonant     */ \
    /* frequencies so neither rings in sympathy with the other.  Result:     */ \
    /* a wide, tonally neutral late tail.  1–2 semi is usually enough;       */ \
    /* large values make the two channels sound tonally dissimilar.          */ \
    X(lt_sym,      "lt_sym",            1.2f,   0.f,   12.f,  kLin,   "semi", "Late width",         "L/R loop offset — prevents sympathetic ringing between channels; wide stereo tail"               ) \
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
    X(lt_theta,    "lt_theta",          0.7853982f, 0.f, 0.7853982f, kLin, "rad", "Late scatter",   "Rotation mixing — pi/4 = max density/diffusion; lower = more echo-y, clearer stereo separation"   ) \
                                                                               \
    /* ── DECAY ─────────────────────────────────────────────────────────── */ \
                                                                               \
    /* rt60_s is derived from size by Params::Derive().                      */ \
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
    X(bloom,       "bloom",             3.14f,  0.25f,  12.f,  kLin,   "x",    "Bloom",              "Swell multiplier — tail builds before fading (SMD character); reduce if tail sounds unstable"      ) \
                                                                               \
    /* ── MODULATION ────────────────────────────────────────────────────── */ \
    /* mod_hz and mod_ms are derived from size by Params::Derive().         */ \
                                                                               \
    /* ── IN-LOOP SHELVES (dmp_*) ───────────────────────────────────────── */ \
    /* Two-band shelf inside the feedback loop: one per rotation-diffuser    */ \
    /* stage (×3) plus one chain shelf = four filter passes per round-trip.  */ \
    /* Gains are hard-capped at 0 dB.  Shelf<true>::SetParams asserts this.  */ \
    /*                                                                       */ \
    /* Corners are baked: HP = pitch 108 (~4186 Hz), LP = pitch 36 (~65 Hz) */ \
    /* Single EQ knob drives both gains with asymmetric scaling:             */ \
    /*   hb_dB = clamp(eq × 18,    −48, 0)   EQ<0 = dark  (HF cut)         */ \
    /*   lb_dB = clamp(eq × −25.92, −48, 0)  EQ>0 = thin  (LF cut)         */ \
    /* Asymmetric multipliers are perceptual: LF needs more cut for the      */ \
    /* same perceived tonal shift.  At EQ=0 both shelves are flat.          */ \
                                                                               \
    X(dmp_eq,      "dmp_eq",             0.f,   -1.f,    1.f,  kLin,   "",     "EQ",                 "In-loop tonal balance: − = dark (HF fades faster), + = thin (LF fades faster), 0 = flat"       ) \
                                                                               \
    /* ── OUTPUT SHELF (eo_*) ───────────────────────────────────────────── */ \
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
    X(el_mix,      "el_mix",            0.85f,  0.f,    1.f,  kLin,   "",     "Early/Late",         "Early/late blend — lower = more room geometry; higher = smooth ambient wash with less structure"  ) \
                                                                               \
    /* dw_mix  [0 – 1, linear]                                              */ \
    /* Dry/wet blend.  0: dry only.  1: wet only — correct for send/return.  */ \
    /* For insert use, blend to taste.  Equal-power crossfade means the dry  */ \
    /* signal does not change level as the wet is faded in.                  */ \
    X(dw_mix,      "dw_mix",            1.f,    0.f,    1.f,  kLin,   "",     "Dry/Wet",            "Dry/wet blend — 1 = full wet for send/return; blend for insert use (equal-power crossfade)"     )

// ── generated: the struct ────────────────────────────────────────────────────

struct Params {
#define X(id, key, def, lo, hi, curve, unit, label, hint) float id = def;
    VERB_PARAM_LIST(X)
#undef X

    // Computed from size — do not set directly; call Derive() after changing size.
    float predelay_ms = 40.f;
    float er_sz       = 30.f;
    float er_sym      =  1.f;
    float er_dffs     = 0.68f;
    float ij_dffs     = 0.70f;
    float lt_sz       = 24.f;
    float rt60_s      =  4.f;
    float mod_hz      =  0.7f;
    float mod_ms      =  0.6f;

    void Derive() {
        predelay_ms = sqrtf(size * 89500.f) + 2.f;
        er_sz       = 60.f - (size * 42.f);
        er_sym      = sqrtf(size * 144.f);
        er_dffs     = sqrtf(size * 0.96f);
        ij_dffs     = fminf(powf(size, 1.f / 8.f), 0.98f);
        lt_sz       = 48.f - (size * 42.f);
        rt60_s      = fmaxf(size * 120.f, 0.05f);
        mod_hz      = fmaxf(size * 20.f, 0.005f);
        mod_ms      = size * 20.f;
    }
};

// ── generated: the descriptor table ──────────────────────────────────────────

struct ParamDesc {
    const char* key;
    float       def, lo, hi;
    ParamCurve  curve;
    const char* unit;
    const char* label;
    const char* hint;
};

enum ParamId {
#define X(id, key, def, lo, hi, curve, unit, label, hint) kP_##id,
    VERB_PARAM_LIST(X)
#undef X
    kParamCount
};

inline const ParamDesc* ParamTable() {
    static const ParamDesc t[kParamCount] = {
#define X(id, key, def, lo, hi, curve, unit, label, hint) { key, def, lo, hi, curve, unit, label, hint },
        VERB_PARAM_LIST(X)
#undef X
    };
    return t;
}

// Pointer-to-member access so generic code can read/write by index.
inline float* ParamSlot(Params& p, int i) {
    float* base[kParamCount] = {
#define X(id, key, def, lo, hi, curve, unit, label, hint) &p.id,
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
