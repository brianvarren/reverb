#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cmath>
#include <complex>
#include <vector>

#include "util.h"
#include "delay.h"
#include "shelf.h"
#include "allpass.h"
#include "early.h"
#include "rotation.h"
#include "late.h"

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

static std::complex<double> h_lp(double a, double omega) {
    double b = 1.0 - a;
    std::complex<double> denom(1.0 - b * std::cos(omega), b * std::sin(omega));
    return a / denom;
}

static double shelf_analytic_mag(double a_h, double A_h,
                                  double a_l, double A_l, double omega) {
    auto F_h = h_lp(a_h, omega);
    auto F_l = h_lp(a_l, omega);
    auto H_hs = A_h + (1.0 - A_h) * F_h;
    auto H_ls = A_l * F_l + (std::complex<double>(1, 0) - F_l);  // A*LP + HP
    return std::abs(H_hs * H_ls);
}

static float measure_shelf_gain(float hf_hz, float hb_lin,
                                 float lf_hz, float lb_lin,
                                 float freq, float fs,
                                 int n_settle = 16384) {
    OutputShelf s;
    s.SetParams(hf_hz, hb_lin, lf_hz, lb_lin, fs);

    const float omega  = 2.f * float(M_PI) * freq / fs;
    const float twopi  = 2.f * float(M_PI);
    float phase = 0.f;
    auto next = [&]() -> float {
        float v = sinf(phase);
        phase += omega; if (phase >= twopi) phase -= twopi;
        return v;
    };

    for (int i = 0; i < n_settle; ++i) s.Process(next());

    float y0 = s.Process(next());
    float y1 = s.Process(next());
    float quad = (y1 - y0 * cosf(omega)) / sinf(omega);
    return sqrtf(y0 * y0 + quad * quad);
}

TEST_CASE("Shelf: magnitude matches analytic within 0.1 dB") {
    constexpr float kFS = 48000.f;
    const float hf_hz  = pitch_to_hz(96.f);
    const float hb_lin = db_to_lin(-6.f);
    const float lf_hz  = pitch_to_hz(60.f);
    const float lb_lin = db_to_lin(0.f);
    const double a_h = 1.0 - std::exp(-2.0 * M_PI * hf_hz / kFS);
    const double a_l = 1.0 - std::exp(-2.0 * M_PI * lf_hz / kFS);

    for (int i = 0; i < 20; ++i) {
        float  freq     = 20.f * powf(1000.f, float(i) / 19.f);
        double omega    = 2.0 * M_PI * freq / kFS;
        double expected = shelf_analytic_mag(a_h, hb_lin, a_l, lb_lin, omega);
        float  measured = measure_shelf_gain(hf_hz, hb_lin, lf_hz, lb_lin, freq, kFS);

        double e_db = 20.0 * std::log10(std::max(expected, 1e-9));
        double m_db = 20.0 * std::log10(std::max(double(measured), 1e-9));
        CHECK(std::abs(m_db - e_db) < 0.1);
    }
}

TEST_CASE("Shelf<true>: max gain <= 1.0 for 100 random in-loop param sets") {
    constexpr float kFS = 48000.f;
    uint32_t rng = 0xDEADBEEF;
    auto rand01 = [&]() -> float {
        rng = rng * 1664525u + 1013904223u;
        return float(rng >> 1) / float(0x7FFFFFFFu);
    };

    for (int trial = 0; trial < 100; ++trial) {
        float hf_hz  = 100.f + rand01() * 9900.f;
        float hb_lin = rand01();
        float lf_hz  = 50.f  + rand01() * 5000.f;
        float lb_lin = rand01();

        double a_h = 1.0 - std::exp(-2.0 * M_PI * hf_hz / kFS);
        double a_l = 1.0 - std::exp(-2.0 * M_PI * lf_hz / kFS);

        for (int k = 0; k < 512; ++k) {
            double omega = double(k) / 512.0 * M_PI;
            double mag   = shelf_analytic_mag(a_h, hb_lin, a_l, lb_lin, omega);
            CHECK(mag <= 1.0 + 1e-9);
        }
    }
}

