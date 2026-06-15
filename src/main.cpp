#include "schroeder.h"
#include <RtAudio.h>
#include <atomic>
#include <cmath>
#include <iostream>

static constexpr unsigned int kSampleRate    = 48000;
static constexpr unsigned int kBufferSize    = 256;
static constexpr unsigned int kChannels      = 2;
static constexpr int          kBeepDuration  = 4800;  // 100ms
static constexpr int          kBeepFade      = 480;   // 10ms fade in/out

struct State {
    float amp = 0.3f;

    std::atomic<bool> fireImpulse{false};

    // beep — startBeep set by main thread; rest owned exclusively by audio thread
    std::atomic<bool> startBeep{false};
    int   beepSamplesLeft = 0;
    float beepPhase       = 0.0f;
    float beepFreq        = 440.0f;

    Schroeder reverb;
};

int audioCallback(void* outputBuffer, void* /*inputBuffer*/,
                  unsigned int nFrames, double /*streamTime*/,
                  RtAudioStreamStatus status, void* userData) {
    auto* out   = static_cast<float*>(outputBuffer);
    auto* state = static_cast<State*>(userData);

    if (status) std::cerr << "Stream underflow\n";

    bool doImpulse = state->fireImpulse.exchange(false);
    if (state->startBeep.exchange(false)) {
        state->beepSamplesLeft = kBeepDuration;
        state->beepPhase       = 0.0f;
    }

    for (unsigned int i = 0; i < nFrames; ++i) {
        float dry = (doImpulse && i == 0) ? state->amp : 0.0f;

        if (state->beepSamplesLeft > 0) {
            float env = 1.0f;
            if (state->beepSamplesLeft > kBeepDuration - kBeepFade)
                env = float(kBeepDuration - state->beepSamplesLeft) / kBeepFade;
            else if (state->beepSamplesLeft < kBeepFade)
                env = float(state->beepSamplesLeft) / kBeepFade;
            dry += env * state->amp * std::sin(state->beepPhase);
            state->beepPhase += 2.0f * M_PI * state->beepFreq / kSampleRate;
            if (state->beepPhase >= 2.0f * M_PI) state->beepPhase -= 2.0f * M_PI;
            --state->beepSamplesLeft;
        }

        float s = dry + state->reverb.process(dry);
        out[i * kChannels + 0] = s;
        out[i * kChannels + 1] = s;
    }

    return 0;
}

int main() {
    RtAudio dac;

    if (dac.getDeviceCount() == 0) {
        std::cerr << "No audio devices found.\n";
        return 1;
    }

    State state;

    RtAudio::StreamParameters params;
    params.deviceId     = dac.getDefaultOutputDevice();
    params.nChannels    = kChannels;
    params.firstChannel = 0;

    unsigned int bufSize = kBufferSize;

    try {
        dac.openStream(&params, nullptr, RTAUDIO_FLOAT32,
                       kSampleRate, &bufSize, &audioCallback, &state);
        dac.startStream();
    } catch (RtAudioError& e) {
        std::cerr << e.getMessage() << '\n';
        return 1;
    }

    std::cout << "Commands: i = impulse  b = beep (440 Hz)  q = quit\n";

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "q") break;
        if (line == "i") {
            state.fireImpulse.store(true);
            std::cout << "* impulse\n";
        }
        if (line == "b") {
            state.startBeep.store(true);
            std::cout << "* beep\n";
        }
    }

    try { dac.stopStream(); } catch (RtAudioError& e) { std::cerr << e.getMessage() << '\n'; }
    if (dac.isStreamOpen()) dac.closeStream();

    return 0;
}
