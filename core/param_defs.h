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
    /* Macro knob, exponential in time: rt60 = 0.3 s · 200^decay            */ \
    /*   0 → 0.3 s   0.25 → 1.1 s   0.5 → 4.2 s   0.75 → 16 s   1 → 60 s    */ \
    /* Every position now changes the tail (old map saturated above ~0.25). */ \
    /* Derive() also sets el_mix = decay (early/late balance).              */ \
    X(decay,      "decay",       0.5f,  0.f,   1.f, kLin, "",    "Decay"         ) \
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
    // (bloom is gone: it only multiplied rt60 inside the feedback gain, i.e. it
    // was a second, hidden decay-length control, not a swell.)
    float rt60_s = 4.243f;   // DecayToRt60(decay)
    float el_mix = 0.5f;     // decay

    // ── permanently baked ─────────────────────────────────────────────────────
    float predelay_ms = 18.0f;
    float pd_sym      = 5.1f;
    // Early diffusers: was er_sz 14.2 → 39–62 ms allpasses, which at g≈0.58 are
    // discrete echo generators (≈16 Hz flutter), not diffusers. Now 4.8–14 ms.
    // Stage offsets live in early.h (kEarlyOffs), found by the same ratio search.
    float er_sz       = 39.9f;
    float er_sym      = 2.37f;
    float er_dffs     = 0.60f;
    float mod_hz      = 0.87f;
    float mod_ms      = 1.0f;   // was 0.45: too shallow to smear loop modes
    float out_width   = 6.28f;
    float lt_sym   = 2.62f;   // was 2.5 → L/R 5 semis apart ≈ 4/3; see kLateOffs
    float lt_theta = 0.7853982f;
    float lt_dffs  = 0.494f;
    float chaos    = 0.94f;
    float sat      = 0.12f;
    float hp_hz    = 4186.f;  // early-section shelf only (not in a loop)
    float hp_db    = -0.1f;
    // Late-loop HF damping: HF T60 = min(rt60 · ratio, max). See Late::_update_shelves.
    // Measured octave-band T60 at rt60=16 s: 500 Hz 14 s, 2 k 8.5 s, 4 k 6 s, 8 k 4.4 s.
    float lt_hf_hz    = 2000.f;
    float lt_hf_ratio = 0.30f;
    float lt_hf_max_s = 8.0f;

    static float DecayToRt60(float d) { return 0.3f * powf(200.f, d); }

    void Derive() {
        rt60_s = DecayToRt60(decay);
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