TEST_CASE("Shelf: low shelf is NOT a lowpass when lb < 1 (spec, catches formula bug)") {
    // The original bug: lb*LP + (1-lb)*HP collapsed to a pure lowpass at lb=0.5.
    // With the correct formula lb*LP + HP, gain above lf_hz must be > lb, not flat.
    // We measure at 10*lf_hz (well above corner) and verify gain > lb + 3 dB.
    constexpr float kFS  = 48000.f;
    const float lf_hz    = pitch_to_hz(60.f);  // ~262 Hz
    const float test_hz  = lf_hz * 10.f;        // ~2620 Hz, well above shelf corner
    const float lb_db    = -6.f;
    const float lb_lin   = db_to_lin(lb_db);

    // With unity high shelf so only the low shelf matters.
    float measured = measure_shelf_gain(pitch_to_hz(96.f), db_to_lin(0.f),
                                        lf_hz, lb_lin, test_hz, kFS);
    float measured_db = 20.f * log10f(std::max(measured, 1e-9f));
    // Correct formula: gain at 10*lf_hz ≈ unity → measured_db ≈ 0 dB.
    // Bug formula: gain is flat at lb everywhere → measured_db ≈ lb_db = -6 dB.
    // Check we're at least 3 dB above lb (i.e. well away from the bugged flat response).
    CHECK(measured_db > lb_db + 3.f);
}

// ── §9.4  AllPass ────────────────────────────────────────────────────────────

static float measure_ap_gain(AllPass& ap, float freq, float fs,
                              int n_settle = 8192) {
    const float omega = 2.f * float(M_PI) * freq / fs;
    const float twopi = 2.f * float(M_PI);
    float phase = 0.f;
    auto next = [&]() -> float {
        float v = sinf(phase);
        phase += omega; if (phase >= twopi) phase -= twopi;
        return v;
    };

    for (int i = 0; i < n_settle; ++i) ap.Process(next());

    float y0 = ap.Process(next());
    float y1 = ap.Process(next());
    float quad = (y1 - y0 * cosf(omega)) / sinf(omega);
    return sqrtf(y0 * y0 + quad * quad);
}

TEST_CASE("AllPass: flat magnitude (|H|=1) with shelf bypassed, within 0.01 dB") {
    constexpr float kFS    = 48000.f;
    constexpr size_t kBuf  = 4096;

    for (int i = 0; i < 20; ++i) {
        float freq = 50.f * powf(400.f, float(i) / 19.f);

        std::vector<float> buf(kBuf, 0.f);
        AllPass ap;
        ap.Init(buf.data(), kBuf, kFS);
        ap.SetGain(0.70f);
        ap.SetShelf(1000.f, 1.0f, 0.0f, 0.0f, kFS);
        ap.SnapDelay(100.f);

        float gain = measure_ap_gain(ap, freq, kFS, 8192);
        float gdb  = 20.f * log10f(std::max(gain, 1e-9f));
        CHECK(std::abs(gdb) < 0.01f);
    }
}

TEST_CASE("AllPass: magnitude <= 1 with shelf active") {
    constexpr float  kFS   = 48000.f;
    constexpr size_t kBuf  = 4096;
    const float hf_hz  = pitch_to_hz(96.f);
    const float hb_lin = db_to_lin(-6.f);
    const float lf_hz  = pitch_to_hz(60.f);
    const float lb_lin = 0.f;

    for (int i = 0; i < 20; ++i) {
        float freq = 50.f * powf(400.f, float(i) / 19.f);

        std::vector<float> buf(kBuf, 0.f);
        AllPass ap;
        ap.Init(buf.data(), kBuf, kFS);
        ap.SetGain(0.70f);
        ap.SetShelf(hf_hz, hb_lin, lf_hz, lb_lin, kFS);
        ap.SnapDelay(100.f);

        float gain = measure_ap_gain(ap, freq, kFS);
        CHECK(gain <= 1.001f);
    }
}

