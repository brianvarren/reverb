// ─────────────────────────────────────────────────────────────────────────────
// verb_panel — touch control surface for the live reverb (landscape console).
//
// Three threads, sharing the lock-free Engine:
//   - audio : SCHED_FIFO direct-ALSA duplex loop (Tonverk hw:1,0), same as verb_live
//   - touch : raw evdev reader on the Fujitsu panel (event8), maps drag → params
//   - main  : ncurses UI, four filled-rectangle faders (Decay/Size/Out/Dry-Wet)
//
//   build/verb_panel [params.txt] [--dev hw:1,0] [--period 256] [--touch /dev/input/event8]
//
// Run this ON the Lifebook's physical console (it draws there and reads touch);
// drive builds over SSH.
// ─────────────────────────────────────────────────────────────────────────────

#include <alsa/asoundlib.h>
#include <ncurses.h>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <thread>
#include <unistd.h>

#include "engine.h"
#include "params.h"

// ── touch calibration (native landscape; flip here if axes are mirrored) ──────
static int  TX_MIN = 128,  TX_MAX = 3932;
static int  TY_MIN = 300,  TY_MAX = 4001;
static bool INVERT_X = false;
static bool INVERT_Y = true;   // screen top = max value

// ── faders: one per engine parameter, built dynamically from ParamTable() ─────
// Fader b drives engine param b directly; ranges/curve/label come from the
// table, so promoting a baked constant to a param makes a slider appear here
// automatically. Touch maps 0..1 → value via the param's curve (ParamMapNorm).
static const int NB = kParamCount;

// ── S24_3LE (3-byte packed, signed) ↔ float ─────────────────────────────────
static inline float s24_to_f(const uint8_t* p) {
    int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    if (v & 0x00800000) v |= ~0x00FFFFFF;
    return (float)v * (1.0f / 8388608.0f);
}
static inline void f_to_s24(float f, uint8_t* p) {
    if (f >  1.0f) f =  1.0f;
    if (f < -1.0f) f = -1.0f;
    int32_t v = (int32_t)lrintf(f * 8388607.0f);
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); p[2] = (uint8_t)((v >> 16) & 0xFF);
}

struct Shared {
    Engine* eng = nullptr;
    std::atomic<bool> run{true};
    // audio cfg
    const char* dev = "hw:1,0";
    unsigned rate = 48000; snd_pcm_uframes_t period = 256; unsigned periods = 3;
    int inch = 4, li = 2, ri = 3, rtprio = 80;
    std::atomic<long> xruns{0};
    // touch state (for UI display + calibration readout)
    std::atomic<int> tx{0}, ty{0}, tdown{0}, active{-1};
};
static Shared g_sh;

static void on_sig(int) { g_sh.run.store(false); }

static Params load_params(const char* path, bool* ok) {
    Params p; FILE* f = fopen(path, "r");
    if (!f) { *ok = false; return p; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char* h = strchr(line, '#'); if (h) *h = 0;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char key[128] = {0}; sscanf(line, "%127s", key);
        const float v = strtof(eq + 1, nullptr);
        const ParamDesc* t = ParamTable();
        for (int i = 0; i < kParamCount; ++i)
            if (strcmp(key, t[i].key) == 0) { *ParamSlot(p, i) = v; break; }
    }
    fclose(f); ParamsClamp(p); *ok = true; return p;
}

