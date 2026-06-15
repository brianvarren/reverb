#pragma once
#include "dsp.h"

// Classic Schroeder reverb: 4 parallel feedback combs → 2 series all-passes.
// Delay times are mutually prime to avoid metallic resonances.
// Values scaled for 48 kHz.
class Schroeder {
    CombFilter<2300> c0_{1687, 0.84f, 0.2f};  // ~35 ms
    CombFilter<2300> c1_{1601, 0.84f, 0.2f};  // ~33 ms
    CombFilter<2300> c2_{2053, 0.84f, 0.2f};  // ~43 ms
    CombFilter<2300> c3_{2251, 0.84f, 0.2f};  // ~47 ms

    AllPassFilter<400> a0_{347, 0.5f};         // ~7 ms
    AllPassFilter<150> a1_{113, 0.5f};         // ~2 ms

public:
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
