#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cmath>
#include <vector>

#include "util.h"
#include "delay.h"

// ── §9.1  pitch_to_samples ───────────────────────────────────────────────────

TEST_CASE("pitch_to_samples: A4=69 maps to fs/440") {
    float got      = pitch_to_samples(69.f, 48000.f);
    float expected = 48000.f / 440.f;
    CHECK(std::abs(got - expected) < 1e-4f);
}

TEST_CASE("pitch_to_samples: octave property") {
    for (int p = 12; p <= 60; ++p) {
        float lower  = pitch_to_samples(float(p),      48000.f);
        float higher = pitch_to_samples(float(p + 12), 48000.f);
        // one octave up halves the period
        CHECK(std::abs(lower - 2.f * higher) < 1e-3f);
    }
}

TEST_CASE("pitch_to_samples: monotonic decreasing") {
    for (int p = 12; p < 80; ++p)
        CHECK(pitch_to_samples(float(p), 48000.f) > pitch_to_samples(float(p + 1), 48000.f));
}

// ── §9.2  DelayLine ──────────────────────────────────────────────────────────

TEST_CASE("DelayLine: integer reads exact for ramp input") {
    constexpr size_t N = 256;
    std::vector<float> buf(N, 0.f);
    DelayLine dl;
    dl.Init(buf.data(), N);

    // Write ramp 0, 1, 2, ..., N-1
    for (size_t i = 0; i < N; ++i)
        dl.Write(float(i));

    // After N writes the pointer wrapped to 0.
    // Read4pt(d) for delay d should give (N-1) - d.
    for (int d = 2; d <= 20; ++d) {
        float expected = float(N - 1 - d);
        float got      = dl.Read4pt(float(d));
        CHECK(std::abs(got - expected) < 1e-4f);
    }
}

TEST_CASE("DelayLine: half-sample read matches analytic Lagrange cubic") {
    constexpr size_t N = 256;
    std::vector<float> buf(N, 0.f);
    DelayLine dl;
    dl.Init(buf.data(), N);

    for (size_t i = 0; i < N; ++i)
        dl.Write(float(i));

    // For a linear ramp the cubic Lagrange is exact: value at d+0.5 = (N-1) - (d+0.5)
    for (int d = 2; d <= 20; ++d) {
        float expected = float(N - 1) - float(d) - 0.5f;
        float got      = dl.Read4pt(float(d) + 0.5f);
        CHECK(std::abs(got - expected) < 1e-4f);
    }
}

TEST_CASE("DelayLine: amplitude flatness - 4pt vs linear interpolation") {
    // Pre-fill a delay line with a 1 kHz sine at 48 kHz.
    // Sweep the fractional read offset from 0 to 1.
    // 4-point Lagrange should stay within 0.5% of the expected analytic value.
    // Linear interpolation would droop to 0.637 at half-sample — this test
    // catches that regression if Read4pt ever gets replaced with lerp.

    constexpr size_t N    = 1024;
    constexpr float  kFS  = 48000.f;
    constexpr float  kHz  = 1000.f;
    constexpr float  kTwoPi = 2.f * float(M_PI);

    std::vector<float> buf(N, 0.f);
    DelayLine dl;
    dl.Init(buf.data(), N);

    for (size_t i = 0; i < N; ++i)
        dl.Write(sinf(kTwoPi * kHz * float(i) / kFS));

    // After N writes w_==0. Read4pt(d) returns the sample written at step N-1-d,
    // i.e. sin(2π * kHz * (N-1-d) / kFS).
    const int base_d = 10;
    const int steps  = 1000;
    for (int k = 0; k <= steps; ++k) {
        float frac     = float(k) / float(steps);
        float d        = float(base_d) + frac;
        float got      = dl.Read4pt(d);
        float expected = sinf(kTwoPi * kHz * (float(N - 1) - d) / kFS);
        CHECK(std::abs(got - expected) < 0.005f);   // 0.5% of unit amplitude
    }
}
