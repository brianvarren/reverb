#pragma once
#include "delay.h"
#include "shelf.h"
#include "util.h"

// Schroeder allpass with an embedded two-band shelf on the delay output.
//
// Topology (matching SMD's DiffDel):
//   v = shelf( dl[n - d] )
//   w = x + g * v
//   dl.write(w)
//   y = v - g * w
//
// The shelf is inside the loop, so use LoopShelf (kLoopSafe=true) which
// asserts shelf gains <= 1.0 on every SetShelf call.
//
// Integer reads only (no modulation on the early section). The Smoother
// protects against zipper noise when Size sweeps.
//
// Read convention: Read(d-1) gives an actual loop delay of d samples.
// SnapDelay / AdvanceDelay take the desired delay in samples (d, not d-1).

class AllPass {
public:
    // buf must be zeroed by the caller; size must be a power of two >= d_samples.
    void Init(float* buf, size_t size_pow2, float fs, size_t block_size = 48) {
        dl_.Init(buf, size_pow2);
        smoother_.SetTC(0.050f, float(block_size) / fs);
    }

    // Hard-clamped to |g| < 0.95 (stability requirement).
    void SetGain(float g) { g_ = clampf(g, -0.949f, 0.949f); }

    // Shelf params. For bypass (flat response): (any_hz, 1.0f, 0.0f, 0.0f, fs).
    // LoopShelf asserts hb_lin <= 1 and lb_lin <= 1.
    void SetShelf(float hf_hz, float hb_lin, float lf_hz, float lb_lin, float fs) {
        shelf_.SetParams(hf_hz, hb_lin, lf_hz, lb_lin, fs);
    }

    // Advance smoother toward d_samples. Call once per control block.
    void AdvanceDelay(float d_samples) { d_ = smoother_.Process(d_samples); }

    // Snap delay immediately (call in Init path to avoid boot ramp).
    void SnapDelay(float d_samples) { smoother_.Reset(d_samples); d_ = d_samples; }

    // Per-sample. Read(d-1) → actual loop delay of d samples. Read before write.
    float Process(float x) {
        size_t rd = (d_ >= 1.f) ? size_t(d_) - 1 : 0;
        float  v  = shelf_.Process(dl_.Read(rd));
        float  w  = x + g_ * v;
        dl_.Write(w);
        return v - g_ * w;
    }

private:
    DelayLine dl_;
    LoopShelf shelf_;
    Smoother  smoother_;
    float     d_ = 0.f;
    float     g_ = 0.70f;
};
