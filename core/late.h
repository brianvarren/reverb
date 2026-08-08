#pragma once
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

// Late recirculating network: cross-coupled, 3 rotation stages, long delay.
//
// Signal flow per sample (plan §5):
//   [aL, aR] = [eL + fbR_(z⁻¹), eR + fbL_(z⁻¹)]   cross-coupled sum
//   × 3 stages: Rotate → Read/Write diffuser delay → LoopShelf
//   → LoopShelf (chain) → Read/Write long delay → DCBlock → ×fb_gain → fbL_, fbR_
//
// Caller-supplied buffers (all zeroed, all power-of-two):
//   bufs[0..2]  diffuser L stages 0,1,2  (diff_sz recommended 4096)
//   bufs[3..5]  diffuser R stages 0,1,2
//   bufs[6]     long delay L            (long_sz recommended 8192)
//   bufs[7]     long delay R
class Late {
public:
    void Init(float* bufs[8], size_t diff_sz, size_t long_sz,
              float fs, size_t block_sz = 48) {
        float blk_dt = float(block_sz) / fs;
        block_sz_ = float(block_sz);
        for (int k = 0; k < 3; ++k) {
            dlL_[k].Init(bufs[k],     diff_sz);
            dlR_[k].Init(bufs[k + 3], diff_sz);
        }
        longL_.Init(bufs[6], long_sz);
        longR_.Init(bufs[7], long_sz);
        for (int k = 0; k < 4; ++k) {
            smL_[k].SetTC(0.050f, blk_dt);
            smR_[k].SetTC(0.050f, blk_dt);
        }
        // Chaos oscillator rates: 0.01 Hz × φ^(k/2), fixed golden-ratio spread.
        // Slowest completes one cycle in ~100 s; fastest in ~18 s — never aligned.
        static const float kChaosHz[8] = {
            0.01000f, 0.01272f, 0.01618f, 0.02058f,
            0.02618f, 0.03330f, 0.04236f, 0.05389f
        };
        for (int k = 0; k < 8; ++k)
            chaos_inc_[k] = kChaosHz[k] / fs;
    }

    void SnapParams(const Params& p, float fs) {
        rot_.SetAngle(p.lt_theta);
        _compute_targets(p, fs);
        for (int k = 0; k < 4; ++k) {
            smL_[k].Reset(tgtL_[k]);  dL_[k] = tgtL_[k];
            smR_[k].Reset(tgtR_[k]);  dR_[k] = tgtR_[k];
        }
        _update_shelves(p, fs);
        _update_fb(fs, p.rt60_s, p.bloom);
        _update_lfo(p, fs);
    }

    void UpdateBlock(const Params& p, float fs) {
        rot_.SetAngle(p.lt_theta);
        _compute_targets(p, fs);
        for (int k = 0; k < 4; ++k) {
            dL_[k] = smL_[k].Process(tgtL_[k]);
            dR_[k] = smR_[k].Process(tgtR_[k]);
        }
        _update_shelves(p, fs);
        _update_fb(fs, p.rt60_s, p.bloom);
        _update_lfo(p, fs);
        // Compute per-sample mod increments so Process() interpolates smoothly
        // across the block rather than holding frozen values (staircase artifact).
        float mod_start[8];
        for (int k = 0; k < 8; ++k) mod_start[k] = mod_[k];
        _step_lfo(block_sz_);
        for (int k = 0; k < 8; ++k) {
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
            for (int k = 0; k < 8; ++k) mod_[k] += mod_inc_[k];
        }

        float aL = eL + fbR_;
        float aR = eR + fbL_;

        for (int k = 0; k < 3; ++k) {
            rot_.Process(aL, aR);
            float ra = dlL_[k].Read4pt(dL_[k] + mod_[k]);      dlL_[k].Write(aL);
            float rb = dlR_[k].Read4pt(dR_[k] + mod_[k + 4]);  dlR_[k].Write(aR);
            aL = shelfL_[k].Process(ra);
            aR = shelfR_[k].Process(rb);
        }
        aL = chainL_.Process(aL);
        aR = chainR_.Process(aR);

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
        constexpr float kHpPitch = 108.f, kLpPitch = 36.f;
        float hf = pitch_to_hz(kHpPitch);
        float lf = pitch_to_hz(kLpPitch);
        float hb = db_to_lin(fminf(p.dmp_eq *  18.f,    0.f));
        float lb = db_to_lin(fminf(p.dmp_eq * -25.92f,  0.f));
        for (int k = 0; k < 3; ++k) {
            shelfL_[k].SetParams(hf, hb, lf, lb, fs);
            shelfR_[k].SetParams(hf, hb, lf, lb, fs);
        }
        chainL_.SetParams(hf, hb, lf, lb, fs);
        chainR_.SetParams(hf, hb, lf, lb, fs);
    }

