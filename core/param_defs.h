#pragma once
#include <cmath>
#include "util.h"

// ─────────────────────────────────────────────────────────────────────────────
// THE parameter list. Single source of truth.
//
// Everything that knows about parameters is generated from this one macro:
//   • the Params struct                (core/params.h)
//   • the params.txt parser            (host/main.cpp)
//   • range clamping / validation      (ParamsClamp)
//   • normalised knob mapping 0..1     (ParamMapNorm)  — stage 10
//   • --dump-params reference output   (host/main.cpp)
//
// To add a parameter: add one line here. Nothing else needs editing.
//
// X(id, key, default, lo, hi, curve, unit, label)
//
// curve controls the 0..1 → value mapping used by knobs and by any UI:
//   kLin   linear                       v = lo + x(hi-lo)
//   kExp   exponential (lo must be > 0) v = lo (hi/lo)^x     — seconds, Hz
//   kSq    squared, fine at the bottom  v = lo + (hi-lo)x²   — depths
//   kPitch linear in MIDI pitch, which is ALREADY exponential in delay time,
//          so it is stored linear but flagged so a UI can label it in Hz/ms.
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
    /* in-loop shelves — gains hard-capped at 0 dB, this is a stability bound */                      \
    X(dmp_hf,      "dmp_hf",           96.f,   60.f,  120.f,  kPitch, "pitch","Damp HF corner"     ) \
    X(dmp_hb,      "dmp_hb",           -7.f,  -24.f,    0.f,  kLin,   "dB",   "Dark"               ) \
    X(dmp_lf,      "dmp_lf",           60.f,   24.f,   84.f,  kPitch, "pitch","Damp LF corner"     ) \
    X(dmp_lb,      "dmp_lb",            0.f,  -24.f,    0.f,  kLin,   "dB",   "Thin"               ) \
                                                                                                      \
    /* output shelf — outside the loop, boost is allowed here */                                      \
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
