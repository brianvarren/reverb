#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#include "params.h"
#include "reverb.h"
#include "testgen.h"

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Realtime engine.
//
// Threading contract: the UI thread only ever touches std::atomic members.
// The audio thread owns the Reverb, the delay buffers, and the generator.
// No locks, no allocation, no syscalls in Render().
//
// Parameters cross the boundary as an array of std::atomic<float>. The audio
// thread snapshots all of them into a plain Params once per 48-frame control
// block. A snapshot can straddle a UI edit, which means two parameters may
// briefly disagree by one block — harmless, and far cheaper than a seqlock.
//
// Render() drives the reverb through the *firmware* path (UpdateBlock once per
// 48 frames, then Process per sample), so what you tune here is what the Daisy
// will do — including the control-rate granularity of the smoothers.
// ─────────────────────────────────────────────────────────────────────────────

class Engine {
public:
    static constexpr size_t kBlock     = 48;       // control block, matches firmware
    static constexpr size_t kEarlyBuf  = 16384;   // 341 ms predelay ceiling
    static constexpr size_t kDiffBuf   = 8192;
    static constexpr size_t kLongBuf   = 16384;
    static constexpr int    kHistLen   = 240;     // tail graph, one cell per hop
    static constexpr int    kHistHop   = 20;      // blocks per cell → 20 ms/cell

    void Init(float fs, const Params& p) {
        fs_ = fs;
        for (int i = 0; i < kParamCount; ++i)
            ap_[i].store(*ParamSlot(const_cast<Params&>(p), i), std::memory_order_relaxed);
        gen_.Init(fs);
        _rebuild();
        for (int i = 0; i < kHistLen; ++i) hist_[i].store(-120.f, std::memory_order_relaxed);
    }

    // ── UI thread ────────────────────────────────────────────────────────────

    void  SetParam(int i, float v) { ap_[i].store(v, std::memory_order_relaxed); }
    float GetParam(int i) const    { return ap_[i].load(std::memory_order_relaxed); }

    void SetSource(int type, float pitch, float len_ms, float level_db) {
        src_type_ .store(type,      std::memory_order_relaxed);
        src_pitch_.store(pitch,     std::memory_order_relaxed);
        src_len_  .store(len_ms,    std::memory_order_relaxed);
        src_lvl_  .store(level_db,  std::memory_order_relaxed);
    }
    void SetRepeat(bool on, float ms) {
        rpt_on_.store(on, std::memory_order_relaxed);
        rpt_ms_.store(ms, std::memory_order_relaxed);
    }
    void SetOutGain(float db)  { out_gain_.store(db_to_lin(db), std::memory_order_relaxed); }
    void SetSafety(bool on)    { safety_.store(on, std::memory_order_relaxed); }
    void SetMute(bool on)      { mute_.store(on, std::memory_order_relaxed); }
    void SetBypass(bool on)    { bypass_.store(on, std::memory_order_relaxed); }
    void SetFileOn(bool on)    { file_on_.store(on, std::memory_order_relaxed); }

    // Fire the current source. Set the source config first; it is latched here.
    void Trigger()             { trig_.fetch_add(1, std::memory_order_release); }
    // Zero every delay line and filter state. Also fired automatically on NaN.
    void Panic()               { clear_.store(true, std::memory_order_release); }

    float peak_db()  const { return _db(peak_.load(std::memory_order_relaxed)); }
    float rms_db()   const { return _db(rms_ .load(std::memory_order_relaxed)); }
    float t60()      const { return t60_.load(std::memory_order_relaxed); }
    float edt()      const { return edt_.load(std::memory_order_relaxed); }
    int   blowups()  const { return blowups_.load(std::memory_order_relaxed); }
    // Exponential moving average of (render time / frame budget). >1.0 = overrun.
    float cpu_load() const { return cpu_load_.load(std::memory_order_relaxed); }
    int   overruns() const { return overruns_.load(std::memory_order_relaxed); }
    // 0..1 while a decay capture is running, −1 when idle.
    float capture_progress() const {
        const int  st = cap_state_.load(std::memory_order_relaxed);
        const long wn = cap_want_;
        if (st == 0 || wn <= 0) return -1.f;
        return (st == 1) ? 0.f
                         : float(cap_n_.load(std::memory_order_relaxed)) / float(wn);
    }

