#pragma once
#include <cstddef>
#include "util.h"

class DelayLine {
public:
    // buf must be zeroed by the caller; size must be a power of two.
    void Init(float* buf, size_t size_pow2) {
        buf_ = buf;
        mask_ = size_pow2 - 1;
        w_ = 0;
    }

    // Read-then-write: always call Read* before Write in a feedback loop.
    // Calling Write first gives d-1 samples of actual delay at d near 0.

    inline float Read(size_t d) const {
        return buf_[(w_ - 1 - d) & mask_];
    }

    inline float Read4pt(float d) const {
        d = clampf(d, 2.0f, float(mask_ - 2));
        int    i    = int(d);
        float  x    = d - float(i);
        size_t base = (w_ - 1 - i) & mask_;        // delay of i samples
        float  ym1  = buf_[(base + 1) & mask_];    // delay i-1 (more recent)
        float  y0   = buf_[base];
        float  y1   = buf_[(base - 1) & mask_];
        float  y2   = buf_[(base - 2) & mask_];
        float c0 = y0;
        float c1 = y1 - (1.f/3.f)*ym1 - 0.5f*y0 - (1.f/6.f)*y2;
        float c2 = 0.5f*(ym1 + y1) - y0;
        float c3 = (1.f/6.f)*(y2 - ym1) + 0.5f*(y0 - y1);
        return ((c3*x + c2)*x + c1)*x + c0;
    }

    inline void Write(float x) {
        buf_[w_] = x;
        w_ = (w_ + 1) & mask_;
    }

private:
    float*  buf_  = nullptr;
    size_t  mask_ = 0;
    size_t  w_    = 0;
};
