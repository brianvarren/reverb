#pragma once
#include "allpass.h"
#include "delay.h"
#include "params.h"
#include "rotation.h"
#include "shelf.h"
#include "util.h"
#include <cmath>

// First-order DC-blocking highpass.  y[n] = g·(x[n] - x[n-1]) + R*y[n-1]
// R=0.9995 ≈ 7 Hz highpass — gentle enough that bass blooms into the tail
// rather than being progressively stripped over many loop passes.
// g = (1+R)/2 normalises the Nyquist gain to exactly 1. Without it the blocker
// has +0.002 dB of gain at HF — harmless once, but it sits inside the loop.
struct DcBlock {
    float x1 = 0.f, y1 = 0.f;
    float Process(float x) {
        constexpr float kR = 0.9995f, kG = 0.5f * (1.f + kR);
        float y = kG * (x - x1) + kR * y1;
        x1 = x; y1 = y;
        return y;
    }
};

// Late recirculating network: cross-coupled, 4 rotation stages, long delay,
// plus a modulated injection delay on the early→loop path.
//
// Signal flow per sample:
//   eL/eR → injection delay (LFO-modulated) ─┐
//   [aL, aR] = [injL + fbR_(z⁻¹), injR + fbL_(z⁻¹)]   cross-coupled sum
//   × 3 stages: Rotate → Read/Write diffuser delay → LoopShelf
//   → Rotate (4th) → LoopShelf (chain) → Read/Write long delay → DCBlock → ×fb_gain → fbL_, fbR_
//
// Caller-supplied buffers (all zeroed, all power-of-two):
//   bufs[0..2]  diffuser L stages 0,1,2  (diff_sz)
//   bufs[3..5]  diffuser R stages 0,1,2
//   bufs[6]     long delay L              (long_sz)
//   bufs[7]     long delay R
//   bufs[8]     injection delay L         (inj_sz)
//   bufs[9]     injection delay R
// Diffuser pitch offsets above the long delay (semitones).
// Deliberately NOT whole semitones: equal-tempered intervals are by design the
// closest approximations to small-integer ratios (M3 ≈ 5/4, m3 ≈ 6/5, P4 ≈ 4/3),
// so delays tuned to them make echo trains nearly realign → pitched/metallic
// colour. These values (and lt_sym) come from a search that minimises the
// worst-case closeness of any delay pair, L and R included, to p/q with q ≤ 8.
// Re-run tools/ratio_search.py if you change any of them or lt_sym.
static constexpr float kLateOffs[3] = { 8.65f, 6.42f, 2.20f };

// Output tap positions (fraction of the *other* channel's long delay) and the
// diffuser stage tapped. See Late::Process for why taps never share a line.
static constexpr float kTapLongFrac = 0.41f;
static constexpr float kTapDiffFrac = 0.73f;
static constexpr int   kTapDiffStage = 1;

// Per-pass loop gain ceiling. Every in-loop element is now ≤ 0 dB (no makeup
// gain, normalised DC blocker), so this is a hard backstop, not the thing that
// sets long decays. 0.9995 allows ~60 s even at the smallest Size.
static constexpr float kFbCap = 0.9995f;

class Late {
public:
    void Init(float* bufs[10], size_t diff_sz, size_t long_sz, size_t inj_sz,
              float fs, size_t block_sz = 48) {
        float blk_dt = float(block_sz) / fs;
        block_sz_ = float(block_sz);
        for (int k = 0; k < 3; ++k) {
            dlL_[k].Init(bufs[k],     diff_sz, fs, block_sz);
            dlR_[k].Init(bufs[k + 3], diff_sz, fs, block_sz);
            // Match glide rate of the long delay so all taps sweep together.
            dlL_[k].SetMaxStep(50.f);
            dlR_[k].SetMaxStep(50.f);
        }
        longL_.Init(bufs[6], long_sz);
        longR_.Init(bufs[7], long_sz);
        injL_ .Init(bufs[8], inj_sz);
        injR_ .Init(bufs[9], inj_sz);
        for (int k = 0; k < 4; ++k) {
            smL_[k].SetTC(0.050f, blk_dt);
            smR_[k].SetTC(0.050f, blk_dt);
        }
        smL_[3].SetMaxStep(50.f);
        smR_[3].SetMaxStep(50.f);
        smInj_[0].SetTC(0.050f, blk_dt);
        smInj_[1].SetTC(0.050f, blk_dt);
        // Chaos oscillator rates: 0.01 Hz × φ^(k/2), fixed golden-ratio spread.
        static const float kChaosHz[10] = {
            0.01000f, 0.01272f, 0.01618f, 0.02058f,
            0.02618f, 0.03330f, 0.04236f, 0.05389f,
            0.06854f, 0.08724f
        };
        for (int k = 0; k < 10; ++k)
            chaos_inc_[k] = kChaosHz[k] / fs;
    }

