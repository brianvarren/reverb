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
    float y = 0.f, a = 1.f;
    void SetTC(float tc_s, float block_dt) { a = 1.f - expf(-block_dt / tc_s); }
    float Process(float target) { y += a * (target - y); return y; }
    void Reset(float v) { y = v; }
};
