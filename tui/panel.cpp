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
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "engine.h"
#include "params.h"

// ── touch calibration (native landscape; flip here if axes are mirrored) ──────
static int  TX_MIN = 128,  TX_MAX = 3932;
static int  TY_MIN = 300,  TY_MAX = 4001;
static bool INVERT_X = false;
static bool INVERT_Y = true;   // screen top = max value

// ── faders: one per engine parameter + a virtual SLEW fader at the end ────────
// Faders 0..kParamCount-1 drive engine params (ranges/curve/label from the
// table, so promoting a baked constant makes a slider appear automatically).
// The last fader (SLEW_IDX) is the global param glide time, 0..kSlewMax s.
static const float kSlewMax = 30.f;
static const int   SLEW_IDX = kParamCount;
static const int   NB       = kParamCount + 1;

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
    // touch state: fx/fy are screen fractions in permille (fx 0=left..1000=right,
    // fy 0=top..1000=bottom); active = param index being dragged; page = current tab.
    std::atomic<int> tx{0}, ty{0}, tdown{0}, active{-1}, page{0};
};

// Faders per page (big targets for a resistive stylus). 22 params → 3 pages.
static const int PAGE_SIZE = 8;
static Shared g_sh;

// Virtual-fader accessors so draw/hit-test treat SLEW like any other fader.
static float       fader_norm(int b)  { return (b == SLEW_IDX) ? g_sh.eng->GetSlew() / kSlewMax
                                                               : ParamNormOf(b, g_sh.eng->GetParam(b)); }
static void        fader_set(int b, float f) { if (b == SLEW_IDX) g_sh.eng->SetSlew(f * kSlewMax);
                                               else g_sh.eng->SetParam(b, ParamMapNorm(b, f)); }
static float       fader_val(int b)   { return (b == SLEW_IDX) ? g_sh.eng->GetSlew() : g_sh.eng->GetParam(b); }
static const char* fader_label(int b) { return (b == SLEW_IDX) ? "SLEW" : ParamTable()[b].label; }
static const char* fader_unit(int b)  { return (b == SLEW_IDX) ? "s"    : ParamTable()[b].unit; }

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

// Resolve the ALSA card index whose name contains `needle` (e.g. "Tonverk").
// Returns -1 if absent — so we never silently fall back to the internal codec.
static int find_card_by_name(const char* needle) {
    int card = -1, found = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char* name = nullptr;
        if (snd_card_get_name(card, &name) == 0 && name) {
            if (strstr(name, needle)) found = card;
            free(name);
        }
        if (found >= 0) break;
    }
    return found;
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

        // Publish screen fractions (fy: 0=top, 1=bottom). The UI thread owns the
        // layout, so it does the hit-testing (tab bar vs fader) and sets params.
        s->tx.store((int)(nx * 1000));
        s->ty.store((int)((1.f - ny) * 1000));
        s->tdown.store(down);
    }
    close(fd);
}

// ── tab bar: nPages big buttons across the top ───────────────────────────────
static void draw_tabs(int ncols, int lo, int hi, int nPages, int page) {
    const int tw = ncols / nPages;
    for (int p = 0; p < nPages; ++p) {
        const bool cur = (p == page);
        const int  x0  = p * tw;
        char lbl[32];
        snprintf(lbl, sizeof lbl, "PAGE %d/%d", p + 1, nPages);
        const int lx = x0 + (tw - (int)strlen(lbl)) / 2;
        int attr = cur ? A_REVERSE | A_BOLD : A_DIM;
        attron(attr);
        for (int r = lo; r < hi; ++r)
            for (int c = 0; c < tw - 1; ++c) mvaddch(r, x0 + c, ' ');
        attroff(attr);
        attron(cur ? A_BOLD : A_DIM);
        mvprintw((lo + hi) / 2, lx, "%s", lbl);
        attroff(cur ? A_BOLD : A_DIM);
    }
}