    void SnapParams(const Params& p, float fs) {
        rot_.SetAngle(p.lt_theta);
        _compute_targets(p, fs);
        for (int k = 0; k < 3; ++k) {
            dlL_[k].SnapDelay(tgtL_[k]);  dL_[k] = tgtL_[k];
            dlR_[k].SnapDelay(tgtR_[k]);  dR_[k] = tgtR_[k];
        }
        smL_[3].Reset(tgtL_[3]);  dL_[3] = tgtL_[3];
        smR_[3].Reset(tgtR_[3]);  dR_[3] = tgtR_[3];
        _update_fb(fs, p.rt60_s);
        _update_shelves(p, fs);
        _update_lfo(p, fs);
        smInj_[0].Reset(inj_base_); d_inj_[0] = inj_base_;
        smInj_[1].Reset(inj_base_); d_inj_[1] = inj_base_;
    }

    void UpdateBlock(const Params& p, float fs) {
        rot_.SetAngle(p.lt_theta);
        _compute_targets(p, fs);
        for (int k = 0; k < 3; ++k) {
            dlL_[k].AdvanceDelay(tgtL_[k]);  dL_[k] = dlL_[k].current_delay();
            dlR_[k].AdvanceDelay(tgtR_[k]);  dR_[k] = dlR_[k].current_delay();
        }
        {
            float prev_dL3 = dL_[3], prev_dR3 = dR_[3];
            dL_[3] = smL_[3].Process(tgtL_[3]);
            dR_[3] = smR_[3].Process(tgtR_[3]);
            dL3_inc_ = (dL_[3] - prev_dL3) / block_sz_;
            dR3_inc_ = (dR_[3] - prev_dR3) / block_sz_;
            dL_[3] = prev_dL3;
            dR_[3] = prev_dR3;
        }
        _update_fb(fs, p.rt60_s);
        _update_shelves(p, fs);
        _update_lfo(p, fs);
        d_inj_[0] = smInj_[0].Process(inj_base_);
        d_inj_[1] = smInj_[1].Process(inj_base_);
        // Compute per-sample mod increments so Process() interpolates smoothly
        // across the block rather than holding frozen values (staircase artifact).
        float mod_start[10];
        for (int k = 0; k < 10; ++k) mod_start[k] = mod_[k];
        _step_lfo(block_sz_);
        for (int k = 0; k < 10; ++k) {
            mod_inc_[k] = (mod_[k] - mod_start[k]) / block_sz_;
            mod_[k]     = mod_start[k];
        }
        block_lfo_ = true;
    }

