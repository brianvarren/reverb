#pragma once
#include "allpass.h"
#include "delay.h"
#include "params.h"
#include "shelf.h"
#include "util.h"

// Early section: predelay → 3 allpasses (per channel) → chain shelf.
//
// No cross-feed between L and R — stereo width comes entirely from SzL/SzR
// asymmetry (ErSym semitones). Adding cross-feed here smears the image and
// makes the reverb sound vague; resist the temptation.
//
// Allpass stage offsets: +8, +4, +0 semitones above ErSz — irrational ratios
// so the echo trains never realign at any Size.
//
// 8 caller-supplied buffers (all zeroed, all same power-of-two size):
//   [0]     predelay L
//   [1]     predelay R
//   [2..4]  allpass L stages 0,1,2  (offsets +8,+4,+0)
//   [5..7]  allpass R stages 0,1,2
// Recommended buf_sz: 4096 — covers ErSz down to pitch 18 (~2076 samples).

class Early {
public:
    void Init(float* bufs[8], size_t buf_sz, float fs, size_t block_sz = 48) {
        buf_sz_ = buf_sz;
        float blk_dt = float(block_sz) / fs;
        pd_[0].Init(bufs[0], buf_sz);
        pd_[1].Init(bufs[1], buf_sz);
        for (int i = 0; i < 2; ++i)
            pd_sm_[i].SetTC(0.050f, blk_dt);
        for (int ch = 0; ch < 2; ++ch)
            for (int st = 0; st < 3; ++st)
                ap_[ch][st].Init(bufs[2 + ch * 3 + st], buf_sz, fs, block_sz);
    }

    // Snap all delays immediately — call after Init to avoid boot ramp.
    void SnapParams(const Params& p, float fs) {
        const float max_pd = float(buf_sz_) - 1.f;
        const float pdL = clampf((p.predelay_ms - p.pd_sym) * fs * 0.001f, 1.f, max_pd);
        const float pdR = clampf((p.predelay_ms + p.pd_sym) * fs * 0.001f, 1.f, max_pd);
        pd_sm_[0].Reset(pdL);  pd_d_[0] = pdL;
        pd_sm_[1].Reset(pdR);  pd_d_[1] = pdR;
        _apply_params(p, fs, /*snap=*/true);
    }

    // Advance smoothers by one block — call once per control block.
    void UpdateBlock(const Params& p, float fs) {
        const float max_pd = float(buf_sz_) - 1.f;
        const float pdL = clampf((p.predelay_ms - p.pd_sym) * fs * 0.001f, 1.f, max_pd);
        const float pdR = clampf((p.predelay_ms + p.pd_sym) * fs * 0.001f, 1.f, max_pd);
        pd_d_[0] = pd_sm_[0].Process(pdL);
        pd_d_[1] = pd_sm_[1].Process(pdR);
        _apply_params(p, fs, /*snap=*/false);
    }

    // Per-sample. No cross-feed.
    void Process(float inL, float inR, float& outL, float& outR) {
        // Predelay: Read(d-1) → actual delay of d samples (read before write)
        float xL = _pd_read(0, inL);
        float xR = _pd_read(1, inR);

        // Three-stage allpass chain, L and R fully independent
        for (int st = 0; st < 3; ++st) {
            xL = ap_[0][st].Process(xL);
            xR = ap_[1][st].Process(xR);
        }

        // Chain shelf (OutputShelf — not in a feedback loop)
        outL = chain_[0].Process(xL);
        outR = chain_[1].Process(xR);
    }

private:
    float _pd_read(int ch, float in) {
        size_t rd = pd_d_[ch] >= 1.f ? size_t(pd_d_[ch]) - 1 : 0;
        float  v  = pd_[ch].Read(rd);
        pd_[ch].Write(in);
        return v;
    }

    void _apply_params(const Params& p, float fs, bool snap) {
        const float szL = p.er_sz - p.er_sym;  // er_sym baked at 2.0 in Params
        const float szR = p.er_sz + p.er_sym;
        const float g   = clampf(p.er_dffs, -0.949f, 0.949f);
        constexpr float kHpPitch = 108.f, kLpPitch = 36.f;
        const float hf  = pitch_to_hz(kHpPitch);
        const float hb  = db_to_lin(fminf(p.dmp_eq *  18.f,    0.f));
        const float lf  = pitch_to_hz(kLpPitch);
        const float lb  = db_to_lin(fminf(p.dmp_eq * -25.92f,  0.f));
        const float offsets[3] = {8.f, 4.f, 0.f};

        for (int ch = 0; ch < 2; ++ch) {
            const float sz = (ch == 0) ? szL : szR;
            for (int st = 0; st < 3; ++st) {
                const float d = pitch_to_samples(sz + offsets[st], fs);
                ap_[ch][st].SetGain(g);
                ap_[ch][st].SetShelf(hf, hb, lf, lb, fs);
                if (snap) ap_[ch][st].SnapDelay(d);
                else      ap_[ch][st].AdvanceDelay(d);
            }
        }
        for (int ch = 0; ch < 2; ++ch)
            chain_[ch].SetParams(hf, hb, lf, lb, fs);
    }

    size_t      buf_sz_ = 0;
    DelayLine   pd_[2];
    Smoother    pd_sm_[2];
    float       pd_d_[2] = {};
    AllPass     ap_[2][3];
    OutputShelf chain_[2];
};