// ── one big filled-rectangle fader (slot of `count` across the width) ─────────
static void draw_bar(int slot, int count, int ncols, int top, int bot, int pidx) {
    const int zw = ncols / count;
    const int bw = (zw - 2 > 3) ? zw - 2 : 3;      // as wide as the zone allows
    const int x0 = slot * zw + (zw - bw) / 2;
    const int h  = bot - top;
    if (h < 1) return;
    const float v  = fader_val(pidx);
    float       nv = fader_norm(pidx);
    if (nv < 0) nv = 0;
    if (nv > 1) nv = 1;
    const int  fill = (int)lrintf(nv * h);
    const bool act  = (g_sh.active.load() == pidx);

    for (int r = 0; r < h; ++r) {
        const bool on = (h - r) <= fill;           // fill from the bottom up
        int attr = on ? (A_REVERSE | (act ? A_BOLD : 0)) : A_DIM;
        attron(attr);
        for (int c = 0; c < bw; ++c) mvaddch(top + r, x0 + c, on ? ' ' : '.');
        attroff(attr);
    }
    char buf[64];
    if (act) attron(A_BOLD);
    mvprintw(bot + 1, x0, "%-*.*s", bw, bw, fader_label(pidx));
    snprintf(buf, sizeof buf, (fabsf(v) >= 100.f) ? "%.0f%s" : "%.2f%s", v, fader_unit(pidx));
    mvprintw(bot + 2, x0, "%-*.*s", bw, bw, buf);
    if (act) attroff(A_BOLD);
}

// ── per-parameter help text (how it affects the sound) ───────────────────────
static const char* param_desc(int b) {
    if (b == SLEW_IDX)
        return "Glide time for every slider: how long a parameter takes to reach "
               "a new value after you move its fader. 0 = instant (also removes "
               "zipper noise on fine moves). Raise it for slow morphs between settings.";
    const char* k = ParamTable()[b].key;
    if (!strcmp(k, "lt_sz"))       return "Overall size of the late reverb. Lower pitch = longer delay lines = a bigger, slower space; higher = tighter and smaller.";
    if (!strcmp(k, "decay"))       return "How long the tail rings out. Exponential: ~0.3 s at minimum up to ~60 s at maximum.";
    if (!strcmp(k, "out_freq"))    return "Centre frequency of the output band-pass. Lower is darker/duller, higher is brighter and thinner.";
    if (!strcmp(k, "dw_mix"))      return "Dry/wet balance. 0 = input only, 1 = reverb only. For a send/return (like this Tonverk setup) keep it at 1.";
    if (!strcmp(k, "predelay_ms")) return "Silence before the reverb begins. More predelay separates the dry sound from the tail, adding clarity and a sense of distance.";
    if (!strcmp(k, "pd_sym"))      return "Left/right offset of the predelay. Widens the stereo image of the reverb's onset.";
    if (!strcmp(k, "er_sz"))       return "Size of the early reflections (the first echoes before the tail). Sets the initial room signature and texture.";
    if (!strcmp(k, "er_sym"))      return "Left/right spread of the early reflections. More = a wider early stereo image.";
    if (!strcmp(k, "er_dffs"))     return "Early diffusion. Higher = denser, smoother early reflections; lower = more discrete, echoey.";
    if (!strcmp(k, "mod_hz"))      return "Rate of the tail's modulation LFOs. Slow = gentle drift; fast = shimmer/chorus.";
    if (!strcmp(k, "mod_ms"))      return "Depth of delay-line modulation. More smears the loop resonances, keeping the tail lush and less metallic.";
    if (!strcmp(k, "chaos"))       return "How much the modulation LFO rates spread apart. Higher = more decorrelated, three-dimensional movement.";
    if (!strcmp(k, "lt_sym"))      return "Left/right size offset of the late loop. More = a wider stereo tail (it also shifts the delay ratios).";
    if (!strcmp(k, "lt_theta"))    return "Rotation angle of the late diffuser's mixing matrix. ~0.79 (pi/4) is balanced; other values change the diffusion character.";
    if (!strcmp(k, "lt_dffs"))     return "Late diffusion. Higher = a smoother, denser tail; lower = more individual echoes in the tail.";
    if (!strcmp(k, "sat"))         return "Saturation drive inside the loop. Adds harmonics and warmth and gently compresses peaks as the tail builds.";
    if (!strcmp(k, "lt_hf_hz"))    return "Corner frequency for high-frequency damping. Content above this darkens as the tail decays.";
    if (!strcmp(k, "lt_hf_ratio")) return "How much faster the highs decay than the overall tail. Lower = darker tail; 1 = highs decay with everything else.";
    if (!strcmp(k, "lt_hf_max_s")) return "Ceiling on the HF decay time, so even very long tails still darken instead of ringing bright forever.";
    if (!strcmp(k, "out_width"))   return "Width of the output band-pass in octaves. Narrow = focused/resonant; wide = full-range.";
    if (!strcmp(k, "hp_hz"))       return "High-shelf frequency in the early section (not in the loop). Shapes the brightness of the early reflections.";
    if (!strcmp(k, "hp_db"))       return "Gain of that early high shelf. Negative darkens the early reflections.";
    return "(no description)";
}

