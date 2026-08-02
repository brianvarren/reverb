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
        _step_lfo();  // advance once per block when UpdateBlock drives the loop
    }

    void Process(float eL, float eR, float& outL, float& outR) {
        // When no block driver calls UpdateBlock, advance LFO per sample.
        // Cost is 8 sinf/sample on the host; the Daisy port moves this to UpdateBlock.
        if (lfo_inc_ > 0.f) _step_lfo();

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

        outL = longL_.Read4pt(dL_[3] + mod_[3]);  longL_.Write(aL);
        outR = longR_.Read4pt(dR_[3] + mod_[7]);  longR_.Write(aR);

        fbL_ = dcL_.Process(outL) * fb_gain_;
        fbR_ = dcR_.Process(outR) * fb_gain_;
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
        float hf = pitch_to_hz(p.dmp_hf);
        float hb = db_to_lin(p.dmp_hb);
        float lf = pitch_to_hz(p.dmp_lf);
        float lb = db_to_lin(p.dmp_lb);
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
        lfo_inc_   = p.mod_hz / fs;
        lfo_depth_ = p.mod_ms * 0.001f * fs;
    }

    // Advances the phase and recomputes all 8 modulation offsets (samples).
    // Eight equally-spaced taps: mod_[0..3] = L diffusers + long delay,
    // mod_[4..7] = R diffusers + long delay.
    void _step_lfo() {
        lfo_phase_ += lfo_inc_;
        if (lfo_phase_ >= 1.f) lfo_phase_ -= 1.f;
        const float kTwoPi = 2.f * float(M_PI);
        for (int k = 0; k < 8; ++k)
            mod_[k] = lfo_depth_ * sinf(kTwoPi * (lfo_phase_ + k * 0.125f));
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
    float        lfo_phase_ = 0.f, lfo_inc_ = 0.f, lfo_depth_ = 0.f;
    float        mod_[8] = {};
};