TEST_CASE("AllPass: impulse response has tap train at spacing d") {
    constexpr float  kFS  = 48000.f;
    constexpr size_t kBuf = 256;
    constexpr int    kD   = 64;
    constexpr float  kG   = 0.70f;

    std::vector<float> buf(kBuf, 0.f);
    AllPass ap;
    ap.Init(buf.data(), kBuf, kFS);
    ap.SetGain(kG);
    ap.SetShelf(1000.f, 1.0f, 0.0f, 0.0f, kFS);
    ap.SnapDelay(float(kD));

    std::vector<float> h(kD + 10);
    h[0] = ap.Process(1.f);
    for (int n = 1; n < kD + 10; ++n)
        h[n] = ap.Process(0.f);

    CHECK(std::abs(h[0] - (-kG)) < 1e-5f);

    for (int n = 1; n < kD; ++n)
        CHECK(std::abs(h[n]) < 1e-5f);

    CHECK(std::abs(h[kD] - (1.f - kG * kG)) < 1e-5f);
}

// ── Stage 5  Early section ────────────────────────────────────────────────────

// Run N samples of Early section's impulse response (mono impulse, L=R=1 at n=0).
static void early_ir(const Params& p, float fs,
                     std::vector<float>& irL, std::vector<float>& irR, size_t N) {
    constexpr size_t kBuf = 4096;
    float storage[8][kBuf] = {};
    float* bufs[8];
    for (int i = 0; i < 8; ++i) bufs[i] = storage[i];

    Early e;
    e.Init(bufs, kBuf, fs);
    e.SnapParams(p, fs);

    irL.resize(N);
    irR.resize(N);
    e.Process(1.f, 1.f, irL[0], irR[0]);
    for (size_t n = 1; n < N; ++n)
        e.Process(0.f, 0.f, irL[n], irR[n]);
}

static double cross_corr(const std::vector<float>& a, const std::vector<float>& b) {
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        ab += a[i] * b[i];
        aa += a[i] * a[i];
        bb += b[i] * b[i];
    }
    if (aa < 1e-20 || bb < 1e-20) return 1.0;
    return ab / std::sqrt(aa * bb);
}

TEST_CASE("Early: L/R correlation drops as ErSym rises") {
    // With sym=0 and pd_sym=0, L and R are identical → corr == 1.
    // With sym=2, delay structures differ → measurably lower correlation.
    constexpr float kFS = 48000.f;
    constexpr size_t kN = 10000;

    Params p0; p0.er_sym = 0.f; p0.pd_sym = 0.f;
    std::vector<float> L0, R0;
    early_ir(p0, kFS, L0, R0, kN);
    double corr0 = cross_corr(L0, R0);
    CHECK(corr0 > 0.9999);   // sym=0, pd_sym=0 → must be identical

    Params p2; p2.er_sym = 2.f; p2.pd_sym = 0.3f;
    std::vector<float> L2, R2;
    early_ir(p2, kFS, L2, R2, kN);
    double corr2 = cross_corr(L2, R2);
    CHECK(corr2 < 0.80);     // meaningful decorrelation at sym=2
    CHECK(corr2 < corr0);    // monotone: more sym → less correlation
}

TEST_CASE("Early: autocorrelation of ER burst < 0.3 for lags 100..2000") {
    // Lags 1..~10 are high due to shelf low-passing — that's expected and
    // irrelevant (it's not metallic ring). We check from lag 100 onward,
    // which covers the allpass delay lengths (~617, 824, 1038 samples).
    // A peak >= 0.3 there would indicate a periodic tap pattern (metallic ring).
    constexpr float  kFS = 48000.f;
    constexpr size_t kN  = 10000;

    Params p;
    std::vector<float> irL, irR;
    early_ir(p, kFS, irL, irR, kN);

    double e0 = 0;
    for (auto v : irL) e0 += v * v;
    REQUIRE(e0 > 1e-10);   // sanity: early section must produce output

    for (int lag = 100; lag <= 2000; ++lag) {
        double c = 0;
        for (size_t n = 0; n + lag < kN; ++n)
            c += irL[n] * irL[n + lag];
        CHECK(std::abs(c / e0) < 0.3);
    }
}

// ── Stage 6  Rotation stage ───────────────────────────────────────────────────