static snd_pcm_t* open_pcm(const char* dev, snd_pcm_stream_t dir, unsigned rate,
                           unsigned ch, snd_pcm_uframes_t* period, unsigned* periods) {
    snd_pcm_t* h = nullptr;
    if (snd_pcm_open(&h, dev, dir, 0) < 0) return nullptr;
    snd_pcm_hw_params_t* hp; snd_pcm_hw_params_alloca(&hp);
    snd_pcm_hw_params_any(h, hp);
    snd_pcm_hw_params_set_access(h, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(h, hp, SND_PCM_FORMAT_S24_3LE);
    snd_pcm_hw_params_set_channels(h, hp, ch);
    snd_pcm_hw_params_set_rate(h, hp, rate, 0);
    snd_pcm_hw_params_set_period_size_near(h, hp, period, 0);
    snd_pcm_hw_params_set_periods_near(h, hp, periods, 0);
    if (snd_pcm_hw_params(h, hp) < 0) { snd_pcm_close(h); return nullptr; }
    return h;
}

// ── audio thread: SCHED_FIFO direct-ALSA duplex ──────────────────────────────
static void audio_thread() {
    Shared* s = &g_sh;
    if (s->rtprio > 0) {
        struct sched_param sp; sp.sched_priority = s->rtprio;
        sched_setscheduler(0, SCHED_FIFO, &sp);         // calling thread only
    }
    snd_pcm_uframes_t cp = s->period, pp = s->period;
    unsigned cn = s->periods, pn = s->periods;
    snd_pcm_t* cap  = open_pcm(s->dev, SND_PCM_STREAM_CAPTURE,  s->rate, (unsigned)s->inch, &cp, &cn);
    snd_pcm_t* play = open_pcm(s->dev, SND_PCM_STREAM_PLAYBACK, s->rate, 2,                 &pp, &pn);
    if (!cap || !play) { s->run.store(false); return; }
    snd_pcm_link(cap, play);
    snd_pcm_prepare(cap);

    const snd_pcm_uframes_t P = cp;
    std::vector<uint8_t> cbuf(P * s->inch * 3), pbuf(P * 2 * 3);
    std::vector<float>   infl(P * s->inch), outfl(P * 2);
    memset(pbuf.data(), 0, pbuf.size());
    for (unsigned i = 0; i < s->periods; ++i) snd_pcm_writei(play, pbuf.data(), P);

    while (s->run.load()) {
        snd_pcm_sframes_t n = snd_pcm_readi(cap, cbuf.data(), P);
        if (n < 0) { snd_pcm_recover(cap, (int)n, 1); s->xruns.fetch_add(1); continue; }
        const snd_pcm_uframes_t got = (snd_pcm_uframes_t)n;
        for (snd_pcm_uframes_t f = 0; f < got; ++f)
            for (int c = 0; c < s->inch; ++c)
                infl[f * s->inch + c] = s24_to_f(&cbuf[(f * s->inch + c) * 3]);
        s->eng->RenderLive(infl.data(), (uint32_t)s->inch, s->li, s->ri, outfl.data(), got);
        for (snd_pcm_uframes_t f = 0; f < got; ++f) {
            f_to_s24(outfl[f * 2 + 0], &pbuf[(f * 2 + 0) * 3]);
            f_to_s24(outfl[f * 2 + 1], &pbuf[(f * 2 + 1) * 3]);
        }
        snd_pcm_sframes_t w = snd_pcm_writei(play, pbuf.data(), got);
        if (w < 0) { snd_pcm_recover(play, (int)w, 1); s->xruns.fetch_add(1); }
    }
    snd_pcm_drain(play); snd_pcm_close(play); snd_pcm_close(cap);
}

// ── touch thread: evdev → param ──────────────────────────────────────────────
static void touch_thread(const char* node) {
    Shared* s = &g_sh;
    int fd = open(node, O_RDONLY);
    if (fd < 0) return;                       // UI still runs; keyboard only
    int rx = (TX_MIN + TX_MAX) / 2, ry = (TY_MIN + TY_MAX) / 2, down = 0;
    struct input_event ev;
    while (s->run.load()) {
        fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
        struct timeval tv{0, 200000};
        if (select(fd + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
        if (read(fd, &ev, sizeof ev) != (ssize_t)sizeof ev) continue;
        if      (ev.type == EV_ABS && ev.code == ABS_X) rx = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_Y) ry = ev.value;
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) down = ev.value;
        else continue;

        float nx = float(rx - TX_MIN) / float(TX_MAX - TX_MIN);
        float ny = float(ry - TY_MIN) / float(TY_MAX - TY_MIN);
        if (nx < 0) nx = 0; if (nx > 1) nx = 1;
        if (ny < 0) ny = 0; if (ny > 1) ny = 1;
        if (INVERT_X) nx = 1 - nx;
        if (INVERT_Y) ny = 1 - ny;

        s->tx.store((int)(nx * 1000)); s->ty.store((int)(ny * 1000)); s->tdown.store(down);

        if (down) {
            int bar = (int)(nx * NB); if (bar < 0) bar = 0; if (bar >= NB) bar = NB - 1;
            s->active.store(bar);
            float val = ParamMapNorm(bar, ny);   // curve-aware; ny already top=1
            s->eng->SetParam(bar, val);
        } else {
            s->active.store(-1);
        }
    }
    close(fd);
}

// ── draw one filled-rectangle fader ──────────────────────────────────────────
static void draw_bar(int zone, int ncols, int top, int bot, int b) {
    const int zw = ncols / NB;
    const int bw = (zw - 2 < 14) ? zw - 2 : 14;
    const int x0 = zone * zw + (zw - bw) / 2;
    const int h  = bot - top;
    const float v  = g_sh.eng->GetParam(b);
    float       nv = ParamNormOf(b, v);
    if (nv < 0) nv = 0;
    if (nv > 1) nv = 1;
    const int fill = (int)lrintf(nv * h);
    const bool act = (g_sh.active.load() == b);

    for (int r = 0; r < h; ++r) {
        const bool on = (h - r) <= fill;         // fill from the bottom up
        int attr = on ? (A_REVERSE | (act ? A_BOLD : 0)) : A_DIM;
        attron(attr);
        for (int c = 0; c < bw; ++c) mvaddch(top + r, x0 + c, ' ');
        attroff(attr);
        if (!on) { attron(A_DIM); for (int c = 0; c < bw; ++c) mvaddch(top + r, x0 + c, '.'); attroff(A_DIM); }
    }
    char buf[64];
    mvprintw(bot + 1, x0, "%-*s", bw, ParamTable()[b].label);
    snprintf(buf, sizeof buf, (fabsf(v) >= 100.f) ? "%.0f" : "%.2f", v);
    if (act) attron(A_BOLD);
    mvprintw(bot + 2, x0, "%-*s", bw, buf);
    if (act) attroff(A_BOLD);
}

int main(int argc, char** argv) {
    const char* params_path = "params.txt";
    const char* touch_node  = "/dev/input/event8";
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--dev")    && i+1 < argc) g_sh.dev = argv[++i];
        else if (!strcmp(argv[i], "--period") && i+1 < argc) g_sh.period = (snd_pcm_uframes_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--touch")  && i+1 < argc) touch_node = argv[++i];
        else if (!strcmp(argv[i], "--inpair") && i+2 < argc) { g_sh.li = atoi(argv[++i]) - 1; g_sh.ri = atoi(argv[++i]) - 1; }
        else if (argv[i][0] != '-')                          params_path = argv[i];
    }

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    mlockall(MCL_CURRENT | MCL_FUTURE);

    bool ok = false;
    const Params p0 = load_params(params_path, &ok);
    static Engine eng; eng.Init((float)g_sh.rate, p0);
    g_sh.eng = &eng;

    // Fader b ↔ engine param b (0..kParamCount-1); nothing to resolve.

    std::thread at(audio_thread);
    std::thread tt(touch_thread, touch_node);

    initscr(); cbreak(); noecho(); curs_set(0); nodelay(stdscr, TRUE); keypad(stdscr, TRUE);
    if (has_colors()) { start_color(); use_default_colors(); }

    while (g_sh.run.load()) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q') { g_sh.run.store(false); break; }

        erase();
        const int ncols = COLS, nrows = LINES;
        mvprintw(0, 0, "verb_panel   dev=%s  period=%lu   cpu=%.0f%%  xruns=%ld  peak=%.0fdB   [q]uit",
                 g_sh.dev, (unsigned long)g_sh.period, eng.cpu_load() * 100.0,
                 g_sh.xruns.load(), eng.peak_db());
        mvprintw(1, 0, "touch: x=%4d y=%4d down=%d  bar=%d   (drag a column to set its value)",
                 g_sh.tx.load(), g_sh.ty.load(), g_sh.tdown.load(), g_sh.active.load());
        const int top = 3, bot = nrows - 4;
        if (bot > top) for (int b = 0; b < NB; ++b) draw_bar(b, ncols, top, bot, b);
        refresh();
        napms(30);
    }

    endwin();
    g_sh.run.store(false);
    tt.join(); at.join();
    printf("verb_panel stopped. xruns=%ld\n", g_sh.xruns.load());
    return 0;
}
