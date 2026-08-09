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
    /* ── PREDELAY ───────────────────────────────────────────────────────── */ \
    /* predelay_ms  [0.5 – 300 ms, exponential]                              */ \
    X(predelay_ms,"predelay_ms", 10.f,  0.5f, 300.f, kExp, "ms", "Pre-delay"      ) \
                                                                               \
    /* pd_sym  [0 – 100 ms², squared]                                        */ \
    /* Offsets L and R predelay by ±pd_sym:                                  */ \
    /*   T_L = predelay_ms − pd_sym,  T_R = predelay_ms + pd_sym            */ \
    /* Squared mapping keeps the useful 0–5 ms range in the bottom half.     */ \
    X(pd_sym,     "pd_sym",      0.3f,  0.f,  100.f, kSq,  "ms", "Pre-delay spread") \
                                                                               \
    /* ── EARLY ──────────────────────────────────────────────────────────── */ \
    /* er_sz  [12 – 66 semi, linear in pitch]                                */ \
    /* Base pitch of the three early allpass stages.  Lower = longer taps.  */ \
    X(er_sz,      "er_sz",      58.6f, 12.f,  66.f, kLin, "semi", "Early size"    ) \
                                                                               \
    /* er_sym  [0 – 6 semi, linear]                                          */ \
    /* L/R pitch offset: szL = er_sz − er_sym, szR = er_sz + er_sym.        */ \
    X(er_sym,     "er_sym",      2.0f,  0.f,   6.f, kLin, "semi", "Early spread"  ) \
                                                                               \
    /* er_dffs  [0 – 0.95, linear]                                           */ \
    /* Allpass coefficient for the early diffusion stages.                   */ \
    /* 0 = bypass (pure tapped delay), 0.95 = maximum smear.                */ \
    X(er_dffs,    "er_dffs",     0.5f,  0.f,  0.95f, kLin, "",    "Early diffusion") \
                                                                               \
    /* ── LATE ───────────────────────────────────────────────────────────── */ \
    /* lt_sz  [−24 – 48 semi, linear in pitch]                               */ \
    /* Base pitch of the late loop delay lines.  Lower pitch = longer delays.*/ \
    /* 24 → ~96 ms loop.  6 → ~350 ms loop.  −24 → ~2 s loop.              */ \
    X(lt_sz,      "lt_sz",      24.0f,-24.f,  48.f, kLin, "semi", "Late size"     ) \
                                                                               \
    /* lt_theta  [0 – π rad, linear]                                          */ \
    /* Rotation angle in the 2×2 orthogonal diffuser before each delay read. */ \
    /* θ = 0: identity.  θ = π/4: max cross-mix.  θ = π/2: channel swap.   */ \
    /* Energy is preserved at all angles (rotation is always unitary).       */ \
    X(lt_theta,   "lt_theta",    0.7853982f, 0.f, 3.1415927f, kLin, "rad", "Late scatter") \
                                                                               \
    /* lt_dffs  [0 – 0.95, linear]                                           */ \
    /* Allpass coefficient for each late diffuser stage.                     */ \
    /* 0 = pure delay (current behaviour).  0.7 = rich smearing.            */ \
    /* Higher values dramatically increase echo density per loop pass.       */ \
    X(lt_dffs,    "lt_dffs",     0.6f,  0.f,  0.95f, kLin, "",    "Late diffusion") \
                                                                               \
    /* ── DECAY ──────────────────────────────────────────────────────────── */ \
    /* rt60_s  [0.05 – 120 s, exponential]                                   */ \
    X(rt60_s,     "rt60_s",      3.3f,  0.05f,120.f, kExp, "s",   "RT60"          ) \
                                                                               \
    /* bloom  [0.25 – 12×, linear]                                           */ \
    /* Multiplier on the effective RT60 in the fb_gain formula.              */ \
    /* > 1: tail grows before it fades (SMD swell).  1: standard decay.     */ \
    X(bloom,      "bloom",       3.14f, 0.25f, 12.f, kLin, "x",   "Bloom"         ) \
                                                                               \
    /* ── MODULATION ─────────────────────────────────────────────────────── */ \
    /* mod_hz  [0.005 – 5 Hz, exponential]  — LFO rate                      */ \
    X(mod_hz,     "mod_hz",      0.5f,  0.005f, 5.f, kExp, "Hz",  "Mod rate"      ) \
                                                                               \
    /* mod_ms  [0 – 5 ms, linear]  — LFO depth (peak delay swing)           */ \
    X(mod_ms,     "mod_ms",      1.5f,  0.f,   5.f, kLin, "ms",  "Mod depth"     ) \
                                                                               \
    /* chaos  [0 – 1, linear]                                                */ \
    /* At 0: all 8 LFOs locked to the same rate (mod_hz), equally-spaced.   */ \
    /* At 1: LFO rates spread φ^(k/7) apart (1× – 1.618×), plus a slow     */ \
    /* per-tap noise oscillator drifts each tap's depth ±60%.               */ \
    /* Creates non-periodic modulation that sounds alive and never repeats.  */ \
    X(chaos,      "chaos",       0.f,   0.f,   1.f, kLin, "",    "Chaos"         ) \
                                                                               \
    /* ── IN-LOOP EQ ─────────────────────────────────────────────────────── */ \
    /* Two-band shelf applied on every pass of early and late.               */ \
    /* Gains must be ≤ 0 dB (LoopShelf assertion) for loop stability.       */ \
    /* LP shelf: frequencies BELOW lp_hz get gain lp_db.                    */ \
    /* HP shelf: frequencies ABOVE hp_hz get gain hp_db.                    */ \
    X(lp_hz,  "lp_hz",    65.f,   20.f, 2000.f, kExp, "Hz", "Loop LP freq"  ) \
    X(lp_db,  "lp_db",     0.f,  -60.f,    0.f, kLin, "dB", "Loop LP gain"  ) \
    X(hp_hz,  "hp_hz",  4186.f,  500.f,20000.f, kExp, "Hz", "Loop HP freq"  ) \
    X(hp_db,  "hp_db",     0.f,  -60.f,    0.f, kLin, "dB", "Loop HP gain"  ) \
                                                                               \
    /* ── OUTPUT FILTER ──────────────────────────────────────────────────── */ \
    /* Four-pole Moog ladder LP + first-order HP → parametric bandpass.      */ \
    /* LP cutoff = out_freq × 2^(out_width/2)                                */ \
    /* HP cutoff = out_freq / 2^(out_width/2)                                */ \
    /* At width=10 both cutoffs are outside the audible range → flat.        */ \
    X(out_freq,  "out_freq",  1000.f,   80.f,18000.f, kExp, "Hz",  "Out center" ) \
    X(out_width, "out_width",   10.f,   0.5f,   10.f, kLin, "oct", "Out width"  ) \
                                                                               \
    /* ── CROSSFADES ─────────────────────────────────────────────────────── */ \
    /* 0 = early only, 1 = late only.                                        */ \
    X(el_mix,     "el_mix",      0.55f, 0.f,   1.f, kLin, "",    "Early/Late"    ) \
                                                                               \
    /* 0 = dry only, 1 = full wet (send/return use).                         */ \
    X(dw_mix,     "dw_mix",      0.5f,  0.f,   1.f, kLin, "",    "Dry/Wet"       )

// ── generated: the struct ────────────────────────────────────────────────────

struct Params {
#define X(id, key, def, lo, hi, curve, unit, label) float id = def;
    VERB_PARAM_LIST(X)
#undef X

    float lt_sym = 2.5f;  // baked: ±2.5 semi between late L and R loop

    void Derive() {}      // placeholder for future SIZE macro; currently a no-op
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