    // ── decay analysis (UI thread) ───────────────────────────────────────────
    // Schroeder backward integration of the captured decay. Call once per UI
    // frame; it does nothing until the audio thread has finished a capture.
    //
    // Backward integration is the honest way to get a decay time from a single
    // shot: integrating the energy from the end backwards removes the ripple
    // that makes a raw envelope unmeasurable, and the result is the curve the
    // ear actually integrates. T60 comes from a least-squares fit over the
    // -5 to -35 dB span (the T30 convention, doubled); EDT from 0 to -10 dB,
    // sextupled. EDT well above T60 is exactly what "bloom" means numerically:
    // the tail is still building while the total energy has already peaked.
    void AnalyseDecay() {
        if (!cap_ready_.load(std::memory_order_acquire)) return;
        const int n = cap_len_;
        if (n < 64) { cap_ready_.store(false, std::memory_order_release); return; }

        double sum = 0.0;
        std::vector<double>& edc = edc_;
        edc.resize(n);
        for (int i = n - 1; i >= 0; --i) { sum += cap_[i]; edc[i] = sum; }

        const double ref = edc[0];
        if (ref <= 0.0) { cap_ready_.store(false, std::memory_order_release); return; }

        const double dt = double(kBlock) / double(fs_);
        auto db_at = [&](int i) { return 10.0 * log10((edc[i] / ref) + 1e-30); };

        auto fit = [&](double d0, double d1) -> float {
            int i0 = -1, i1 = -1;
            for (int i = 0; i < n; ++i) {
                const double d = db_at(i);
                if (i0 < 0 && d <= d0) i0 = i;
                if (i0 >= 0 && d <= d1) { i1 = i; break; }
            }
            if (i0 < 0 || i1 <= i0 + 4) return -1.f;
            double sx=0, sy=0, sxx=0, sxy=0; int m=0;
            for (int i = i0; i <= i1; ++i) {
                const double x = i * dt, y = db_at(i);
                sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; ++m;
            }
            const double den = m*sxx - sx*sx;
            if (den == 0.0) return -1.f;
            const double slope = (m*sxy - sx*sy) / den;      // dB per second
            if (slope >= -0.1) return -1.f;
            return float(-60.0 / slope);
        };

        t60_.store(fit(-5.0, -35.0), std::memory_order_relaxed);
        edt_.store(fit( 0.0, -10.0), std::memory_order_relaxed);
        cap_ready_.store(false, std::memory_order_release);
    }

    // Newest-last copy of the tail history, in dBFS.
    void CopyHistory(float* dst) const {
        const int w = hist_w_.load(std::memory_order_acquire);
        for (int i = 0; i < kHistLen; ++i)
            dst[i] = hist_[(w + i) % kHistLen].load(std::memory_order_relaxed);
    }

    void LoadFile(std::vector<float> stereo) { file_ = std::move(stereo); file_pos_ = 0; }
    bool has_file() const { return !file_.empty(); }

    // ── audio thread ─────────────────────────────────────────────────────────

