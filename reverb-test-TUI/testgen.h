#pragma once
#include <cmath>
#include <cstdint>
#include "util.h"

// ─────────────────────────────────────────────────────────────────────────────
// Test-signal generator for live reverb tuning.
//
// Four excitation types, all mono (fed identically to L and R) so that every
// bit of stereo you hear is produced by the reverb, not by the source.
//
//   impulse — single unit sample. Full spectrum, zero duration: the honest
//             look at the impulse response. Sounds quiet; that is correct.
//   noise   — white burst. Fills every mode at once; the fastest way to hear
//             density, metallic ringing, and the shape of the bloom.
//   sine    — windowed sine burst. Isolates modal behaviour: if the loop rings
//             at a particular frequency you will hear it beat against the tone.
//   saw     — windowed polyBLEP saw. Band-limited, so aliasing never gets
//             mistaken for reverb grit. Harmonically rich like real material.
//
// Tones and noise get a 2 ms raised-cosine attack and a 15 ms release so the
// burst edges do not smear a click into the reverb and confuse what you hear.
// ─────────────────────────────────────────────────────────────────────────────

enum GenType { kGenImpulse = 0, kGenNoise, kGenSine, kGenSaw, kGenTypeCount };

inline const char* GenName(int t) {
    static const char* n[kGenTypeCount] = { "impulse", "noise", "sine", "saw" };
    return (t >= 0 && t < kGenTypeCount) ? n[t] : "?";
}

class TestGen {
public:
    void Init(float fs) { fs_ = fs; rng_ = 0x9E3779B9u; active_ = false; }

    // pitch is MIDI pitch (ignored for impulse/noise). len_ms ignored for impulse.
    void Trigger(int type, float pitch, float len_ms, float level_db) {
        type_ = type;
        len_  = (type == kGenImpulse) ? 1L : (long)(len_ms * 0.001f * fs_);
        if (len_ < 1) len_ = 1;
        amp_    = db_to_lin(level_db);
        inc_    = pitch_to_hz(pitch) / fs_;
        phase_  = 0.f;
        n_      = 0;
        active_ = true;
    }

    void Stop()          { active_ = false; }
    bool active()  const { return active_; }

    float Process() {
        if (!active_) return 0.f;
        const float e = _env();
        float y = 0.f;
        switch (type_) {
            case kGenImpulse: y = (n_ == 0) ? 1.f : 0.f;                 break;
            case kGenNoise:   y = _noise() * e;                          break;
            case kGenSine:    y = sinf(6.2831853f * phase_) * e;         break;
            case kGenSaw:     y = _saw() * e;                            break;
            default: break;
        }
        phase_ += inc_;
        if (phase_ >= 1.f) phase_ -= 1.f;
        if (++n_ >= len_) active_ = false;
        return y * amp_;
    }

private:
    // Raised-cosine attack and release. Overlaps gracefully on very short bursts.
    float _env() const {
        const float kPi = 3.14159265f;
        const float atk = 0.002f * fs_;
        const float rel = 0.015f * fs_;
        float a = 1.f, r = 1.f;
        if (float(n_) < atk)          a = 0.5f - 0.5f * cosf(kPi * float(n_) / atk);
        const float left = float(len_ - n_);
        if (left < rel)               r = 0.5f - 0.5f * cosf(kPi * left / rel);
        return a * r;
    }

    float _noise() {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return float(int32_t(rng_)) * (1.f / 2147483648.f);
    }

    // polyBLEP: subtracts the aliasing of the discontinuity at the wrap point.
    static float _blep(float t, float dt) {
        if (dt <= 0.f) return 0.f;
        if (t < dt)        { t /= dt;         return t + t - t * t - 1.f; }
        if (t > 1.f - dt)  { t = (t - 1.f)/dt; return t * t + t + t + 1.f; }
        return 0.f;
    }

    float _saw() {
        float y = 2.f * phase_ - 1.f;
        return y - _blep(phase_, inc_);
    }

    float    fs_     = 48000.f;
    int      type_   = kGenSine;
    long     len_    = 1, n_ = 0;
    float    amp_    = 0.5f;
    float    phase_  = 0.f, inc_ = 0.f;
    bool     active_ = false;
    uint32_t rng_    = 0x9E3779B9u;
};
