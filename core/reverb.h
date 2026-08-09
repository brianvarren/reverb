#pragma once
#include "early.h"
#include "ladder.h"
#include "late.h"
#include "params.h"
#include "util.h"
#include <cmath>

// Top-level signal graph:
//   In → Early ──────────────────────────────────┐
//      → Late (cross-coupled, modulated) ─────────┤
//                               eq-power E/L mix ─┘
//                               → OutputShelf → eq-power D/W → Out
//
// Caller-supplied buffers (18 zeroed power-of-two arrays):
//   [0..7]   early section       (early_sz each — see Early::Init)
//   [8..13]  late diffusers      (diff_sz each)
//   [14..15] late long delays    (long_sz each)
//   [16..17] late injection delays (inj_sz each)
class Reverb {
public:
    void Init(float* bufs[18], size_t early_sz, size_t diff_sz, size_t long_sz,
              size_t inj_sz, float fs, size_t block_sz = 48) {
        fs_ = fs;
        float* eb[8];
        float* lb[10];
        for (int i = 0; i < 8; ++i) eb[i] = bufs[i];
        for (int i = 0; i < 6; ++i) lb[i] = bufs[8 + i];
        lb[6] = bufs[14];
        lb[7] = bufs[15];
        lb[8] = bufs[16];
        lb[9] = bufs[17];
        early_.Init(eb, early_sz, fs, block_sz);
        late_ .Init(lb, diff_sz, long_sz, inj_sz, fs, block_sz);
    }

    // Call once after Init to skip the startup ramp.
    void SnapParams(Params p) {
        p.Derive();
        early_.SnapParams(p, fs_);
        late_ .SnapParams(p, fs_);
        _update_out_filter(p);
        _update_xfade(p);
    }

    // Call once per block before the per-sample loop (firmware path).
    void UpdateBlock(Params p) {
        p.Derive();
        early_.UpdateBlock(p, fs_);
        late_ .UpdateBlock(p, fs_);
        _update_out_filter(p);
        _update_xfade(p);
    }

    void Process(float inL, float inR, float& outL, float& outR) {
        float eL, eR, ltL, ltR;
        early_.Process(inL, inR, eL, eR);
        late_ .Process(eL,  eR,  ltL, ltR);

        // Equal-power E/L — cos/sin removes the 3 dB hole that linear mixing creates
        // when early and late are decorrelated.
        float wetL = el_cos_ * eL + el_sin_ * ltL;
        float wetR = el_cos_ * eR + el_sin_ * ltR;

        wetL = outHpL_.Process(outLadderL_.Process(wetL));
        wetR = outHpR_.Process(outLadderR_.Process(wetR));

        // Equal-power D/W
        outL = dw_cos_ * inL + dw_sin_ * wetL;
        outR = dw_cos_ * inR + dw_sin_ * wetR;
    }

private:
    void _update_out_filter(const Params& p) {
        float lp_hz = p.out_freq * powf(2.f, p.out_width * 0.5f);
        float hp_hz = p.out_freq / powf(2.f, p.out_width * 0.5f);
        outLadderL_.SetParams(lp_hz, 0.f, fs_);
        outLadderR_.SetParams(lp_hz, 0.f, fs_);
        outHpL_.SetHP(hp_hz, fs_);
        outHpR_.SetHP(hp_hz, fs_);
    }

    void _update_xfade(const Params& p) {
        constexpr float kHalfPi = static_cast<float>(M_PI) * 0.5f;
        el_cos_ = cosf(p.el_mix * kHalfPi);
        el_sin_ = sinf(p.el_mix * kHalfPi);
        dw_cos_ = cosf(p.dw_mix * kHalfPi);
        dw_sin_ = sinf(p.dw_mix * kHalfPi);
    }

    float        fs_ = 48000.f;
    Early        early_;
    Late         late_;
    Ladder       outLadderL_, outLadderR_;
    HP1          outHpL_, outHpR_;
    float        el_cos_ = 0.f, el_sin_ = 1.f;
    float        dw_cos_ = 0.f, dw_sin_ = 1.f;
};
