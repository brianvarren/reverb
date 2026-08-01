#pragma once
#include <cmath>

// 2×2 rotation matrix applied in-place.
// Energy-preserving: ‖Rx‖ == ‖x‖ for all x, all θ.
// Default θ = π/4 (45°), which gives maximum cross-mixing (c = s = 1/√2).
// Recompute c/s per block via SetAngle; never call SetAngle per sample.

struct Rotation {
    float c = 0.70710678f;   // cos(π/4)
    float s = 0.70710678f;   // sin(π/4)

    void SetAngle(float th) { c = cosf(th); s = sinf(th); }

    void Process(float& a, float& b) const {
        float a2 = c * a - s * b;
        float b2 = s * a + c * b;
        a = a2; b = b2;
    }
};
