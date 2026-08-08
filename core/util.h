#pragma once
#include <cmath>

inline float pitch_to_samples(float p, float fs) {
    return (fs / 440.0f) * exp2f((69.0f - p) * (1.0f / 12.0f));
}

inline float pitch_to_hz(float p)   { return 440.f * exp2f((p - 69.f) / 12.f); }
inline float db_to_lin(float db)    { return powf(10.f, db / 20.f); }

inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

struct Smoother {
    float y = 0.f, a = 1.f, max_step = 1e9f;
    void SetTC(float tc_s, float block_dt) { a = 1.f - expf(-block_dt / tc_s); }
    // Clamp the per-block step so large jumps become a constant-rate pitch glide
    // rather than an exponential lurch. No effect when not set (default = inf).
    void SetMaxStep(float s) { max_step = s; }
    float Process(float target) {
        float delta = a * (target - y);
        if (delta >  max_step) delta =  max_step;
        if (delta < -max_step) delta = -max_step;
        y += delta;
        return y;
    }
    void Reset(float v) { y = v; }
};
