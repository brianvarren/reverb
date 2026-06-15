#pragma once
#include <array>

// Circular buffer. N is the maximum delay in samples.
template <int N>
class DelayLine {
    std::array<float, N> buf_{};
    int write_ = 0;
public:
    void write(float x) {
        buf_[write_] = x;
        if (++write_ == N) write_ = 0;
    }
    float read(int d) const {
        int i = write_ - d;
        if (i < 0) i += N;
        return buf_[i];
    }
    void clear() { buf_.fill(0.0f); }
};

// Feedback comb filter with one-pole low-pass damping in the feedback loop.
// Classic Schroeder-Moorer structure: output = delay[n], delay[n] = x + fb * lp(delay[n])
template <int N>
class CombFilter {
    DelayLine<N> line_;
    float lp_   = 0.0f;
    int   delay_;
    float fb_;
    float damp_;
public:
    CombFilter(int delay, float fb, float damp)
        : delay_(delay), fb_(fb), damp_(damp) {}

    float process(float x) {
        float d = line_.read(delay_);
        lp_ = d * (1.0f - damp_) + lp_ * damp_;
        line_.write(x + fb_ * lp_);
        return d;
    }

    void clear() { line_.clear(); lp_ = 0.0f; }
};

// Schroeder all-pass filter.
// y[n] = -g*x[n] + x[n-M] + g*y[n-M]  — flat frequency response, adds diffusion.
template <int N>
class AllPassFilter {
    DelayLine<N> line_;
    int   delay_;
    float gain_;
public:
    AllPassFilter(int delay, float gain)
        : delay_(delay), gain_(gain) {}

    float process(float x) {
        float d = line_.read(delay_);
        float v = x + gain_ * d;
        line_.write(v);
        return d - gain_ * v;
    }

    void clear() { line_.clear(); }
};