    void Render(float* out, size_t frames) {
        using clk = std::chrono::steady_clock;
        const auto t0 = clk::now();
        _denormals_off();

        size_t done = 0;
        while (done < frames) {
            const size_t n = (frames - done < kBlock) ? (frames - done) : kBlock;

            if (clear_.exchange(false, std::memory_order_acquire)) _rebuild();

            // Snapshot params and run the control-rate update.
            Params p;
            for (int i = 0; i < kParamCount; ++i)
                *ParamSlot(p, i) = ap_[i].load(std::memory_order_relaxed);
            reverb_.UpdateBlock(p);

            const uint32_t t = trig_.load(std::memory_order_acquire);
            if (t != trig_seen_) { trig_seen_ = t; _fire(); }

            if (rpt_on_.load(std::memory_order_relaxed)) {
                rpt_count_ += (long)n;
                const long period = (long)(rpt_ms_.load(std::memory_order_relaxed) * 0.001f * fs_);
                if (period > 0 && rpt_count_ >= period) { rpt_count_ = 0; _fire(); }
            } else {
                rpt_count_ = 0;
            }

            const bool  byp  = bypass_  .load(std::memory_order_relaxed);
            const bool  mute = mute_    .load(std::memory_order_relaxed);
            const bool  safe = safety_  .load(std::memory_order_relaxed);
            const float g    = out_gain_.load(std::memory_order_relaxed);
            const bool  fon  = file_on_ .load(std::memory_order_relaxed) && !file_.empty();
            const float flvl = db_to_lin(src_lvl_.load(std::memory_order_relaxed));

            float blk_peak = 0.f, blk_sum2 = 0.f;
            bool  bad = false;

            for (size_t i = 0; i < n; ++i) {
                float s = gen_.Process();
                float sL = s, sR = s;
                if (fon) {
                    sL += file_[file_pos_ * 2 + 0] * flvl;
                    sR += file_[file_pos_ * 2 + 1] * flvl;
                    if (++file_pos_ >= file_.size() / 2) file_pos_ = 0;
                }

                float oL, oR;
                if (byp) { oL = sL; oR = sR; }
                else     { reverb_.Process(sL, sR, oL, oR); }

                if (!std::isfinite(oL) || !std::isfinite(oR)) { bad = true; oL = oR = 0.f; }

                oL *= g; oR *= g;
                if (safe) { oL = _softclip(oL); oR = _softclip(oR); }
                if (mute) { oL = 0.f; oR = 0.f; }

                out[(done + i) * 2 + 0] = oL;
                out[(done + i) * 2 + 1] = oR;

                const float aL = fabsf(oL), aR = fabsf(oR);
                if (aL > blk_peak) blk_peak = aL;
                if (aR > blk_peak) blk_peak = aR;
                blk_sum2 += oL * oL + oR * oR;
            }

            if (bad) { ++blowups_local_; blowups_.store(blowups_local_, std::memory_order_relaxed);
                       clear_.store(true, std::memory_order_release); }

            const float blk_rms = sqrtf(blk_sum2 / float(2 * (n ? n : 1)));
            _meters(blk_peak, blk_rms);

            done += n;
        }
        const float elapsed = std::chrono::duration<float>(clk::now() - t0).count();
        const float budget  = float(frames) / fs_;
        const float load    = elapsed / budget;
        const float prev    = cpu_load_.load(std::memory_order_relaxed);
        cpu_load_.store(prev + 0.1f * (load - prev), std::memory_order_relaxed);
        if (load >= 1.f) overruns_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    // ── helpers ──────────────────────────────────────────────────────────────

    void _fire() {
        gen_.Trigger(src_type_ .load(std::memory_order_relaxed),
                     src_pitch_.load(std::memory_order_relaxed),
                     src_len_  .load(std::memory_order_relaxed),
                     src_lvl_  .load(std::memory_order_relaxed));
        // Arm the decay capture. It starts for real once the excitation has
        // finished, so the measurement describes the reverb and not the burst.
        if (!cap_ready_.load(std::memory_order_acquire)) {
            const float rt = ap_[kP_rt60_s].load(std::memory_order_relaxed);
            const float bl = ap_[kP_bloom ].load(std::memory_order_relaxed);
            long want = long((rt * bl * 1.3f + 0.5f) * fs_ / float(kBlock));
            if (want > kCapMax) want = kCapMax;
            if (want < 256)     want = 256;
            cap_want_ = want;
            cap_n_.store(0, std::memory_order_relaxed);
            cap_state_.store(1, std::memory_order_relaxed);   // wait for burst end
        }
    }

    // Full reset: zero every sample of delay memory and reconstruct the DSP so
    // filter states, DC blockers and feedback registers all start from silence.
    void _rebuild() {
        memset(early_, 0, sizeof(early_));
        memset(diff_,  0, sizeof(diff_));
        memset(long_,  0, sizeof(long_));

        float* bufs[16];
        for (int i = 0; i < 8; ++i) bufs[i]     = early_[i];
        for (int i = 0; i < 6; ++i) bufs[8 + i] = diff_[i];
        bufs[14] = long_[0];
        bufs[15] = long_[1];

        reverb_.~Reverb();
        new (&reverb_) Reverb();
        reverb_.Init(bufs, kEarlyBuf, kDiffBuf, kLongBuf, fs_, kBlock);

        Params p;
        for (int i = 0; i < kParamCount; ++i)
            *ParamSlot(p, i) = ap_[i].load(std::memory_order_relaxed);
        reverb_.SnapParams(p);

        gen_.Stop();
    }

    void _meters(float blk_peak, float blk_rms) {
        // Peak: instant attack, ~1.5 s release so you can read it.
        float pk = peak_.load(std::memory_order_relaxed);
        pk = (blk_peak > pk) ? blk_peak : pk * 0.9993f;
        peak_.store(pk, std::memory_order_relaxed);

        float rm = rms_.load(std::memory_order_relaxed);
        rm += 0.05f * (blk_rms - rm);
        rms_.store(rm, std::memory_order_relaxed);

        // Tail history: one cell every kHistHop blocks (~20 ms).
        hop_peak_ = (blk_rms > hop_peak_) ? blk_rms : hop_peak_;
        if (++hop_n_ >= kHistHop) {
            hop_n_ = 0;
            const int w = hist_w_.load(std::memory_order_relaxed);
            hist_[w].store(_db(hop_peak_), std::memory_order_relaxed);
            hist_w_.store((w + 1) % kHistLen, std::memory_order_release);
            hop_peak_ = 0.f;
        }

        // Decay capture: block-rate energy, started once the burst has ended.
        const int cst = cap_state_.load(std::memory_order_relaxed);
        if (cst == 1 && !gen_.active()) {
            cap_state_.store(2, std::memory_order_relaxed);
            cap_n_.store(0, std::memory_order_relaxed);
        } else if (cst == 2) {
            long k = cap_n_.load(std::memory_order_relaxed);
            if (k < cap_want_) { cap_[k++] = blk_rms * blk_rms;
                                 cap_n_.store(k, std::memory_order_relaxed); }
            if (k >= cap_want_) {
                cap_len_ = int(k);
                cap_state_.store(0, std::memory_order_relaxed);
                cap_ready_.store(true, std::memory_order_release);
            }
        }
    }

    static float _softclip(float x) {
        // Transparent below −3 dBFS, asymptotic above. Protects monitors while
        // you push fb_gain; turn it off from the SOURCE panel for honest tuning.
        const float k = 0.7f;
        if (x >  k) return  k + (1.f - k) * tanhf((x - k) / (1.f - k));
        if (x < -k) return -k - (1.f - k) * tanhf((-x - k) / (1.f - k));
        return x;
    }

    static float _db(float lin) { return (lin > 1e-9f) ? 20.f * log10f(lin) : -120.f; }

    static void _denormals_off() {
#if defined(__x86_64__) || defined(__i386__)
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
    }

    // ── state ────────────────────────────────────────────────────────────────

    float  fs_ = 48000.f;
    Reverb reverb_;
    float  early_[8][kEarlyBuf];
    float  diff_ [6][kDiffBuf];
    float  long_ [2][kLongBuf];

    TestGen gen_;
    std::vector<float> file_;      // interleaved stereo
    size_t             file_pos_ = 0;

    std::atomic<float> ap_[kParamCount];

    std::atomic<uint32_t> trig_{0};
    uint32_t              trig_seen_ = 0;
    std::atomic<bool>     clear_{false};

    std::atomic<int>   src_type_ {kGenSine};
    std::atomic<float> src_pitch_{45.f};
    std::atomic<float> src_len_  {150.f};
    std::atomic<float> src_lvl_  {-6.f};
    std::atomic<bool>  rpt_on_   {false};
    std::atomic<float> rpt_ms_   {1500.f};
    long               rpt_count_ = 0;

    std::atomic<float> out_gain_{0.5f};
    std::atomic<bool>  safety_ {true};
    std::atomic<bool>  mute_   {false};
    std::atomic<bool>  bypass_ {false};
    std::atomic<bool>  file_on_{false};

    std::atomic<float> peak_{0.f}, rms_{0.f}, t60_{-1.f}, edt_{-1.f};
    std::atomic<int>   blowups_{0};
    int                blowups_local_ = 0;
    std::atomic<float> cpu_load_{0.f};
    std::atomic<int>   overruns_{0};

    std::atomic<float> hist_[kHistLen];
    std::atomic<int>   hist_w_{0};
    float              hop_peak_ = 0.f;
    int                hop_n_    = 0;

    // Decay capture. Only the audio thread writes cap_; the UI thread reads it
    // after cap_ready_ goes true and clears the flag when finished.
    static constexpr long kCapMax = 40000;      // ~40 s at 1 ms per block
    float              cap_[kCapMax];
    std::atomic<long>  cap_n_{0};
    long               cap_want_ = 0;
    int                cap_len_  = 0;
    std::atomic<int>   cap_state_{0};
    std::atomic<bool>  cap_ready_{false};
    std::vector<double> edc_;                   // UI-thread scratch
};
