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
    /* ── LATE ───────────────────────────────────────────────────────────── */ \
    /* lt_sz  [−24 – 48 semi, linear in pitch]                               */ \
    /* Base pitch of the late loop delay lines.  Lower pitch = longer delays.*/ \
    /* 24 → ~96 ms loop.  6 → ~350 ms loop.  −24 → ~2 s loop.              */ \
    X(lt_sz,      "lt_sz",      24.0f,-24.f,  48.f, kLin, "semi", "Size"          ) \
                                                                               \
    /* ── DECAY ──────────────────────────────────────────────────────────── */ \
    /* Macro knob: 0=short (rt60=0.05s, bloom=0.25, el_mix=0),              */ \
    /*             1=long  (rt60=120s,  bloom=12,   el_mix=1).              */ \
    /* Derive() linearly interpolates rt60_s, bloom, el_mix.                */ \
    X(decay,      "decay",       0.3f,  0.f,   1.f, kLin, "",    "Decay"         ) \
                                                                               \
    /* ── OUTPUT FILTER ──────────────────────────────────────────────────── */ \
    /* Four-pole Moog ladder LP + first-order HP → parametric bandpass.      */ \
    /* LP cutoff = out_freq × 2^(out_width/2)    out_width baked at 6.28    */ \
    X(out_freq,  "out_freq",  1000.f,   80.f,18000.f, kExp, "Hz",  "Tone"       ) \
                                                                               \
    /* ── MIX ────────────────────────────────────────────────────────────── */ \
    /* 0 = dry only, 1 = full wet (send/return use).                         */ \
    X(dw_mix,     "dw_mix",      0.5f,  0.f,   1.f, kLin, "",    "Mix"           )

// ── generated: the struct ────────────────────────────────────────────────────

struct Params {
#define X(id, key, def, lo, hi, curve, unit, label) float id = def;
    VERB_PARAM_LIST(X)
#undef X

    // ── derived from decay ────────────────────────────────────────────────────
    float rt60_s = 36.035f;  // 0.05 + decay * 119.95
    float bloom  = 3.775f;   // 0.25 + decay * 11.75
    float el_mix = 0.3f;     // decay

    // ── permanently baked ─────────────────────────────────────────────────────
    float predelay_ms = 18.0f;
    float pd_sym      = 5.1f;
    float er_sz       = 14.2f;
    float er_sym      = 2.42f;
    float er_dffs     = 0.576f;
    float mod_hz      = 0.87f;
    float mod_ms      = 0.45f;
    float out_width   = 6.28f;
    float lt_sym   = 2.5f;
    float lt_theta = 0.7853982f;
    float lt_dffs  = 0.494f;
    float chaos    = 0.94f;
    float sat      = 0.12f;
    float hp_hz    = 4186.f;
    float hp_db    = -0.1f;

    void Derive() {
        rt60_s = 0.05f + decay * 119.95f;
        bloom  = 0.25f + decay * 11.75f;
        el_mix = decay;
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
