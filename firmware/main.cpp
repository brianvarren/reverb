// ─────────────────────────────────────────────────────────────────────────────
// Stage-10 smoke test: does the core run on the Seed, and at what cost?
//
// Needs no external wiring. Flash it, open a serial monitor, read the numbers.
// Audio out on pins 18/19 is optional — it's there if you want to hear it.
//
// Build:  make            (VERB_HOT_SRAM=1 — see layout below)
//         make sdram      (everything in SDRAM — measures the latency penalty)
// ─────────────────────────────────────────────────────────────────────────────

#include "daisy_seed.h"
#include "config.h"
#include "reverb.h"

using namespace daisy;

// ── memory layout ────────────────────────────────────────────────────────────
// Buffer sizes come from config.h (shared with host/TUI) so null tests stay
// valid across builds.
//
// VERB_HOT_SRAM=1 (default):
//   SDRAM  — predelay (sequential reads, largest)
//            + late diffusers (6 × 32 KB, randomly accessed with modulation)
//   SRAM   — early allpass (6 × 2 KB), long delays (2 × 128 KB), injection (2 × 4 KB)
//            Total SRAM hot: 48 + 256 + 8 = 312 KB  (of 512 KB)
//
// VERB_HOT_SRAM=0:
//   All buffers in SDRAM.  Same audio, weaker cache utilisation.
#ifndef VERB_HOT_SRAM
#define VERB_HOT_SRAM 1
#endif

constexpr size_t kBlock = 48;
constexpr float  kFS    = 48000.f;

// Predelay and late diffusers always live in SDRAM.
static float DSY_SDRAM_BSS predelay_buf[2][kBufEarly];
static float DSY_SDRAM_BSS diff_buf[6][kBufDiff];

#if VERB_HOT_SRAM
static float ap_buf  [6][kBufEarly];   //  48 KB  ┐
static float long_buf[2][kBufLong];    // 256 KB  ├ 312 KB internal SRAM
static float inj_buf [2][kBufInj];     //   8 KB  ┘
#else
static float DSY_SDRAM_BSS ap_buf  [6][kBufEarly];
static float DSY_SDRAM_BSS long_buf[2][kBufLong];
static float DSY_SDRAM_BSS inj_buf [2][kBufInj];
#endif

// ── globals ──────────────────────────────────────────────────────────────────

static DaisySeed    hw;
static CpuLoadMeter meter;
static Reverb       reverb;
static Params       params;

// Internal test signal, so the measurement doesn't depend on anything being
// plugged in. Cycles: impulse → 250 ms noise burst → silence (tail + denormals).
struct TestGen {
    uint32_t rng   = 0x13579BDFu;
    uint32_t phase = 0;

    float Noise() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return float(int32_t(rng)) * (1.f / 2147483648.f);
    }

    float Process() {
        const uint32_t kPeriod = uint32_t(kFS) * 4;   // 4 s cycle
        float out = 0.f;
        if (phase == 0)                       out = 0.9f;                  // impulse
        else if (phase < uint32_t(kFS) * 1)   out = 0.f;                   // decay
        else if (phase < uint32_t(kFS) * 5/4) out = Noise() * 0.25f;       // burst
        // remainder: silence — this is where denormals show up
        if (++phase >= kPeriod) phase = 0;
        return out;
    }
};
static TestGen gen;

// Sweep lt_sz across the full pitch range to stress-test all buffer depths
// and every path through the interpolated-read/smoother logic.
static volatile bool  stress    = true;
static volatile float stress_sz = 18.f;

// ── linker symbols, for the memory report ────────────────────────────────────
extern "C" {
extern uint32_t _sdata, _ebss;              // internal SRAM, 0x24000000 + 512K
extern uint32_t _ssdram_bss, _esdram_bss;   // SDRAM,        0xC0000000 + 64M
}

static inline void EnableFlushToZero() {
    // Subnormal operands cost tens of cycles each on the M7, and a reverb tail
    // decaying toward silence generates them across every filter state at once.
    // FPDSCR is what exception contexts (i.e. the audio callback) inherit —
    // setting FPSCR alone does nothing for the interrupt that matters.
    FPU->FPDSCR |= (1u << 24);
    __set_FPSCR(__get_FPSCR() | (1u << 24));
}

// ── audio ────────────────────────────────────────────────────────────────────

