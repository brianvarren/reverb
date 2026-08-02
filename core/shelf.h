#pragma once
#include <cassert>
#include <cmath>

struct OnePole {
    float z = 0.f, a = 0.f;
    void SetCutoff(float fc, float fs) {
        a = 1.f - expf(-2.f * float(M_PI) * fc / fs);
    }
    float LP(float x) { z += a * (x - z); return z; }
    void Reset() { z = 0.f; }
};

// kLoopSafe=true: assert gains <= 1.0 (i.e. <= 0 dB) on SetParams.
// This enforces the §6.1 stability precondition structurally — any shelf
// inside the feedback loop must be instantiated as Shelf<true>.
template<bool kLoopSafe = false>
struct Shelf {
    // hb_lin / lb_lin: linear amplitude gains (10^(dB/20)).
    // In-loop shelves: both must be <= 1.0 (gains <= 0 dB).
    void SetParams(float hf_hz, float hb_lin,
                   float lf_hz, float lb_lin, float fs) {
        if constexpr (kLoopSafe) {
            assert(hb_lin <= 1.f && "in-loop high shelf: gain must be <= 0 dB");
            assert(lb_lin <= 1.f && "in-loop low shelf:  gain must be <= 0 dB");
            hb_lin = fminf(hb_lin, 1.f);
            lb_lin = fminf(lb_lin, 1.f);
        }
        hb_ = hb_lin;
        lb_ = lb_lin;
        lp_h_.SetCutoff(hf_hz, fs);
        lp_l_.SetCutoff(lf_hz, fs);
    }

    float Process(float x) {
        // High shelf: unity at DC, gain hb_ at Nyquist.
        // Shape: A + (1-A)*LP  →  |H| <= 1 for A <= 1  (triangle inequality)
        x = hb_ * x + (1.f - hb_) * lp_h_.LP(x);
        // Low shelf: gain lb_ at DC, unity at Nyquist.
        // Shape: A*LP + HP = 1 - (1-A)*LP  →  |H| <= 1 for A <= 1 (proved via |F|² <= Re(F))
        float l = lp_l_.LP(x);
        return lb_ * l + (x - l);
    }

    void Reset() { lp_h_.Reset(); lp_l_.Reset(); }

private:
    OnePole lp_h_, lp_l_;
    float   hb_ = 1.f, lb_ = 1.f;
};

using LoopShelf   = Shelf<true>;
using OutputShelf = Shelf<false>;
