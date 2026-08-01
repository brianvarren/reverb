#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cmath>
#include <complex>
#include <vector>

#include "util.h"
#include "delay.h"
#include "shelf.h"

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

    for (size_t i = 0; i < N; ++i)
        dl.Write(float(i));

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

    for (int d = 2; d <= 20; ++d) {
        float expected = float(N - 1) - float(d) - 0.5f;
        float got      = dl.Read4pt(float(d) + 0.5f);
        CHECK(std::abs(got - expected) < 1e-4f);
    }
}

TEST_CASE("DelayLine: amplitude flatness - 4pt vs linear interpolation") {
    constexpr size_t N    = 1024;
    constexpr float  kFS  = 48000.f;
    constexpr float  kHz  = 1000.f;
    constexpr float  kTwoPi = 2.f * float(M_PI);

    std::vector<float> buf(N, 0.f);
    DelayLine dl;
    dl.Init(buf.data(), N);

    for (size_t i = 0; i < N; ++i)
        dl.Write(sinf(kTwoPi * kHz * float(i) / kFS));

    const int base_d = 10;
    const int steps  = 1000;
    for (int k = 0; k <= steps; ++k) {
        float frac     = float(k) / float(steps);
        float d        = float(base_d) + frac;
        float got      = dl.Read4pt(d);
        float expected = sinf(kTwoPi * kHz * (float(N - 1) - d) / kFS);
        CHECK(std::abs(got - expected) < 0.005f);
    }
}

// ── §9.3  Shelf magnitude ────────────────────────────────────────────────────

// Analytic complex LP transfer function at digital frequency omega = 2π*f/fs.
static std::complex<double> h_lp(double a, double omega) {
    double b = 1.0 - a;
    // H(z) = a / (1 - b*z^{-1}), z = e^{jω}
    // denominator = 1 - b*e^{-jω} = (1 - b*cosω) + j*(b*sinω)
    std::complex<double> denom(1.0 - b * std::cos(omega), b * std::sin(omega));
    return a / denom;
}

// Analytic magnitude of the combined (high shelf + low shelf) Shelf::Process.
static double shelf_analytic_mag(double a_h, double A_h,
                                  double a_l, double A_l, double omega) {
    auto F_h = h_lp(a_h, omega);
    auto F_l = h_lp(a_l, omega);
    // High shelf applied first: H_hs = A_h + (1-A_h)*F_h
    auto H_hs = A_h + (1.0 - A_h) * F_h;
    // Low shelf applied to H_hs output: H_ls = A_l*F_l + (1-A_l)*(1-F_l)
    auto H_ls = A_l * F_l + (1.0 - A_l) * (std::complex<double>(1, 0) - F_l);
    return std::abs(H_hs * H_ls);
}

// Feed a unit sine at freq, return steady-state amplitude via quadrature.
// For a settled filter, output = A*sin(ωn+φ).  From two consecutive samples:
//   y[n]   = A*sin(ωn+φ)
//   y[n+1] = A*sin(ω(n+1)+φ)  →  (y[n+1] - y[n]*cos(ω)) / sin(ω) = A*cos(ωn+φ)
//   A = sqrt( y[n]² + ((y[n+1] - y[n]*cosω) / sinω)² )
// Exact for any phase, works at all frequencies ≠ 0 and ≠ Nyquist.
static float measure_gain(float hf_hz, float hb_lin,
                           float lf_hz, float lb_lin,
                           float freq, float fs,
                           int n_settle = 16384) {
    OutputShelf s;
    s.SetParams(hf_hz, hb_lin, lf_hz, lb_lin, fs);

    // Phase accumulator keeps sinf argument in [0, 2π] — avoids the ~13-bit
    // precision loss that sinf(omega * 16384) would suffer at high frequencies.
    const float omega   = 2.f * float(M_PI) * freq / fs;
    const float twopi   = 2.f * float(M_PI);
    float phase = 0.f;
    auto next_sample = [&]() -> float {
        float v = sinf(phase);
        phase += omega;
        if (phase >= twopi) phase -= twopi;
        return v;
    };

    for (int i = 0; i < n_settle; ++i)
        s.Process(next_sample());

    float y0   = s.Process(next_sample());
    float y1   = s.Process(next_sample());
    float quad = (y1 - y0 * cosf(omega)) / sinf(omega);   // = A*cos(phase)
    return sqrtf(y0 * y0 + quad * quad);
}

TEST_CASE("Shelf: magnitude matches analytic within 0.1 dB") {
    constexpr float kFS = 48000.f;

    // Fixed parameters: moderately dark in-loop shelf
    const float hf_hz  = pitch_to_hz(96.f);   // ~2093 Hz
    const float hb_lin = db_to_lin(-6.f);
    const float lf_hz  = pitch_to_hz(60.f);   // ~262 Hz
    const float lb_lin = db_to_lin(0.f);       // 0 dB = unity

    // One-pole coefficients (match OnePole::SetCutoff)
    const double a_h = 1.0 - std::exp(-2.0 * M_PI * hf_hz / kFS);
    const double a_l = 1.0 - std::exp(-2.0 * M_PI * lf_hz / kFS);

    // 20 log-spaced frequencies from 20 Hz to 20 kHz
    for (int i = 0; i < 20; ++i) {
        float freq = 20.f * powf(1000.f, float(i) / 19.f);  // 20 Hz → 20 kHz
        double omega    = 2.0 * M_PI * freq / kFS;
        double expected = shelf_analytic_mag(a_h, hb_lin, a_l, lb_lin, omega);
        float  measured = measure_gain(hf_hz, hb_lin, lf_hz, lb_lin, freq, kFS);

        double expected_dB = 20.0 * std::log10(std::max(expected, 1e-9));
        double measured_dB = 20.0 * std::log10(std::max(double(measured), 1e-9));
        CHECK(std::abs(measured_dB - expected_dB) < 0.1);
    }
}

TEST_CASE("Shelf<true>: max gain <= 1.0 for 100 random in-loop param sets") {
    // Stability precondition: any in-loop shelf must satisfy max|H(ω)| <= 1.
    // Test 100 parameter sets across a 512-point frequency grid.
    constexpr float kFS = 48000.f;

    // Simple LCG for reproducible pseudo-random params
    uint32_t rng = 0xDEADBEEF;
    auto rand01 = [&]() -> float {
        rng = rng * 1664525u + 1013904223u;
        return float(rng >> 1) / float(0x7FFFFFFFu);
    };

    for (int trial = 0; trial < 100; ++trial) {
        float hf_hz  = 100.f + rand01() * 9900.f;   // 100 – 10000 Hz
        float hb_lin = rand01();                      // 0 – 1.0  (i.e. <= 0 dB)
        float lf_hz  = 50.f  + rand01() * 5000.f;    // 50  – 5050 Hz
        float lb_lin = rand01();                      // 0 – 1.0

        double a_h = 1.0 - std::exp(-2.0 * M_PI * hf_hz / kFS);
        double a_l = 1.0 - std::exp(-2.0 * M_PI * lf_hz / kFS);

        // 512-point grid from 0 to π (DC to Nyquist)
        for (int k = 0; k < 512; ++k) {
            double omega = double(k) / 512.0 * M_PI;
            double mag   = shelf_analytic_mag(a_h, hb_lin, a_l, lb_lin, omega);
            CHECK(mag <= 1.0 + 1e-9);  // tiny epsilon for floating-point rounding
        }
    }
}