    void _update_fb(float fs, float rt60_s, float bloom) {
        float D = loop_length_samples();
        fb_gain_ = powf(10.f, -3.f * (D / fs) / (rt60_s * bloom));
        fb_gain_ = fminf(fb_gain_, 0.995f);
    }

    void _update_lfo(const Params& p, float fs) {
        chaos_     = p.chaos;
        mod_depth_ = p.mod_ms * 0.001f * fs;
        // LFO rates spread by φ^(k/7) at chaos=1, all identical at chaos=0.
        static const float kLfoRatios[8] = {
            1.0000f, 1.0712f, 1.1474f, 1.2289f,
            1.3163f, 1.4100f, 1.5103f, 1.6180f
        };
        for (int k = 0; k < 8; ++k) {
            float ratio = 1.f + p.chaos * (kLfoRatios[k] - 1.f);
            lfo_inc_[k] = p.mod_hz * ratio / fs;
        }
    }

    // Advances 8 independent LFOs and 8 slow chaos oscillators.
    // mod_[0..3] = L diffusers + long delay, mod_[4..7] = R counterparts.
    // `stride` is samples per call (1 on the per-sample path, block_sz on
    // the block-rate path — keeps both paths phase-accurate).
    void _step_lfo(float stride) {
        const float kTwoPi    = 2.f * float(M_PI);
        const float kChaosAmt = 0.6f;  // max ±60 % depth variance per tap
        for (int k = 0; k < 8; ++k) {
            lfo_phase_[k] += lfo_inc_[k] * stride;
            if (lfo_phase_[k] >= 1.f) lfo_phase_[k] -= 1.f;
            chaos_phase_[k] += chaos_inc_[k] * stride;
            if (chaos_phase_[k] >= 1.f) chaos_phase_[k] -= 1.f;
            float depth = mod_depth_ * fmaxf(0.f, 1.f + chaos_ * kChaosAmt
                                                 * sinf(kTwoPi * chaos_phase_[k]));
            mod_[k] = depth * sinf(kTwoPi * lfo_phase_[k]);
        }
    }

    DelayLine    dlL_[3], dlR_[3];
    DelayLine    longL_, longR_;
    LoopShelf    shelfL_[3], shelfR_[3];
    LoopShelf    chainL_, chainR_;
    Rotation     rot_;
    Smoother     smL_[4], smR_[4];
    float        dL_[4] = {}, dR_[4] = {};
    float        tgtL_[4] = {}, tgtR_[4] = {};
    DcBlock      dcL_, dcR_;
    float        fbL_ = 0.f, fbR_ = 0.f;
    float        fb_gain_ = 0.5f;
    float        lfo_phase_[8]   = {};
    float        lfo_inc_[8]     = {};
    float        chaos_phase_[8] = {};
    float        chaos_inc_[8]   = {};
    float        chaos_          = 0.f;
    float        mod_depth_      = 0.f;
    float        block_sz_  = 48.f;
    float        mod_[8]     = {};
    float        mod_inc_[8] = {};
    bool         block_lfo_  = false;
};
