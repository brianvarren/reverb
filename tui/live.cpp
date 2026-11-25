// ─────────────────────────────────────────────────────────────────────────────
// verb_live — direct-ALSA full-duplex host for the verb reverb.
//
// Opens one physical device (e.g. the Tonverk at hw:1,0) for both capture and
// playback, snd_pcm_link()s the two streams so they share a single clock, and
// runs a tight read → reverb → write loop on one thread. No miniaudio, no
// resampling, no bridge buffer — which is what the duplex path needs on this
// hardware. Headless: prints xrun count + DSP load once a second.
//
//   build/verb_live [params.txt] [--dev hw:1,0] [--period 256] [--periods 3]
//                   [--inch 4] [--inpair 3 4] [--secs N] [--rtprio 80]
// ─────────────────────────────────────────────────────────────────────────────

#include <alsa/asoundlib.h>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <sys/mman.h>
#include <vector>

#include "engine.h"
#include "params.h"

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int) { g_stop = 1; }

// ── S24_3LE (3-byte packed, signed) ↔ float ─────────────────────────────────
static inline float s24_to_f(const uint8_t* p) {
    int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    if (v & 0x00800000) v |= ~0x00FFFFFF;          // sign-extend bit 23
    return (float)v * (1.0f / 8388608.0f);
}
static inline void f_to_s24(float f, uint8_t* p) {
    if (f >  1.0f) f =  1.0f;
    if (f < -1.0f) f = -1.0f;
    int32_t v = (int32_t)lrintf(f * 8388607.0f);
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}

// ── params (minimal key=value loader, same format as the rest of the repo) ───
static Params load_params(const char* path, bool* ok) {
    Params p;
    FILE* f = fopen(path, "r");
    if (!f) { *ok = false; return p; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char* h = strchr(line, '#'); if (h) *h = 0;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char key[128] = {0};
        sscanf(line, "%127s", key);
        const float v = strtof(eq + 1, nullptr);
        const ParamDesc* t = ParamTable();
        for (int i = 0; i < kParamCount; ++i)
            if (strcmp(key, t[i].key) == 0) { *ParamSlot(p, i) = v; break; }
    }
    fclose(f);
    ParamsClamp(p);
    *ok = true;
    return p;
}