    void Process(float eL, float eR, float& outL, float& outR) {
        if (!block_lfo_) {
            // Host per-sample path: advance LFO every sample, no interpolation needed.
            _step_lfo(1.f);
        } else {
            // Block path: advance mod and long delay by precomputed per-sample increments.
            for (int k = 0; k < 10; ++k) mod_[k] += mod_inc_[k];
            dL_[3] += dL3_inc_;
            dR_[3] += dR3_inc_;
        }

        // Modulated injection: LFO-modulated short delay before the cross-coupled sum.
        float iL = injL_.Read4pt(d_inj_[0] + mod_[8]);  injL_.Write(eL);
        float iR = injR_.Read4pt(d_inj_[1] + mod_[9]);  injR_.Write(eR);

        float aL = iL + fbR_;
        float aR = iR + fbL_;

        for (int k = 0; k < 3; ++k) {
            rot_.Process(aL, aR);
            float ra = dlL_[k].Process(aL, mod_[k]);
            float rb = dlR_[k].Process(aR, mod_[k + 4]);
            aL = shelfL_[k].Process(ra);
            aR = shelfR_[k].Process(rb);
        }
        rot_.Process(aL, aR);  // 4th rotation before chain shelf
        aL = chainL_.Process(aL);
        aR = chainR_.Process(aR);
        aL = _sat(aL);  // harmonic generation before long delay
        aR = _sat(aR);

        float rawL = longL_.Read4pt(dL_[3] + mod_[3]);
        float rawR = longR_.Read4pt(dR_[3] + mod_[7]);

        // Output taps. Rule: an output channel never reads the same delay line
        // twice. Two taps of one line summed at a fixed spacing Δ are an FIR
        // comb (notches every 1/Δ Hz) applied to the whole tail — that was the
        // old 0.382/0.618/1.0 golden tap set, and the dominant metallic colour.
        // Instead: own long line + the other channel's long line + one of the
        // other channel's diffusers, with alternating signs (cancels residual
        // common-mode correlation) and equal-power normalisation.
        // Taps are read before the writes below, so the read positions are the
        // same convention as rawL/rawR.
        constexpr float kN = 0.57735027f;  // 1/√3: three ~decorrelated taps
        const int ks = kTapDiffStage;
        outL = kN * (rawL
                     - longR_.Read4pt(dR_[3] * kTapLongFrac + mod_[7])
                     + dlR_[ks].Tap(dR_[ks] * kTapDiffFrac + mod_[ks + 4]));
        outR = kN * (rawR
                     - longL_.Read4pt(dL_[3] * kTapLongFrac + mod_[3])
                     + dlL_[ks].Tap(dL_[ks] * kTapDiffFrac + mod_[ks]));
        longL_.Write(aL);
        longR_.Write(aR);

        fbL_ = dcL_.Process(rawL) * fb_gain_;
        fbR_ = dcR_.Process(rawR) * fb_gain_;
    }

    float fb_gain()              const { return fb_gain_; }
    float loop_length_samples()  const {
        float D = 0.f;
        for (int k = 0; k < 4; ++k) D += 0.5f * (dL_[k] + dR_[k]);
        return D;
    }

private:
    void _compute_targets(const Params& p, float fs) {
        float szL = p.lt_sz - p.lt_sym;
        float szR = p.lt_sz + p.lt_sym;
        for (int k = 0; k < 3; ++k) {
            tgtL_[k] = pitch_to_samples(szL + kLateOffs[k], fs);
            tgtR_[k] = pitch_to_samples(szR + kLateOffs[k], fs);
        }
        tgtL_[3] = pitch_to_samples(szL, fs);
        tgtR_[3] = pitch_to_samples(szR, fs);
    }

    // Frequency-dependent decay. The 4 in-loop shelves (3 stage + chain) set the
    // HF decay time as a *ratio* of the broadband T60, so damping stays musically
    // consistent across every Size and Decay instead of being a fixed dB/pass
    // (a fixed dB/pass means big rooms barely damp and small rooms go dull).
    //   per-pass HF gain wanted  = 10^(−3·D / (fs·T60_hf))
    //   per-pass broadband gain  = fb_gain_            (already applied)
    //   → total shelf gain at HF = wanted / fb_gain_,  split across 4 shelves.
    // T60_hf is also capped at lt_hf_max_s so very long tails still darken.
    void _update_shelves(const Params& p, float fs) {
        const float D      = loop_length_samples();
        const float t60_hf = fminf(p.rt60_s * p.lt_hf_ratio, p.lt_hf_max_s);
        float hb = 1.f;
        if (t60_hf < p.rt60_s && D > 0.f) {
            const float g_hf  = powf(10.f, -3.f * (D / fs) / t60_hf);
            const float ratio = fminf(g_hf / fmaxf(fb_gain_, 1e-6f), 1.f);
            hb = powf(ratio, 0.25f);
        }
        float hf = p.lt_hf_hz;
        float lf = 20.f;
        float lb = 1.f;
        for (int k = 0; k < 3; ++k) {
            // Allpass internal shelf is flat — EQ applied externally by shelfL_/R_.
            dlL_[k].SetGain(p.lt_dffs);
            dlL_[k].SetShelf(hf, 1.0f, lf, 1.0f, fs);
            dlR_[k].SetGain(p.lt_dffs);
            dlR_[k].SetShelf(hf, 1.0f, lf, 1.0f, fs);
            shelfL_[k].SetParams(hf, hb, lf, lb, fs);
            shelfR_[k].SetParams(hf, hb, lf, lb, fs);
        }
        chainL_.SetParams(hf, hb, lf, lb, fs);
        chainR_.SetParams(hf, hb, lf, lb, fs);
    }

