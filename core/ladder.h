#pragma once
#include "shelf.h"
#include "util.h"
#include <cmath>

// Four-pole Moog ladder lowpass (linear, no saturation).
//
// TPT (topology-preserving transform) one-pole stages cascaded × 4 with
// a delayed global resonance feedback. One sample of delay in the feedback
// is inaudible at 48 kHz and avoids solving the nonlinear system per sample.
//
// g: bilinear-warped cutoff (tanf(π·fc/fs))
// k: resonance in [0, 4); 4 = self-oscillation threshold
struct Ladder {
    void SetParams(float cutoff_hz, float res_01, float fs) {
        float fc = clampf(cutoff_hz, 1.f, fs * 0.499f);
        g_ = tanf(float(M_PI) * fc / fs);
        k_ = clampf(res_01, 0.f, 1.f) * 3.96f;
    }

    float Process(float x) {
        float u = x - k_ * out_;
        for (int i = 0; i < 4; ++i) {
            float v = g_ * (u - s_[i]) / (1.f + g_);
            u = s_[i] + v;
            s_[i] += 2.f * v;
        }
        return (out_ = u);
    }

private:
    float s_[4] = {}, out_ = 0.f;
    float g_ = 0.f, k_ = 0.f;
};

// First-order highpass (HP = input − LP).
struct HP1 {
    void SetHP(float cutoff_hz, float fs) {
        float fc = clampf(cutoff_hz, 1.f, fs * 0.499f);
        lp_.SetCutoff(fc, fs);
    }
    float Process(float x) { return x - lp_.LP(x); }
private:
    OnePole lp_;
};
