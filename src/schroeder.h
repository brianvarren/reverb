#pragma once
#include "dsp.h"

// Classic Schroeder reverb: 4 parallel feedback combs → 2 series all-passes.
// Delay times are mutually prime to avoid metallic resonances.
// Values scaled for 48 kHz.
class Schroeder {
    CombFilter<2300> c0_;  // ~35 ms
    CombFilter<2300> c1_;  // ~33 ms
    CombFilter<2300> c2_;  // ~43 ms
    CombFilter<2300> c3_;  // ~47 ms

    AllPassFilter<400> a0_; // ~7 ms
    AllPassFilter<150> a1_; // ~2 ms

public:
    // Delay times (samples) default to values mutually prime enough to avoid
    // metallic resonances at 48 kHz. fb/damp/apGain are the primary tuning knobs.
    Schroeder(float fb = 0.84f, float damp = 0.2f, float apGain = 0.5f,
              int d0 = 1687, int d1 = 1601, int d2 = 2053, int d3 = 2251,
              int da0 = 347, int da1 = 113)
        : c0_(d0, fb, damp), c1_(d1, fb, damp), c2_(d2, fb, damp), c3_(d3, fb, damp),
          a0_(da0, apGain), a1_(da1, apGain) {}

    float process(float x) {
        float wet = c0_.process(x) + c1_.process(x)
                  + c2_.process(x) + c3_.process(x);
        wet *= 0.25f;
        wet = a0_.process(wet);
        wet = a1_.process(wet);
        return wet;
    }

    void clear() {
        c0_.clear(); c1_.clear(); c2_.clear(); c3_.clear();
        a0_.clear(); a1_.clear();
    }
};
