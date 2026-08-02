#pragma once
#include <cmath>
#include "util.h"

// ─────────────────────────────────────────────────────────────────────────────
// Parameter definitions — single source of truth.
//
// The list lives in param_list.inc, which is #included with different
// definitions of X to generate each artefact below:
//   • Params struct              — float members with defaults
//   • ParamDesc table            — key, range, curve, unit, label per param
//   • ParamId enum               — typed index for each param
//   • ParamSlot                  — index → pointer-to-member
//   • ParamsClamp                — clamp all fields to [lo, hi]
//   • ParamMapNorm / ParamNormOf — normalised 0..1 ↔ value (knob mapping)
//
// To add a parameter: one entry in param_list.inc. Nothing else.
//
// X(id, key, default, lo, hi, curve, unit, label)
//
// curve controls the 0..1 → value mapping for hardware knobs and any UI:
//   kLin    linear                       v = lo + x(hi−lo)
//   kExp    exponential (lo must be > 0) v = lo · (hi/lo)^x    — time, Hz
//   kSq     squared                      v = lo + (hi−lo)·x²   — depths
//   kPitch  linear in MIDI pitch — already exponential in delay time,
//           stored linear but flagged so a UI can label it in Hz or ms.
// ─────────────────────────────────────────────────────────────────────────────

enum ParamCurve { kLin, kExp, kSq, kPitch };

// ── Params struct ─────────────────────────────────────────────────────────────

struct Params {
#define X(id, key, def, lo, hi, curve, unit, label) float id = def;
#include "param_list.inc"
#undef X
};

// ── Descriptor table ──────────────────────────────────────────────────────────

struct ParamDesc {
    const char* key;
    float       def, lo, hi;
    ParamCurve  curve;
    const char* unit;
    const char* label;
};

enum ParamId {
#define X(id, key, def, lo, hi, curve, unit, label) kP_##id,
#include "param_list.inc"
#undef X
    kParamCount
};

inline const ParamDesc* ParamTable() {
    static const ParamDesc t[] = {
#define X(id, key, def, lo, hi, curve, unit, label) { key, def, lo, hi, curve, unit, label },
#include "param_list.inc"
#undef X
    };
    return t;
}

// ── Index → pointer-to-member ─────────────────────────────────────────────────

inline float* ParamSlot(Params& p, int i) {
    static const ptrdiff_t offsets[] = {
#define X(id, key, def, lo, hi, curve, unit, label) offsetof(Params, id),
#include "param_list.inc"
#undef X
    };
    return reinterpret_cast<float*>(reinterpret_cast<char*>(&p) + offsets[i]);
}

// ── Range clamping ────────────────────────────────────────────────────────────
// Call once after loading params from any source (file, knobs, presets).
// Returns the number of values that were out of range.

inline int ParamsClamp(Params& p) {
    const ParamDesc* t = ParamTable();
    int n = 0;
    for (int i = 0; i < kParamCount; ++i) {
        float* v = ParamSlot(p, i);
        float  c = clampf(*v, t[i].lo, t[i].hi);
        if (c != *v) { *v = c; ++n; }
    }
    return n;
}

// ── Normalised knob mapping ───────────────────────────────────────────────────
// x in [0,1] → parameter value, honouring the curve. This is what the Daisy
// firmware calls on every ADC read — the reason curve lives in the table.

inline float ParamMapNorm(int i, float x) {
    const ParamDesc& d = ParamTable()[i];
    x = clampf(x, 0.f, 1.f);
    switch (d.curve) {
        case kExp:   return d.lo * powf(d.hi / d.lo, x);
        case kSq:    return d.lo + (d.hi - d.lo) * x * x;
        case kLin:
        case kPitch:
        default:     return d.lo + (d.hi - d.lo) * x;
    }
}

// Inverse: value → normalised position, for displaying a stored value as a
// knob position.
inline float ParamNormOf(int i, float v) {
    const ParamDesc& d = ParamTable()[i];
    v = clampf(v, d.lo, d.hi);
    switch (d.curve) {
        case kExp:   return logf(v / d.lo) / logf(d.hi / d.lo);
        case kSq:    return sqrtf((v - d.lo) / (d.hi - d.lo));
        case kLin:
        case kPitch:
        default:     return (v - d.lo) / (d.hi - d.lo);
    }
}