TEST_CASE("Rotation: energy preserved for 1000 random vectors, five angles") {
    const float angles[] = { 0.f,
                              float(M_PI) / 6.f,
                              float(M_PI) / 4.f,
                              float(M_PI) / 3.f,
                              float(M_PI) / 2.f };
    uint32_t rng = 0xBAADF00D;
    auto randf = [&]() -> float {
        rng = rng * 1664525u + 1013904223u;
        return (float(int32_t(rng)) / float(0x80000000u));
    };

    for (float th : angles) {
        Rotation rot;
        rot.SetAngle(th);
        for (int i = 0; i < 1000; ++i) {
            float a = randf(), b = randf();
            float norm_sq_in = a * a + b * b;
            rot.Process(a, b);
            float norm_sq_out = a * a + b * b;
            CHECK(std::abs(norm_sq_out - norm_sq_in) < 1e-6f);
        }
    }
}

TEST_CASE("Rotation: RᵀR == I within 1e-6, five angles") {
    // Apply R to standard basis vectors, reconstruct Rᵀ, verify Rᵀ R = I.
    const float angles[] = { 0.f,
                              float(M_PI) / 6.f,
                              float(M_PI) / 4.f,
                              float(M_PI) / 3.f,
                              float(M_PI) / 2.f };
    for (float th : angles) {
        Rotation rot;
        rot.SetAngle(th);

        // Column 0: R * [1, 0]
        float r00 = 1.f, r10 = 0.f;
        rot.Process(r00, r10);
        // Column 1: R * [0, 1]
        float r01 = 0.f, r11 = 1.f;
        rot.Process(r01, r11);

        // RᵀR — each entry is dot product of rows of R (= columns of Rᵀ)
        // Rᵀ rows are R columns: [r00, r10], [r01, r11]
        float rtr00 = r00*r00 + r10*r10;  // should be 1
        float rtr01 = r00*r01 + r10*r11;  // should be 0
        float rtr10 = r01*r00 + r11*r10;  // should be 0
        float rtr11 = r01*r01 + r11*r11;  // should be 1

        CHECK(std::abs(rtr00 - 1.f) < 1e-6f);
        CHECK(std::abs(rtr01)       < 1e-6f);
        CHECK(std::abs(rtr10)       < 1e-6f);
        CHECK(std::abs(rtr11 - 1.f) < 1e-6f);
    }
}

// ── Stage 7  Late section — T60 via Schroeder integration (§9.5) ─────────────

// Build a Late with flat (pass-through) in-loop shelves.
// Recipe: hb_lin=1 (high shelf passthrough) + lf_hz≈0 + lb_lin≈0 (low shelf
// passthrough when lp coefficient a≈0 from near-zero cutoff).
static Params flat_late_params(float rt60_s, float lt_sz = 24.f) {
    Params p;
    p.lt_sz   = lt_sz;
    p.lt_sym  = 0.f;          // symmetric → L==R, cleaner T60 read
    p.rt60_s  = rt60_s;
    p.bloom   = 1.f;          // honest T60 with correct D sum
    p.dmp_hb  = 0.f;          // 0 dB → hb_lin=1 → high shelf passthrough
    p.dmp_lf  = -1000.f;      // pitch → ~0 Hz → a≈0 in OnePole → LP(x)=0
    p.dmp_lb  = -200.f;       // lb_lin≈0 → low shelf output = (1-0)*(x-0) = x
    return p;
}