// ── centered info overlay for one parameter ──────────────────────────────────
static void draw_info(int b, int ncols, int nrows) {
    const char* txt = param_desc(b);
    int bw = ncols - 8; if (bw > 62) bw = 62; if (bw < 24) bw = 24;
    const int tw = bw - 4;                       // wrap width inside margins

    std::vector<std::string> lines;
    std::string cur, word;
    auto flush = [&](){
        if (word.empty()) return;
        if (cur.empty()) cur = word;
        else if ((int)(cur.size() + 1 + word.size()) <= tw) cur += " " + word;
        else { lines.push_back(cur); cur = word; }
        word.clear();
    };
    for (const char* p = txt; ; ++p) {
        if (*p == ' ' || *p == '\0') { flush(); if (*p == '\0') break; }
        else word += *p;
    }
    if (!cur.empty()) lines.push_back(cur);

    const int bh = (int)lines.size() + 5;        // title, gap, lines, gap, hint
    int x0 = (ncols - bw) / 2, y0 = (nrows - bh) / 2;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;

    char title[80];
    const float v = fader_val(b);
    snprintf(title, sizeof title, "%s = %.2f %s", fader_label(b), v, fader_unit(b));

    attron(A_REVERSE);
    for (int r = 0; r < bh; ++r)
        for (int c = 0; c < bw; ++c) mvaddch(y0 + r, x0 + c, ' ');
    attron(A_BOLD); mvprintw(y0 + 1, x0 + 2, "%.*s", bw - 4, title); attroff(A_BOLD);
    for (int i = 0; i < (int)lines.size(); ++i)
        mvprintw(y0 + 3 + i, x0 + 2, "%s", lines[i].c_str());
    mvprintw(y0 + bh - 2, x0 + 2, "tap anywhere to close");
    attroff(A_REVERSE);
}

