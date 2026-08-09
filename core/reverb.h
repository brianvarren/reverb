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
        fs_       = fs;
        block_sz_ = float(block_sz);
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
        _update_xfade(p, /*interp=*/true);
    }

    void Process(float inL, float inR, float& outL, float& outR) {
        float eL, eR, ltL, ltR;
        early_.Process(inL, inR, eL, eR);
        late_ .Process(eL,  eR,  ltL, ltR);

        // Equal-power E/L — cos/sin removes the 3 dB hole that linear mixing creates
        // when early and late are decorrelated.
        float wetL = el_cos_ * eL + el_sin_ * ltL;
        float wetR = el_cos_ * eR + el_sin_ * ltR;

        wetL = _out_sat(outHpL_.Process(outLadderL_.Process(wetL)));
        wetR = _out_sat(outHpR_.Process(outLadderR_.Process(wetR)));

        // Equal-power D/W
        outL = dw_cos_ * inL + dw_sin_ * wetL;
        outR = dw_cos_ * inR + dw_sin_ * wetR;

        // Advance crossfade coefficients (zero increment in snap/host path).
        el_cos_ += el_cos_inc_;  el_sin_ += el_sin_inc_;
        dw_cos_ += dw_cos_inc_;  dw_sin_ += dw_sin_inc_;
    }

private:
    // VA-style output saturation: tanh with unity small-signal gain.
    // drive=2.0 gives gentle 2nd/3rd harmonic warmth; normalised by drive so
    // the small-signal path is transparent and headroom scales with drive.
    static float _out_sat(float x) {
        constexpr float kDrive = 2.0f;
        return tanhf(kDrive * x) / kDrive;
    }

    void _update_out_filter(const Params& p) {
        float lp_hz = p.out_freq * powf(2.f, p.out_width * 0.5f);
        float hp_hz = p.out_freq / powf(2.f, p.out_width * 0.5f);
        outLadderL_.SetParams(lp_hz, 0.f, fs_);
        outLadderR_.SetParams(lp_hz, 0.f, fs_);
        outHpL_.SetHP(hp_hz, fs_);
        outHpR_.SetHP(hp_hz, fs_);
    }

    void _update_xfade(const Params& p, bool interp = false) {
        constexpr float kHalfPi = static_cast<float>(M_PI) * 0.5f;
        float new_el_cos = cosf(p.el_mix * kHalfPi);
        float new_el_sin = sinf(p.el_mix * kHalfPi);
        float new_dw_cos = cosf(p.dw_mix * kHalfPi);
        float new_dw_sin = sinf(p.dw_mix * kHalfPi);
        if (interp) {
            el_cos_inc_ = (new_el_cos - el_cos_) / block_sz_;
            el_sin_inc_ = (new_el_sin - el_sin_) / block_sz_;
            dw_cos_inc_ = (new_dw_cos - dw_cos_) / block_sz_;
            dw_sin_inc_ = (new_dw_sin - dw_sin_) / block_sz_;
        } else {
            el_cos_ = new_el_cos;  el_sin_ = new_el_sin;
            dw_cos_ = new_dw_cos;  dw_sin_ = new_dw_sin;
            el_cos_inc_ = el_sin_inc_ = dw_cos_inc_ = dw_sin_inc_ = 0.f;
        }
    }

    float        fs_       = 48000.f;
    float        block_sz_ = 48.f;
    Early        early_;
    Late         late_;
    Ladder       outLadderL_, outLadderR_;
    HP1          outHpL_, outHpR_;
    float        el_cos_ = 0.f, el_sin_ = 1.f;
    float        el_cos_inc_ = 0.f, el_sin_inc_ = 0.f;
    float        dw_cos_ = 0.f, dw_sin_ = 1.f;
    float        dw_cos_inc_ = 0.f, dw_sin_inc_ = 0.f;
};