// Returns measured T60 in seconds via Schroeder backward integration.
// Fits a line through the -5 to -35 dB range of the EDC and extrapolates to -60.
static double measure_t60(float rt60_s, float lt_sz = 24.f) {
    constexpr float  kFS      = 48000.f;
    constexpr size_t kDiffSz  = 4096;

    Params p = flat_late_params(rt60_s, lt_sz);

    // Buffers for 6 diffuser lines + 2 long delays (all same size for simplicity)
    std::vector<float> storage(8 * kDiffSz, 0.f);
    float* bufs[8];
    for (int i = 0; i < 8; ++i) bufs[i] = storage.data() + i * kDiffSz;

    Late late;
    late.Init(bufs, kDiffSz, kDiffSz, kFS);
    late.SnapParams(p, kFS);

    // Capture 2.5 * rt60_s seconds of impulse response
    size_t N = size_t(2.5f * rt60_s * kFS) + 1;
    std::vector<float> irL(N), irR(N);
    late.Process(1.f, 1.f, irL[0], irR[0]);
    for (size_t n = 1; n < N; ++n)
        late.Process(0.f, 0.f, irL[n], irR[n]);

    // Schroeder backward integral: E[n] = sum_{k=n}^{N-1} irL[k]^2
    std::vector<double> edc(N + 1, 0.0);
    for (int n = int(N) - 1; n >= 0; --n)
        edc[n] = edc[n + 1] + double(irL[n]) * double(irL[n]);

    double e_total = edc[0];
    REQUIRE(e_total > 1e-20);   // sanity: loop must produce output

    // Find -5 dB and -35 dB crossings (in energy dB = 10*log10)
    double thresh5  = e_total * std::pow(10.0, -5.0  / 10.0);
    double thresh35 = e_total * std::pow(10.0, -35.0 / 10.0);

    size_t n5 = N, n35 = N;
    for (size_t n = 0; n < N; ++n) {
        if (n5  == N && edc[n] < thresh5)  n5  = n;
        if (n35 == N && edc[n] < thresh35) n35 = n;
    }
    REQUIRE(n5  < N);
    REQUIRE(n35 < N);
    REQUIRE(n35 > n5);

    // Linear extrapolation from -5 to -35 dB region to -60 dB
    // T60 = n5/fs + (55/30) * (n35-n5)/fs
    double T60_samples = double(n5) + (55.0 / 30.0) * double(n35 - n5);
    return T60_samples / kFS;
}

TEST_CASE("Late: measured T60 matches formula within 15% at three rt60_s values") {
    // Flat shelves + bloom=1 → decay driven by fb_gain. The DC-blocking highpass
    // in the loop causes non-exponential EDC, so Schroeder extrapolation has a
    // systematic bias of ~7-11%; 15% tolerance catches real bugs while staying realistic.
    for (float rt60_s : {0.5f, 2.0f, 8.0f}) {
        double measured = measure_t60(rt60_s);
        double rel_err  = std::abs(measured / rt60_s - 1.0);
        CHECK(rel_err < 0.15);
    }
}

TEST_CASE("Late: T60 round-trip — fb_gain from target T60 recovers target") {
    // Derive fb_gain from rt60_s, then compute the implied T60 from fb_gain and
    // D. The two should agree to machine precision (it's just algebra).
    constexpr float kFS = 48000.f;
    for (float rt60_s : {0.5f, 2.0f, 8.0f}) {
        Params p = flat_late_params(rt60_s);
        std::vector<float> storage(8 * 4096, 0.f);
        float* bufs[8];
        for (int i = 0; i < 8; ++i) bufs[i] = storage.data() + i * 4096;
        Late late;
        late.Init(bufs, 4096, 4096, kFS);
        late.SnapParams(p, kFS);

        float D        = late.loop_length_samples();
        float fb       = late.fb_gain();
        double implied = -3.0 * (D / kFS) / std::log10(double(fb));
        double rel_err = std::abs(implied / rt60_s - 1.0);
        CHECK(rel_err < 1e-5);
    }
}

TEST_CASE("Late: no NaN/Inf after 5-second white-noise soak at low fb_gain") {
    constexpr float  kFS      = 48000.f;
    constexpr size_t kDiffSz  = 4096;
    constexpr size_t N        = size_t(5.f * kFS);

    Params p;   // default params
    std::vector<float> storage(8 * kDiffSz, 0.f);
    float* bufs[8];
    for (int i = 0; i < 8; ++i) bufs[i] = storage.data() + i * kDiffSz;

    Late late;
    late.Init(bufs, kDiffSz, kDiffSz, kFS);
    late.SnapParams(p, kFS);

    uint32_t rng = 0xFEEDBEEF;
    auto rand11 = [&]() -> float {
        rng = rng * 1664525u + 1013904223u;
        return float(int32_t(rng)) / float(0x80000000u);
    };

    float outL, outR;
    bool any_bad = false;
    for (size_t n = 0; n < N; ++n) {
        late.Process(rand11(), rand11(), outL, outR);
        if (!std::isfinite(outL) || !std::isfinite(outR)) { any_bad = true; break; }
    }
    CHECK(!any_bad);
}