int main(int argc, char** argv) {
    const char* params_path = "params.txt";
    const char* touch_node  = "/dev/input/event8";
    bool        dev_set     = false;     // explicit --dev overrides auto-detect
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--dev")    && i+1 < argc) { g_sh.dev = argv[++i]; dev_set = true; }
        else if (!strcmp(argv[i], "--period") && i+1 < argc) g_sh.period = (snd_pcm_uframes_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--touch")  && i+1 < argc) touch_node = argv[++i];
        else if (!strcmp(argv[i], "--inpair") && i+2 < argc) { g_sh.li = atoi(argv[++i]) - 1; g_sh.ri = atoi(argv[++i]) - 1; }
        else if (argv[i][0] != '-')                          params_path = argv[i];
    }

    // Bind the Tonverk by NAME, never a hardcoded index. If it is not present,
    // refuse to open any device — do NOT fall back to the internal speaker.
    if (!dev_set) {
        int c = find_card_by_name("Tonverk");
        if (c < 0) {
            fprintf(stderr, "verb_panel: Tonverk not found — not opening audio "
                            "(won't touch the internal speaker). Connect it in "
                            "USB AUDIO/MIDI mode and relaunch.\n");
            return 2;
        }
        static char devbuf[32];
        snprintf(devbuf, sizeof devbuf, "hw:%d,0", c);
        g_sh.dev = devbuf;
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

    const int nPages = (NB + PAGE_SIZE - 1) / PAGE_SIZE;
    int  info     = -1;                 // param whose info overlay is open (-1 none)
    bool was_down = false;
    while (g_sh.run.load()) {
        int page = g_sh.page.load();
        int ch = getch();
        if      (ch == 'q' || ch == 'Q')       { g_sh.run.store(false); break; }
        else if (ch == ']' || ch == KEY_RIGHT) { if (page < nPages - 1) g_sh.page.store(page = page + 1); info = -1; }
        else if (ch == '[' || ch == KEY_LEFT)  { if (page > 0)          g_sh.page.store(page = page - 1); info = -1; }
        else if (ch >= '1' && ch <= '9')       { int p = ch - '1'; if (p < nPages) g_sh.page.store(page = p); info = -1; }

        const int ncols = COLS, nrows = LINES;
        const int tab_lo = 1, tab_hi = 5;            // tab bar occupies rows 1..4
        const int fad_top = 6, fad_bot = nrows - 4;  // big faders below it

        const bool down  = g_sh.tdown.load();
        const bool press = down && !was_down;        // rising edge = a fresh tap

        // ── touch hit-test (UI thread owns the layout) ──
        if (info >= 0) {
            if (press) info = -1;                    // overlay open: a tap dismisses it (consumed)
            g_sh.active.store(-1);
        } else if (down) {
            const float fx  = g_sh.tx.load() / 1000.f;
            const float fy  = g_sh.ty.load() / 1000.f;
            const int   row = (int)(fy * nrows);
            int cnt = NB - page * PAGE_SIZE; if (cnt > PAGE_SIZE) cnt = PAGE_SIZE;
            int slot = (int)(fx * cnt); if (slot < 0) slot = 0; if (slot >= cnt) slot = cnt - 1;
            if (row >= tab_lo && row < tab_hi && nPages > 1) {
                int p = (int)(fx * nPages); if (p < 0) p = 0; if (p >= nPages) p = nPages - 1;
                g_sh.page.store(page = p);
                g_sh.active.store(-1);
            } else if (row >= fad_top && row <= fad_bot && fad_bot > fad_top) {
                const int pidx = page * PAGE_SIZE + slot;
                float frac = float(fad_bot - row) / float(fad_bot - fad_top);
                if (frac < 0) frac = 0; if (frac > 1) frac = 1;
                fader_set(pidx, frac);
                g_sh.active.store(pidx);
            } else if (row > fad_bot) {               // label/value strip → info on tap
                if (press) info = page * PAGE_SIZE + slot;
                g_sh.active.store(-1);
            } else {
                g_sh.active.store(-1);
            }
        } else {
            g_sh.active.store(-1);
        }
        was_down = down;

        int count = NB - page * PAGE_SIZE; if (count > PAGE_SIZE) count = PAGE_SIZE;

        // ── draw ──
        erase();
        mvprintw(0, 0, "verb_panel  %s per=%lu  cpu=%.0f%%  xruns=%ld  peak=%.0fdB   page %d/%d   [ ] tab   tap label = info   q quit",
                 g_sh.dev, (unsigned long)g_sh.period, eng.cpu_load() * 100.0,
                 g_sh.xruns.load(), eng.peak_db(), page + 1, nPages);
        draw_tabs(ncols, tab_lo, tab_hi, nPages, page);
        if (fad_bot > fad_top)
            for (int slot2 = 0; slot2 < count; ++slot2)
                draw_bar(slot2, count, ncols, fad_top, fad_bot, page * PAGE_SIZE + slot2);
        if (info >= 0 && info < NB) draw_info(info, ncols, nrows);
        refresh();
        napms(30);
    }

    endwin();
    g_sh.run.store(false);
    tt.join(); at.join();
    printf("verb_panel stopped. xruns=%ld\n", g_sh.xruns.load());
    return 0;
}