static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    meter.OnBlockStart();

    if (stress) params.lt_sz = stress_sz;
    reverb.UpdateBlock(params);

    for (size_t i = 0; i < size; ++i) {
        const float t  = gen.Process();
        const float xL = in[0][i] + t;
        const float xR = in[1][i] + t;
        float yL, yR;
        reverb.Process(xL, xR, yL, yR);
        out[0][i] = yL;
        out[1][i] = yR;
    }

    meter.OnBlockEnd();
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(void)
{
    hw.Init();                       // brings up SDRAM, among other things
    hw.SetAudioBlockSize(kBlock);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    EnableFlushToZero();

    // SDRAM contents are undefined at boot and constructors ran before the
    // controller was up. Zero every SDRAM buffer explicitly; SRAM BSS is
    // already zeroed by the C startup code.
    for (int c = 0; c < 2; ++c)
        for (size_t i = 0; i < kBufEarly; ++i) predelay_buf[c][i] = 0.f;
    for (int c = 0; c < 6; ++c)
        for (size_t i = 0; i < kBufDiff; ++i) diff_buf[c][i] = 0.f;
#if !VERB_HOT_SRAM
    for (int c = 0; c < 6; ++c)
        for (size_t i = 0; i < kBufEarly; ++i) ap_buf[c][i] = 0.f;
    for (int c = 0; c < 2; ++c)
        for (size_t i = 0; i < kBufLong; ++i) long_buf[c][i] = 0.f;
    for (int c = 0; c < 2; ++c)
        for (size_t i = 0; i < kBufInj; ++i) inj_buf[c][i] = 0.f;
#endif

    // Wire up the 18-buffer array matching Reverb::Init's expected layout:
    //   [0..7]   early section (kBufEarly each)
    //   [8..13]  late diffusers (kBufDiff each)
    //   [14..15] late long delays (kBufLong each)
    //   [16..17] late injection delays (kBufInj each)
    float* bufs[18];
    bufs[0] = predelay_buf[0];
    bufs[1] = predelay_buf[1];
    for (int i = 0; i < 6; ++i) bufs[2 + i]  = ap_buf[i];
    for (int i = 0; i < 6; ++i) bufs[8 + i]  = diff_buf[i];
    bufs[14] = long_buf[0];
    bufs[15] = long_buf[1];
    bufs[16] = inj_buf[0];
    bufs[17] = inj_buf[1];

    reverb.Init(bufs, kBufEarly, kBufDiff, kBufLong, kBufInj, kFS, kBlock);
    ParamsClamp(params);
    reverb.SnapParams(params);

    meter.Init(hw.AudioSampleRate(), hw.AudioBlockSize());
    hw.StartLog(false);              // false = don't block waiting for a terminal
    hw.StartAudio(AudioCallback);

    const uint32_t sram_used  = uint32_t((uintptr_t)&_ebss - 0x24000000u);
    const uint32_t sdram_used = uint32_t((uintptr_t)&_esdram_bss
                                       - (uintptr_t)&_ssdram_bss);

    uint32_t last = System::GetNow();
    bool     led  = false;

    while (1) {
        // Sweep lt_sz from high (short loops, heavy cache on diffusers) down to
        // the minimum safe pitch (-24 = ~23k-sample diffuser taps) to exercise
        // the full buffer depth and smoother-glide path.
        if (stress) {
            static float dir = -1.f;
            stress_sz += dir * 0.25f;
            if (stress_sz < -24.f) { stress_sz = -24.f; dir =  1.f; }
            if (stress_sz >  42.f) { stress_sz =  42.f; dir = -1.f; }
        }

        if (System::GetNow() - last >= 1000) {
            last = System::GetNow();
            led  = !led;
            hw.SetLed(led);

            hw.PrintLine("── sz %d  |  cpu avg " FLT_FMT3 "  max " FLT_FMT3
                         "  min " FLT_FMT3,
                         int(stress_sz),
                         FLT_VAR3(meter.GetAvgCpuLoad() * 100.f),
                         FLT_VAR3(meter.GetMaxCpuLoad() * 100.f),
                         FLT_VAR3(meter.GetMinCpuLoad() * 100.f));
            hw.PrintLine("   sram %u/524288 (%u%%)   sdram %u/67108864 (%u%%)",
                         sram_used,  (sram_used  * 100u) / 524288u,
                         sdram_used, (sdram_used * 100u) / 67108864u);
            meter.Reset();
        }
        System::Delay(20);
    }
}