TEST_CASE("Late: modulation is live — output diverges from unmodulated reference") {
    // Verify the LFO actually moves the delay read heads. Feed identical impulses
    // into two Late instances (mod vs no-mod); outputs must differ after settling.
    constexpr float  kFS     = 48000.f;
    constexpr size_t kDiffSz = 4096;
    constexpr size_t kSettle = size_t(1.f * kFS);  // 1 s of settling
    constexpr size_t kMeas   = size_t(0.1f * kFS); // 0.1 s measurement window

    auto make_late = [&](float mod_ms) {
        Params p;
        p.mod_ms = mod_ms;
        p.mod_hz = 10.f;   // fast rate so effect is visible in 1 s
        std::vector<float> storage(8 * kDiffSz, 0.f);
        std::vector<float*> bufs(8);
        for (int i = 0; i < 8; ++i) bufs[i] = storage.data() + i * kDiffSz;
        Late* l = new Late();
        l->Init(bufs.data(), kDiffSz, kDiffSz, kFS);
        l->SnapParams(p, kFS);
        return std::make_pair(l, storage);  // storage owns memory
    };

    // Can't easily return two things; run each leg separately.
    Params p_mod;   p_mod.mod_ms = 1.0f;  p_mod.mod_hz = 10.f;
    Params p_nomod; p_nomod.mod_ms = 0.f; p_nomod.mod_hz = 0.f;

    std::vector<float> storMod(8 * kDiffSz, 0.f), storNo(8 * kDiffSz, 0.f);
    float* bMod[8], *bNo[8];
    for (int i = 0; i < 8; ++i) { bMod[i] = storMod.data() + i*kDiffSz;
                                    bNo[i]  = storNo.data()  + i*kDiffSz; }

    Late lateMod, lateNo;
    lateMod.Init(bMod, kDiffSz, kDiffSz, kFS);  lateMod.SnapParams(p_mod,   kFS);
    lateNo .Init(bNo,  kDiffSz, kDiffSz, kFS);  lateNo .SnapParams(p_nomod, kFS);

    float oL, oR;
    // Shared impulse to settle both
    lateMod.Process(1.f, 1.f, oL, oR);
    lateNo .Process(1.f, 1.f, oL, oR);
    for (size_t n = 1; n < kSettle; ++n) {
        lateMod.Process(0.f, 0.f, oL, oR);
        lateNo .Process(0.f, 0.f, oL, oR);
    }

    double totalDiff = 0.0;
    for (size_t n = 0; n < kMeas; ++n) {
        float mL, mR, nL, nR;
        lateMod.Process(0.f, 0.f, mL, mR);
        lateNo .Process(0.f, 0.f, nL, nR);
        totalDiff += std::abs(double(mL - nL)) + std::abs(double(mR - nR));
    }
    CHECK(totalDiff > 1e-6);  // modulated output must differ from unmodulated
}

TEST_CASE("Late: no NaN/Inf after 30-second white-noise soak at fb_gain=0.995 clamp") {
    // Stage 8: stress-test at maximum fb_gain. rt60=60s, bloom=3.14 pushes
    // computed fb_gain to 0.9965, which the clamp holds at 0.995.
    constexpr float  kFS     = 48000.f;
    constexpr size_t kDiffSz = 4096;
    constexpr size_t N       = size_t(30.f * kFS);

    Params p;
    p.rt60_s = 60.f;    // → fb_gain hits 0.995 clamp
    p.bloom  = 3.14f;

    std::vector<float> storage(8 * kDiffSz, 0.f);
    float* bufs[8];
    for (int i = 0; i < 8; ++i) bufs[i] = storage.data() + i * kDiffSz;

    Late late;
    late.Init(bufs, kDiffSz, kDiffSz, kFS);
    late.SnapParams(p, kFS);

    uint32_t rng = 0xCAFEBABE;
    auto rand11 = [&]() -> float {
        rng = rng * 1664525u + 1013904223u;
        return float(int32_t(rng)) / float(0x80000000u);
    };

    float outL, outR, peak = 0.f;
    bool any_bad = false;
    for (size_t n = 0; n < N; ++n) {
        late.Process(rand11(), rand11(), outL, outR);
        if (!std::isfinite(outL) || !std::isfinite(outR)) { any_bad = true; break; }
        peak = std::max(peak, std::max(std::abs(outL), std::abs(outR)));
    }
    CHECK(!any_bad);
    CHECK(peak < 100.f);   // bounded, not runaway
}