    void _update_fb(float fs, float rt60_s) {
        float D = loop_length_samples();
        fb_gain_ = powf(10.f, -3.f * (D / fs) / rt60_s);
        fb_gain_ = fminf(fb_gain_, kFbCap);
        // No makeup gain on the saturator. tanh(d·x)/d already has slope 1 at
        // the origin, so it is transparent at small signals and only squashes
        // peaks. The old makeup (>1) raised the small-signal loop gain to ~0.999
        // whatever rt60 asked for, which roughly doubled every long tail and
        // made the Decay knob dead above ~0.25.
    }

    void _update_lfo(const Params& p, float fs) {
        chaos_     = p.chaos;
        mod_depth_ = p.mod_ms * 0.001f * fs;
        inj_base_  = mod_depth_ + 1.f;  // injection base: center depth + 1 sample guard
        drive_     = 1.f + p.sat * 4.f;   // range [1, 5]; unity small-signal gain
        // LFO rates spread by φ^(k/7) at chaos=1, all identical at chaos=0.
        // Slots 0..3 = L diffusers + long; 4..7 = R counterparts; 8..9 = injection L/R.
        static const float kLfoRatios[10] = {
            1.0000f, 1.0712f, 1.1474f, 1.2289f,
            1.3163f, 1.4100f, 1.5103f, 1.6180f,
            1.7330f, 1.8570f
        };
        for (int k = 0; k < 10; ++k) {
            float ratio = 1.f + p.chaos * (kLfoRatios[k] - 1.f);
            lfo_inc_[k] = p.mod_hz * ratio / fs;
        }
    }

    float _sat(float x) const {
        if (drive_ <= 1.001f) return x;
        return tanhf(drive_ * x) / drive_;
    }

    // Advances 8 independent LFOs and 8 slow chaos oscillators.
    // mod_[0..3] = L diffusers + long delay, mod_[4..7] = R counterparts.
    // `stride` is samples per call (1 on the per-sample path, block_sz on
    // the block-rate path — keeps both paths phase-accurate).
    void _step_lfo(float stride) {
        const float kTwoPi    = 2.f * float(M_PI);
        const float kChaosAmt = 0.6f;  // max ±60 % depth variance per tap
        for (int k = 0; k < 10; ++k) {
            lfo_phase_[k] += lfo_inc_[k] * stride;
            if (lfo_phase_[k] >= 1.f) lfo_phase_[k] -= 1.f;
            chaos_phase_[k] += chaos_inc_[k] * stride;
            if (chaos_phase_[k] >= 1.f) chaos_phase_[k] -= 1.f;
            float depth = mod_depth_ * fmaxf(0.f, 1.f + chaos_ * kChaosAmt
                                                 * sinf(kTwoPi * chaos_phase_[k]));
            mod_[k] = depth * sinf(kTwoPi * lfo_phase_[k]);
        }
    }

    AllPass      dlL_[3], dlR_[3];
    DelayLine    longL_, longR_;
    DelayLine    injL_, injR_;
    LoopShelf    shelfL_[3], shelfR_[3];
    LoopShelf    chainL_, chainR_;
    Rotation     rot_;
    Smoother     smL_[4], smR_[4];
    Smoother     smInj_[2];
    float        dL_[4] = {}, dR_[4] = {};
    float        tgtL_[4] = {}, tgtR_[4] = {};
    float        dL3_inc_ = 0.f, dR3_inc_ = 0.f;
    float        d_inj_[2] = {1.f, 1.f};
    DcBlock      dcL_, dcR_;
    float        fbL_ = 0.f, fbR_ = 0.f;
    float        fb_gain_ = 0.5f;
    float        lfo_phase_[10]   = {};
    float        lfo_inc_[10]     = {};
    float        chaos_phase_[10] = {};
    float        chaos_inc_[10]   = {};
    float        chaos_           = 0.f;
    float        mod_depth_       = 0.f;
    float        inj_base_        = 1.f;
    float        drive_           = 1.f;
    float        block_sz_   = 48.f;
    float        mod_[10]     = {};
    float        mod_inc_[10] = {};
    bool         block_lfo_   = false;
};