static snd_pcm_t* open_pcm(const char* dev, snd_pcm_stream_t dir, unsigned rate,
                           unsigned ch, snd_pcm_uframes_t* period, unsigned* periods) {
    snd_pcm_t* h = nullptr;
    int err = snd_pcm_open(&h, dev, dir, 0);
    if (err < 0) { fprintf(stderr, "open %s (%s): %s\n", dev,
                           dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                           snd_strerror(err)); return nullptr; }
    snd_pcm_hw_params_t* hp; snd_pcm_hw_params_alloca(&hp);
    snd_pcm_hw_params_any(h, hp);
    snd_pcm_hw_params_set_access(h, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(h, hp, SND_PCM_FORMAT_S24_3LE);
    snd_pcm_hw_params_set_channels(h, hp, ch);
    snd_pcm_hw_params_set_rate(h, hp, rate, 0);
    snd_pcm_hw_params_set_period_size_near(h, hp, period, 0);
    snd_pcm_hw_params_set_periods_near(h, hp, periods, 0);
    err = snd_pcm_hw_params(h, hp);
    if (err < 0) { fprintf(stderr, "hw_params %s: %s\n", dev, snd_strerror(err));
                   snd_pcm_close(h); return nullptr; }
    return h;
}

int main(int argc, char** argv) {
    const char* params_path = "params.txt";
    const char* dev   = "hw:1,0";
    unsigned    rate  = 48000;
    snd_pcm_uframes_t period = 256;
    unsigned    periods = 3;
    int         inch  = 4;
    int         li1 = 3, ri1 = 4;      // 1-based capture channels (OUT CD)
    int         secs  = 0;             // 0 = until Ctrl-C
    int         rtprio = 80;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--dev")     && i+1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "--period")  && i+1 < argc) period = (snd_pcm_uframes_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--periods") && i+1 < argc) periods = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--inch")    && i+1 < argc) inch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--inpair")  && i+2 < argc) { li1 = atoi(argv[++i]); ri1 = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--secs")    && i+1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rtprio")  && i+1 < argc) rtprio = atoi(argv[++i]);
        else if (argv[i][0] != '-')                           params_path = argv[i];
    }
    const int li = li1 - 1, ri = ri1 - 1;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    // ── realtime: lock memory + SCHED_FIFO so the loop can't be preempted ──────
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "warn: mlockall failed (continuing)\n");
    if (rtprio > 0) {
        struct sched_param sp; sp.sched_priority = rtprio;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
            fprintf(stderr, "warn: SCHED_FIFO prio %d denied (need rtprio limit); "
                            "running at normal priority\n", rtprio);
        else
            fprintf(stderr, "realtime: SCHED_FIFO prio %d\n", rtprio);
    }

    // ── engine ─────────────────────────────────────────────────────────────────
    bool ok = false;
    const Params p0 = load_params(params_path, &ok);
    static Engine eng;
    eng.Init((float)rate, p0);

    // ── open + link the two streams ─────────────────────────────────────────────
    snd_pcm_uframes_t cap_period = period, play_period = period;
    unsigned cap_periods = periods, play_periods = periods;
    snd_pcm_t* cap  = open_pcm(dev, SND_PCM_STREAM_CAPTURE,  rate, (unsigned)inch, &cap_period, &cap_periods);
    snd_pcm_t* play = open_pcm(dev, SND_PCM_STREAM_PLAYBACK, rate, 2,              &play_period, &play_periods);
    if (!cap || !play) return 1;
    if (cap_period != play_period)
        fprintf(stderr, "warn: period differs cap=%lu play=%lu\n",
                (unsigned long)cap_period, (unsigned long)play_period);
    period = cap_period;

    if (snd_pcm_link(cap, play) < 0)
        fprintf(stderr, "warn: snd_pcm_link failed (streams unlinked)\n");

    snd_pcm_prepare(cap);

    // Prime playback with silence so the linked start has frames queued and does
    // not underrun immediately.
    std::vector<uint8_t> pbuf(period * 2 * 3);          // S24_3LE stereo
    std::vector<uint8_t> cbuf(period * inch * 3);       // S24_3LE inch-channel
    std::vector<float>   infl(period * inch);
    std::vector<float>   outfl(period * 2);
    memset(pbuf.data(), 0, pbuf.size());
    for (unsigned i = 0; i < periods; ++i)
        snd_pcm_writei(play, pbuf.data(), period);

    fprintf(stderr, "live: dev=%s  %u Hz  period=%lu  periods=%u  inpair=%d/%d of %dch\n",
            dev, rate, (unsigned long)period, periods, li1, ri1, inch);

    // ── run loop ─────────────────────────────────────────────────────────────
    long  xruns = 0;
    long  frames_done = 0;
    long  next_report = rate;      // report every ~1s of frames
    int   elapsed_s = 0;

    while (!g_stop) {
        snd_pcm_sframes_t n = snd_pcm_readi(cap, cbuf.data(), period);
        if (n < 0) {
            snd_pcm_recover(cap, (int)n, 1);
            ++xruns;
            continue;
        }
        const snd_pcm_uframes_t got = (snd_pcm_uframes_t)n;

        // S24 interleaved → float interleaved (inch channels)
        for (snd_pcm_uframes_t f = 0; f < got; ++f)
            for (int c = 0; c < inch; ++c)
                infl[f * inch + c] = s24_to_f(&cbuf[(f * inch + c) * 3]);

        // reverb: read li/ri, write stereo float
        eng.RenderLive(infl.data(), (uint32_t)inch, li, ri, outfl.data(), got);

        // float → S24 stereo
        for (snd_pcm_uframes_t f = 0; f < got; ++f) {
            f_to_s24(outfl[f * 2 + 0], &pbuf[(f * 2 + 0) * 3]);
            f_to_s24(outfl[f * 2 + 1], &pbuf[(f * 2 + 1) * 3]);
        }

        snd_pcm_sframes_t w = snd_pcm_writei(play, pbuf.data(), got);
        if (w < 0) {
            snd_pcm_recover(play, (int)w, 1);
            ++xruns;
        }

        frames_done += (long)got;
        if (frames_done >= next_report) {
            next_report += rate;
            ++elapsed_s;
            fprintf(stderr, "  t=%3ds  xruns=%ld  cpu_load=%.2f  blowups=%d\n",
                    elapsed_s, xruns, eng.cpu_load(), eng.blowups());
            if (secs > 0 && elapsed_s >= secs) break;
        }
    }

    snd_pcm_drain(play);
    snd_pcm_close(play);
    snd_pcm_close(cap);
    fprintf(stderr, "done: %ld frames, %ld xruns\n", frames_done, xruns);
    return 0;
}
