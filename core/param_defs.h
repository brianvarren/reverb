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
    /* ── SIZE ───────────────────────────────────────────────────────────── */ \
    /* Single macro that derives all geometry: predelay, early size,         */ \
    /* late size, RT60, mod rate/depth.  Call Params::Derive() after change. */ \
    /* Character zones:                                                      */ \
    /*   0.00–0.05: slap/chamber   0.05–0.25: room/hall                     */ \
    /*   0.25–0.6:  large/cave     0.6–1.0:   vast drone / near-infinite    */ \
    X(size,    "size",    0.033f, 0.f, 1.f, kLin, "",   "Size"          ) \
                                                                               \
    /* ── PREDELAY STEREO ────────────────────────────────────────────────── */ \
    /* pd_sym  [0 – 100 ms², squared]                                        */ \
    /* Offsets L and R predelay by ±pd_sym:                                  */ \
    /*   T_L = predelay_ms − pd_sym,  T_R = predelay_ms + pd_sym            */ \
    /* Squared mapping keeps the useful 0–5 ms range in the bottom half.     */ \
    /* Above ~8 ms the two channels start to feel like separate reverbs.     */ \
    X(pd_sym,  "pd_sym",  0.3f,  0.f, 100.f, kSq,  "ms", "Pre-delay spread") \
                                                                               \
    /* ── LATE SECTION ───────────────────────────────────────────────────── */ \
    /* lt_theta  [0 – π/4 rad, linear]                                       */ \
    /* Rotation angle in the 2×2 orthogonal diffuser before each delay read. */ \
    /* θ = 0: identity — echoes, no diffusion.                               */ \
    /* θ = π/4: max mixing — smooth, dense tail.                             */ \
    /* Lower = more articulate and echo-y; higher = more wash.               */ \
    X(lt_theta,"lt_theta",0.7853982f, 0.f, 0.7853982f, kLin, "rad", "Late scatter") \
                                                                               \
    /* ── DECAY ──────────────────────────────────────────────────────────── */ \
    /* bloom  [0.25 – 12×, linear]                                           */ \
    /* Multiplier on the effective RT60 in the fb_gain formula.              */ \
    /* At bloom > 1 the tail grows before it fades — the SMD swell.         */ \
    /* At bloom = 1: standard Schroeder decay (falls from onset).            */ \
    X(bloom,   "bloom",   3.14f, 0.25f, 12.f, kLin, "x",  "Bloom"         ) \
                                                                               \
    /* ── IN-LOOP EQ ─────────────────────────────────────────────────────── */ \
    /* Single knob driving two baked shelves on every loop pass.             */ \
    /* Corners baked: HP = pitch 108 (~4186 Hz), LP = pitch 36 (~65 Hz).    */ \
    /* Asymmetric dB scaling (Reaktor ensemble source values):               */ \
    /*   hb_dB = clamp(eq × 18,     −∞, 0)   eq < 0 → dark  (HF cut)       */ \
    /*   lb_dB = clamp(eq × −25.92, −∞, 0)   eq > 0 → thin  (LF cut)       */ \
    /* Output shelf (outside loop) uses symmetric ±36 dB, can boost.        */ \
    /* At eq = 0 all shelves are flat.                                       */ \
    X(dmp_eq,  "dmp_eq",  0.f,  -1.f,  1.f, kLin, "",   "EQ"            ) \
                                                                               \
    /* ── CROSSFADES ─────────────────────────────────────────────────────── */ \
    /* Equal-power (cos/sin) mixing — avoids the 3 dB hole that linear mix   */ \
    /* creates when the two signals are decorrelated (as they always are).   */ \
                                                                               \
    /* el_mix  [0 – 1, linear]                                               */ \
    /* 0: early only (discrete taps, transient detail, no tail).             */ \
    /* 1: late only (smooth sustained tail, no echo structure).              */ \
    X(el_mix,  "el_mix",  0.55f, 0.f,  1.f, kLin, "",   "Early/Late"    ) \
                                                                               \
    /* dw_mix  [0 – 1, linear]                                               */ \
    /* 0: dry only.  1: full wet (send/return use).                          */ \
    X(dw_mix,  "dw_mix",  0.5f,  0.f,  1.f, kLin, "",   "Dry/Wet"       )

// ── generated: the struct ────────────────────────────────────────────────────

struct Params {
#define X(id, key, def, lo, hi, curve, unit, label) float id = def;
    VERB_PARAM_LIST(X)
#undef X

    // Derived from size via original Reaktor formulas (S = size × 100).
    // Do not set directly; call Derive() after changing size.
    float predelay_ms = 10.1f;   // (sqrt(S × 2e-5) + 0.002) × 1000   @ size=0.033
    float er_sz       = 58.6f;   // 60 − S × 0.42
    float er_sym      =  0.10f;  // sqrt(S × 0.003)  — stereo spread in semitones
    float er_dffs     =  0.13f;  // sqrt(S × 0.005)  — allpass coefficient
    float lt_sz       = 45.6f;   // 48 − S × 0.72
    float rt60_s      =  3.3f;   // S  (seconds)
    float mod_hz      =  0.03f;  // S × 0.01  (Hz)
    float mod_ms      =  0.03f;  // S × 0.01  (ms)

    // Baked stereo symmetry constant — not derived.
    float lt_sym = 2.5f;  // ±2.5 semi between late L and R loop

    void Derive() {
        const float S = size * 100.f;
        predelay_ms = (sqrtf(S * 2e-05f) + 0.002f) * 1000.f;
        er_sz       = 60.f  - S * 0.42f;
        er_sym      = sqrtf(S * 0.003f);
        er_dffs     = sqrtf(S * 0.005f);
        lt_sz       = 48.f  - S * 0.72f;
        rt60_s      = fmaxf(S, 0.05f);
        mod_hz      = fmaxf(S * 0.01f, 0.005f);
        mod_ms      = S * 0.01f;
    }
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
// Daisy firmware calls on every ADC read.

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
