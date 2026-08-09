#pragma once
#include "allpass.h"
#include "delay.h"
#include "params.h"
#include "rotation.h"
#include "shelf.h"
#include "util.h"
#include <cmath>

// First-order DC-blocking highpass.  y[n] = x[n] - x[n-1] + 0.995*y[n-1]
struct DcBlock {
    float x1 = 0.f, y1 = 0.f;
    float Process(float x) {
        float y = x - x1 + 0.995f * y1;
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
class Late {
public:
    void Init(float* bufs[10], size_t diff_sz, size_t long_sz, size_t inj_sz,
              float fs, size_t block_sz = 48) {
        float blk_dt = float(block_sz) / fs;
        block_sz_ = float(block_sz);
        for (int k = 0; k < 3; ++k) {
            dlL_[k].Init(bufs[k],     diff_sz, fs, block_sz);
            dlR_[k].Init(bufs[k + 3], diff_sz, fs, block_sz);
        }
        longL_.Init(bufs[6], long_sz);
        longR_.Init(bufs[7], long_sz);
        injL_ .Init(bufs[8], inj_sz);
        injR_ .Init(bufs[9], inj_sz);
        for (int k = 0; k < 4; ++k) {
            smL_[k].SetTC(0.050f, blk_dt);
            smR_[k].SetTC(0.050f, blk_dt);
        }
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
        _update_shelves(p, fs);
        _update_fb(fs, p.rt60_s, p.bloom, p.sat);
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
        dL_[3] = smL_[3].Process(tgtL_[3]);
        dR_[3] = smR_[3].Process(tgtR_[3]);
        _update_shelves(p, fs);
        _update_fb(fs, p.rt60_s, p.bloom, p.sat);
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
            // Block path: advance mod by precomputed per-sample increment.
            for (int k = 0; k < 10; ++k) mod_[k] += mod_inc_[k];
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

        float rawL = longL_.Read4pt(dL_[3] + mod_[3]);  longL_.Write(aL);
        float rawR = longR_.Read4pt(dR_[3] + mod_[7]);  longR_.Write(aR);
        outL = (rawL + longL_.Read4pt(dL_[3] * 0.382f + mod_[3])
                     + longL_.Read4pt(dL_[3] * 0.618f + mod_[3])) * (1.f/3.f);
        outR = (rawR + longR_.Read4pt(dR_[3] * 0.382f + mod_[7])
                     + longR_.Read4pt(dR_[3] * 0.618f + mod_[7])) * (1.f/3.f);

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
            float off = float(9 - 3 * k);  // +9, +6, +3
            tgtL_[k] = pitch_to_samples(szL + off, fs);
            tgtR_[k] = pitch_to_samples(szR + off, fs);
        }
        tgtL_[3] = pitch_to_samples(szL, fs);
        tgtR_[3] = pitch_to_samples(szR, fs);
    }

    void _update_shelves(const Params& p, float fs) {
        float hf = p.hp_hz;
        float hb = db_to_lin(p.hp_db);
        float lf = p.lp_hz;
        float lb = db_to_lin(p.lp_db);
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

    void _update_fb(float fs, float rt60_s, float bloom, float sat) {
        float D = loop_length_samples();
        fb_gain_ = powf(10.f, -3.f * (D / fs) / (rt60_s * bloom));
        fb_gain_ = fminf(fb_gain_, 0.995f);
        // Makeup gain: compensate tanh gain reduction at a reference level of 0.1.
        // Ideal = ref * drive / tanh(drive * ref); clamped so small-signal loop gain < 1.
        // (tanh slope = 1 at origin, so total gain = fb_gain * makeup must stay < 1.)
        if (sat > 0.001f) {
            const float kRef = 0.1f;
            float drive = 1.f + sat * 4.f;
            float ideal = kRef * drive / tanhf(drive * kRef);
            makeup_  = fminf(ideal, 0.999f / fb_gain_);
        } else {
            makeup_ = 1.f;
        }
    }

    void _update_lfo(const Params& p, float fs) {
        chaos_     = p.chaos;
        mod_depth_ = p.mod_ms * 0.001f * fs;
        inj_base_  = mod_depth_ + 1.f;  // injection base: center depth + 1 sample guard
        drive_     = 1.f + p.sat * 4.f;   // range [1, 5]; makeup compensates gain loss
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
        return tanhf(drive_ * x) / drive_ * makeup_;
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
    float        makeup_          = 1.f;
    float        block_sz_   = 48.f;
    float        mod_[10]     = {};
    float        mod_inc_[10] = {};
    bool         block_lfo_   = false;
};
