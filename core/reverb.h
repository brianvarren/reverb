#pragma once
#include "early.h"
#include "late.h"
#include "params.h"
#include "shelf.h"
#include "util.h"
#include <cmath>

// Top-level signal graph:
//   In → Early ──────────────────────────────────┐
//      → Late (cross-coupled, modulated) ─────────┤
//                               eq-power E/L mix ─┘
//                               → OutputShelf → eq-power D/W → Out
//
// Caller-supplied buffers (16 zeroed power-of-two arrays):
//   [0..7]   early section  (early_sz each — see Early::Init)
//   [8..13]  late diffusers (diff_sz each)
//   [14..15] late long delays (long_sz each)
class Reverb {
public:
    void Init(float* bufs[16], size_t early_sz, size_t diff_sz, size_t long_sz,
              float fs, size_t block_sz = 48) {
        fs_ = fs;
        float* eb[8];
        float* lb[8];
        for (int i = 0; i < 8; ++i) eb[i] = bufs[i];
        for (int i = 0; i < 6; ++i) lb[i] = bufs[8 + i];
        lb[6] = bufs[14];
        lb[7] = bufs[15];
        early_.Init(eb, early_sz, fs, block_sz);
        late_ .Init(lb, diff_sz, long_sz, fs, block_sz);
    }

    // Call once after Init to skip the startup ramp.
    void SnapParams(const Params& p) {
        early_.SnapParams(p, fs_);
        late_ .SnapParams(p, fs_);
        _update_out_shelf(p);
        _update_xfade(p);
    }

    // Call once per block before the per-sample loop (firmware path).
    void UpdateBlock(const Params& p) {
        early_.UpdateBlock(p, fs_);
        late_ .UpdateBlock(p, fs_);
        _update_out_shelf(p);
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

        wetL = out_shelfL_.Process(wetL);
        wetR = out_shelfR_.Process(wetR);

        // Equal-power D/W
        outL = dw_cos_ * inL + dw_sin_ * wetL;
        outR = dw_cos_ * inR + dw_sin_ * wetR;
    }

private:
    void _update_out_shelf(const Params& p) {
        float hf = pitch_to_hz(p.eo_hf);
        float hb = db_to_lin(p.eo_hb);
        float lf = pitch_to_hz(p.eo_lf);
        float lb = db_to_lin(p.eo_lb);
        out_shelfL_.SetParams(hf, hb, lf, lb, fs_);
        out_shelfR_.SetParams(hf, hb, lf, lb, fs_);
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
    OutputShelf  out_shelfL_, out_shelfR_;
    float        el_cos_ = 0.f, el_sin_ = 1.f;
    float        dw_cos_ = 0.f, dw_sin_ = 1.f;
};
