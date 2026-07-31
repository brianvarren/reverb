#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

// Minimal 16-bit PCM mono WAV writer.
inline void writeWav(const char* path, const std::vector<float>& samples, unsigned int sampleRate) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;

    uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    uint32_t byteRate  = sampleRate * sizeof(int16_t);
    uint16_t blockAlign = sizeof(int16_t);
    uint32_t riffSize  = 36 + dataBytes;

    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riffSize, 4, 1, f);
    std::fwrite("WAVE", 1, 4, f);

    std::fwrite("fmt ", 1, 4, f);
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1; // PCM
    uint16_t numChannels = 1;
    uint16_t bitsPerSample = 16;
    std::fwrite(&fmtSize, 4, 1, f);
    std::fwrite(&audioFormat, 2, 1, f);
    std::fwrite(&numChannels, 2, 1, f);
    std::fwrite(&sampleRate, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&blockAlign, 2, 1, f);
    std::fwrite(&bitsPerSample, 2, 1, f);

    std::fwrite("data", 1, 4, f);
    std::fwrite(&dataBytes, 4, 1, f);

    for (float s : samples) {
        float clamped = s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s);
        int16_t v = static_cast<int16_t>(clamped * 32767.0f);
        std::fwrite(&v, 2, 1, f);
    }

    std::fclose(f);
}
