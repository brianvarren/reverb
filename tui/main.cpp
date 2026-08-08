// ─────────────────────────────────────────────────────────────────────────────
// verb_tui — realtime tuning bench for the verb reverb.
//
// Opens a live audio stream, runs the reverb through the same block-rate path
// the Daisy firmware will use, and puts every parameter under the arrow keys
// with a playable test-signal keyboard on top.
//
//   build/verb_tui [params.txt] [--in loop.wav] [--device N] [--list-devices]
//
// Nothing here belongs in core/. This is a host-side bench; the DSP it drives
// is byte-for-byte the DSP that ships.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <langinfo.h>
#include <locale.h>
#include <string>
#include <vector>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <ncurses.h>

#include "engine.h"
#include "params.h"

// ─── UI-only values (not reverb parameters) ──────────────────────────────────

struct Ui {
    float gen_type = float(kGenSine);
    float pitch    = 45.f;      // MIDI pitch, A2 = 110 Hz
    float len_ms   = 150.f;
    float level_db = -6.f;
    float repeat   = 0.f;
    float rpt_ms   = 1500.f;
    float file_on  = 0.f;
    float out_db   = -6.f;
    float safety   = 1.f;
};

static const char* kOffOn[]   = { "off", "on" };
static const char* kGenList[] = { "impulse", "noise", "sine", "saw" };

// ─── row model ───────────────────────────────────────────────────────────────

enum RowKind { kRowHeader, kRowParam, kRowUi };

struct Row {
    RowKind            kind;
    const char*        label;
    int                pidx  = -1;        // reverb param index
    float*             uval  = nullptr;   // UI value
    float              lo    = 0.f, hi = 1.f;
    ParamCurve         curve = kLin;
    const char*        unit  = "";
    const char* const* names = nullptr;   // enum labels
    int                nnames = 0;
    int                dec   = 2;
};

static std::vector<Row> g_rows;

static void row_header(const char* t) {
    Row r; r.kind = kRowHeader; r.label = t; g_rows.push_back(r);
}

static void row_param(int idx) {
    const ParamDesc& d = ParamTable()[idx];
    Row r;
    r.kind  = kRowParam;
    r.label = d.label;
    r.pidx  = idx;
    r.lo = d.lo; r.hi = d.hi; r.curve = d.curve; r.unit = d.unit;
    const float span = d.hi - d.lo;
    r.dec = (span >= 100.f) ? 1 : (span >= 10.f ? 1 : (span >= 1.f ? 2 : 3));
    if (d.curve == kPitch) r.dec = 1;
    g_rows.push_back(r);
}

static void row_ui(const char* label, float* v, float lo, float hi,
                   ParamCurve c, const char* unit, int dec,
                   const char* const* names = nullptr, int nnames = 0) {
    Row r;
    r.kind = kRowUi; r.label = label; r.uval = v;
    r.lo = lo; r.hi = hi; r.curve = c; r.unit = unit; r.dec = dec;
    r.names = names; r.nnames = nnames;
    g_rows.push_back(r);
}

static void build_rows(Ui& ui) {
    row_header("SIZE");
    row_param(kP_size);

    row_header("PREDELAY STEREO");
    row_param(kP_pd_sym);

    row_header("CHARACTER");
    row_param(kP_lt_theta);
    row_param(kP_bloom);
    row_param(kP_dmp_eq);

    row_header("MIX");
    row_param(kP_el_mix); row_param(kP_dw_mix);

    row_header("SOURCE / MONITOR");
    row_ui("Signal",      &ui.gen_type, 0.f, 3.f,     kLin, "",   0, kGenList, 4);
    row_ui("Pitch",       &ui.pitch,    12.f, 96.f,   kLin, "st", 1);
    row_ui("Burst len",   &ui.len_ms,   5.f, 4000.f,  kExp, "ms", 0);
    row_ui("Source level",&ui.level_db, -48.f, 0.f,   kLin, "dB", 1);
    row_ui("Repeat",      &ui.repeat,   0.f, 1.f,     kLin, "",   0, kOffOn, 2);
    row_ui("Repeat every",&ui.rpt_ms,   100.f, 8000.f,kExp, "ms", 0);
    row_ui("File loop",   &ui.file_on,  0.f, 1.f,     kLin, "",   0, kOffOn, 2);
    row_ui("Monitor",     &ui.out_db,   -60.f, 12.f,  kLin, "dB", 1);
    row_ui("Safety clip", &ui.safety,   0.f, 1.f,     kLin, "",   0, kOffOn, 2);
}

// ─── value access ────────────────────────────────────────────────────────────

static Engine g_eng;
static bool   g_mute   = false;
static bool   g_bypass = false;

static float row_get(const Row& r) {
    if (r.kind == kRowParam) return g_eng.GetParam(r.pidx);
    if (r.kind == kRowUi)    return *r.uval;
    return 0.f;
}

static void row_set(const Row& r, float v) {
    v = clampf(v, r.lo, r.hi);
    if (r.kind == kRowParam) g_eng.SetParam(r.pidx, v);
    else if (r.kind == kRowUi) *r.uval = v;
}

// Normalised position, used for the bars and for curve-aware stepping.
static float row_norm(const Row& r) {
    const float v = clampf(row_get(r), r.lo, r.hi);
    switch (r.curve) {
        case kExp: return logf(v / r.lo) / logf(r.hi / r.lo);
        case kSq:  return sqrtf((v - r.lo) / (r.hi - r.lo));
        default:   return (v - r.lo) / (r.hi - r.lo);
    }
}

static float row_from_norm(const Row& r, float x) {
    x = clampf(x, 0.f, 1.f);
    switch (r.curve) {
        case kExp: return r.lo * powf(r.hi / r.lo, x);
        case kSq:  return r.lo + (r.hi - r.lo) * x * x;
        default:   return r.lo + (r.hi - r.lo) * x;
    }
}

// mag: 0 = fine, 1 = normal, 2 = coarse
static void row_adjust(const Row& r, int dir, int mag) {
    if (r.names) {                       // enum / bool: step and wrap
        int n = int(lrintf(row_get(r))) + dir;
        const int hi = int(r.hi);
        if (n < 0)  n = hi;
        if (n > hi) n = 0;
        row_set(r, float(n));
        return;
    }
    if (r.curve == kPitch) {             // semitone-domain stepping
        const float step[3] = { 0.1f, 1.f, 6.f };
        float v = row_get(r);
        if (mag == 1) v = (dir > 0) ? floorf(v + 1.f) : ceilf(v - 1.f);  // snap
        else          v += dir * step[mag];
        row_set(r, v);
        return;
    }
    const float step[3] = { 0.0025f, 0.01f, 0.05f };
    row_set(r, row_from_norm(r, row_norm(r) + dir * step[mag]));
}

// ─── formatting ──────────────────────────────────────────────────────────────

static const char* kNoteNames[12] =
    { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

static void note_name(float pitch, char* out, size_t n) {
    const int p = int(lrintf(clampf(pitch, 0.f, 127.f)));
    snprintf(out, n, "%s%d", kNoteNames[((p % 12) + 12) % 12], p / 12 - 1);
}

static void fmt_value(const Row& r, char* out, size_t n) {
    const float v = row_get(r);
    if (r.names) { snprintf(out, n, "%s", r.names[int(clampf(v, 0.f, float(r.nnames-1)))]); return; }
    snprintf(out, n, "%.*f", r.dec, v);
}

static bool g_utf8 = false;

static void bar(char* out, size_t n, float x, int w) {
    const int fill = int(clampf(x, 0.f, 1.f) * float(w) + 0.5f);
    size_t k = 0;
    for (int i = 0; i < w && k + 1 < n; ++i) out[k++] = (i < fill) ? '=' : '.';
    out[k] = 0;
}

// ─── drawing ─────────────────────────────────────────────────────────────────

enum { kColNormal = 1, kColHeader, kColSel, kColDim, kColWarn, kColGood };

static int g_row_w = 38;      // column width, recomputed per frame
static int g_lab_w = 15;      // label field inside it

static void draw_row(int y, int x, const Row& r, bool sel) {
    const int w = g_row_w - 1;
    if (r.kind == kRowHeader) {
        attron(COLOR_PAIR(kColHeader) | A_BOLD);
        mvprintw(y, x, "%-*.*s", w, w, r.label);
        attroff(COLOR_PAIR(kColHeader) | A_BOLD);
        return;
    }
    char val[24], bch[16];
    fmt_value(r, val, sizeof val);
    const int barw = w - g_lab_w - 16;
    bar(bch, sizeof bch, row_norm(r), (barw > 3) ? ((barw > 10) ? 10 : barw) : 3);

    if (sel) attron(COLOR_PAIR(kColSel) | A_BOLD);
    mvprintw(y, x, "%s%-*.*s %8.8s %-3.3s %s",
             sel ? ">" : " ", g_lab_w, g_lab_w, r.label, val, r.unit, bch);
    if (sel) attroff(COLOR_PAIR(kColSel) | A_BOLD);
}

static void put(int y, const char* s) {
    mvprintw(y, 1, "%-*.*s", COLS - 2, COLS - 2, s);
}

// ─── main ────────────────────────────────────────────────────────────────────

static void audio_cb(ma_device* dev, void* out, const void* in, ma_uint32 frames) {
    (void)in;
    static_cast<Engine*>(dev->pUserData)->Render(static_cast<float*>(out), frames);
}

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

static bool save_params(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# verb parameter file — written by verb_tui\n\n");
    const ParamDesc* t = ParamTable();
    for (int i = 0; i < kParamCount; ++i) {
        // Clamp and print with enough precision that the value survives the
        // round-trip — %.5f rounds lt_theta up past pi/4 and trips the clamp.
        const float v = clampf(g_eng.GetParam(i), t[i].lo, t[i].hi);
        fprintf(f, "%-12s= %-11.6f # %s%s%s\n", t[i].key, v,
                t[i].label, t[i].unit[0] ? " — " : "", t[i].unit);
    }
    fclose(f);
    return true;
}

static bool load_wav(const char* path, std::vector<float>& out) {
    drwav w;
    if (!drwav_init_file(&w, path, nullptr)) return false;
    const size_t n  = size_t(w.totalPCMFrameCount);
    const unsigned c = w.channels;
    std::vector<float> il(n * c);
    drwav_read_pcm_frames_f32(&w, n, il.data());
    drwav_uninit(&w);
    out.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out[i*2+0] = il[i*c];
        out[i*2+1] = (c > 1) ? il[i*c+1] : il[i*c];
    }
    return true;
}

static int piano_semitone(int c) {
    switch (c) {
        case 'a': return 0;  case 'w': return 1;  case 's': return 2;  case 'e': return 3;
        case 'd': return 4;  case 'f': return 5;  case 't': return 6;  case 'g': return 7;
        case 'y': return 8;  case 'h': return 9;  case 'u': return 10; case 'j': return 11;
        case 'k': return 12; case 'o': return 13; case 'l': return 14; case 'p': return 15;
        default:  return -1;
    }
}

int main(int argc, char** argv) {
    const char* params_path = "params.txt";
    const char* wav_path    = nullptr;
    int         dev_index   = -1;
    ma_uint32   period      = 256;
    ma_uint32   rate        = 48000;
    bool        list_only   = false;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--in")     && i+1 < argc) wav_path  = argv[++i];
        else if (!strcmp(argv[i], "--device") && i+1 < argc) dev_index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--period") && i+1 < argc) period    = (ma_uint32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate")   && i+1 < argc) rate      = (ma_uint32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--list-devices"))         list_only = true;
        else if (argv[i][0] != '-')                          params_path = argv[i];
    }

    // ── device selection ─────────────────────────────────────────────────────
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) {
        fprintf(stderr, "error: cannot init audio context\n"); return 1;
    }
    ma_device_info* play_info = nullptr; ma_uint32 play_n = 0;
    ma_device_info* cap_info  = nullptr; ma_uint32 cap_n  = 0;
    ma_context_get_devices(&ctx, &play_info, &play_n, &cap_info, &cap_n);

    if (list_only) {
        printf("playback devices:\n");
        for (ma_uint32 i = 0; i < play_n; ++i)
            printf("  %2u  %s%s\n", i, play_info[i].name, play_info[i].isDefault ? "  (default)" : "");
        ma_context_uninit(&ctx);
        return 0;
    }

    // ── params ───────────────────────────────────────────────────────────────
    bool ok = false;
    const Params p0 = load_params(params_path, &ok);

    g_eng.Init(float(rate), p0);
    Ui ui;
    build_rows(ui);

    if (wav_path) {
        std::vector<float> s;
        if (load_wav(wav_path, s)) { g_eng.LoadFile(std::move(s)); ui.file_on = 1.f; }
    }

    // ── audio ────────────────────────────────────────────────────────────────
    ma_device_config cfg   = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format    = ma_format_f32;
    cfg.playback.channels  = 2;
    cfg.sampleRate         = rate;
    cfg.periodSizeInFrames = period;
    cfg.dataCallback       = audio_cb;
    cfg.pUserData          = &g_eng;
    if (dev_index >= 0 && (ma_uint32)dev_index < play_n)
        cfg.playback.pDeviceID = &play_info[dev_index].id;

    ma_device dev;
    bool silent = false;
    if (ma_device_init(&ctx, &cfg, &dev) != MA_SUCCESS) {
        // No usable output (headless box, SSH session, ALSA busy). Fall back to
        // the null backend so the bench still runs — you can read every meter
        // and the decay analysis, you just cannot hear it.
        ma_context_uninit(&ctx);
        ma_backend nb = ma_backend_null;
        if (ma_context_init(&nb, 1, nullptr, &ctx) != MA_SUCCESS ||
            ma_device_init(&ctx, &cfg, &dev) != MA_SUCCESS) {
            fprintf(stderr, "error: cannot open audio device\n");
            return 1;
        }
        silent = true;
    }
    ma_device_start(&dev);

    // ── curses ───────────────────────────────────────────────────────────────
    setlocale(LC_ALL, "");
    g_utf8 = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;

    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    nodelay(stdscr, TRUE);
    if (has_colors()) {
        start_color(); use_default_colors();
        init_pair(kColNormal, -1,            -1);
        init_pair(kColHeader, COLOR_CYAN,    -1);
        init_pair(kColSel,    COLOR_BLACK,   COLOR_CYAN);
        init_pair(kColDim,    COLOR_BLUE,    -1);
        init_pair(kColWarn,   COLOR_RED,     -1);
        init_pair(kColGood,   COLOR_GREEN,   -1);
    }

    int  sel  = 1;                     // first non-header row
    int  oct  = 2;
    int  top  = 0;
    char msg[160]; snprintf(msg, sizeof msg,
        ok ? "loaded %s" : "%s not found — using defaults", params_path);

    Params slot[2];
    for (int s = 0; s < 2; ++s)
        for (int i = 0; i < kParamCount; ++i) *ParamSlot(slot[s], i) = g_eng.GetParam(i);
    int cur_slot = 0;

    std::vector<float> hist(Engine::kHistLen);
    bool running = true;

    auto push_ui = [&]() {
        g_eng.SetSource(int(lrintf(ui.gen_type)), ui.pitch, ui.len_ms, ui.level_db);
        g_eng.SetRepeat(ui.repeat > 0.5f, ui.rpt_ms);
        g_eng.SetOutGain(ui.out_db);
        g_eng.SetSafety(ui.safety > 0.5f);
        g_eng.SetFileOn(ui.file_on > 0.5f);
    };
    push_ui();

    // ── audio restart ─────────────────────────────────────────────────────────
    // Stops the device, optionally reinitialises the DSP (required on rate
    // change because pitch_to_samples and smoother coefficients use fs), then
    // starts a new device with the current rate/period values.
    // Period-only changes preserve the reverb tail; rate changes clear it.
    static const ma_uint32 kRates[]  = { 44100, 48000, 88200, 96000 };
    static const int       kNRates   = 4;

    auto restart_audio = [&](bool reinit_dsp) {
        ma_device_stop(&dev);
        ma_device_uninit(&dev);
        if (reinit_dsp) {
            Params cur;
            for (int i = 0; i < kParamCount; ++i)
                *ParamSlot(cur, i) = g_eng.GetParam(i);
            g_eng.Init(float(rate), cur);
            push_ui();
        }
        ma_device_config cfg2 = ma_device_config_init(ma_device_type_playback);
        cfg2.playback.format    = ma_format_f32;
        cfg2.playback.channels  = 2;
        cfg2.sampleRate         = rate;
        cfg2.periodSizeInFrames = period;
        cfg2.dataCallback       = audio_cb;
        cfg2.pUserData          = &g_eng;
        if (!silent && dev_index >= 0 && (ma_uint32)dev_index < play_n)
            cfg2.playback.pDeviceID = &play_info[dev_index].id;
        if (ma_device_init(&ctx, &cfg2, &dev) != MA_SUCCESS ||
            ma_device_start(&dev) != MA_SUCCESS) {
            snprintf(msg, sizeof msg, "audio restart failed");
        } else {
            snprintf(msg, sizeof msg, "audio: %u Hz / %u frames", dev.sampleRate, period);
        }
    };

    while (running) {
        push_ui();
        g_eng.AnalyseDecay();

        // ── layout ───────────────────────────────────────────────────────────
        const int nrows  = int(g_rows.size());
        const int chrome = 15;                        // title + panels + help
        int       avail  = (LINES - chrome > 4) ? LINES - chrome : 4;

        int cols = (COLS - 2) / 34;
        if (cols < 1) cols = 1;
        if (cols > 3) cols = 3;
        g_row_w = (COLS - 2) / cols;
        if (g_row_w > 46) g_row_w = 46;
        g_lab_w = g_row_w - 23;
        if (g_lab_w < 9)  g_lab_w = 9;
        if (g_lab_w > 16) g_lab_w = 16;

        // Balance the columns when everything fits, rather than filling the
        // first column to the bottom and leaving the last one nearly empty.
        if (nrows <= avail * cols) {
            const int per = (nrows + cols - 1) / cols;
            if (per < avail) avail = (per > 4) ? per : 4;
        }
        const int visible = avail * cols;
        if (sel < top) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;
        if (top > nrows - visible) top = (nrows > visible) ? nrows - visible : 0;
        if (top < 0) top = 0;

        erase();

        // ── title ────────────────────────────────────────────────────────────
        {
            const float cpu = g_eng.cpu_load() * 100.f;
            const int   ovr = g_eng.overruns();
#ifndef VERB_GIT_VERSION
#define VERB_GIT_VERSION "dev"
#endif
            attron(A_BOLD);
            mvprintw(0, 1, "verb  " VERB_GIT_VERSION "  ·  %u Hz  ·  %u fr  ·  CPU %.0f%%  ·  %s",
                     rate, period, cpu, dev.playback.name);
            attroff(A_BOLD);
            // Rate mismatch: driver gave us something different from what we asked for.
            if (!silent && dev.sampleRate != rate) {
                attron(COLOR_PAIR(kColWarn) | A_BOLD);
                char wb[48];
                snprintf(wb, sizeof wb, " RATE MISMATCH: got %u Hz ", dev.sampleRate);
                mvprintw(0, COLS - (int)strlen(wb), "%s", wb);
                attroff(COLOR_PAIR(kColWarn) | A_BOLD);
            } else if (silent) {
                attron(COLOR_PAIR(kColWarn) | A_BOLD);
                mvprintw(0, COLS - 20, " NO AUDIO DEVICE ");
                attroff(COLOR_PAIR(kColWarn) | A_BOLD);
            } else if (ovr > 0) {
                attron(COLOR_PAIR(kColWarn) | A_BOLD);
                char wb[32];
                snprintf(wb, sizeof wb, " %d overrun%s ", ovr, ovr == 1 ? "" : "s");
                mvprintw(0, COLS - (int)strlen(wb), "%s", wb);
                attroff(COLOR_PAIR(kColWarn) | A_BOLD);
            } else if (cpu > 80.f) {
                attron(COLOR_PAIR(kColWarn));
                mvprintw(0, COLS - 10, " CPU HIGH ");
                attroff(COLOR_PAIR(kColWarn));
            }
        }

        // ── params ───────────────────────────────────────────────────────────
        for (int i = 0; i < visible && top + i < nrows; ++i) {
            const int c = i / avail, rr = i % avail;
            draw_row(2 + rr, 1 + c * g_row_w, g_rows[top + i], (top + i) == sel);
        }
        if (top + visible < nrows || top > 0) {
            attron(COLOR_PAIR(kColDim));
            mvprintw(1, COLS - 12, "%d-%d/%d", top + 1,
                     (top + visible < nrows) ? top + visible : nrows, nrows);
            attroff(COLOR_PAIR(kColDim));
        }

        int y = 2 + avail;
        mvhline(y++, 1, ACS_HLINE, COLS - 2);

        // ── derived engineering readout ──────────────────────────────────────
        // All geometry comes from Params::Derive() — single source of truth.
        const float fs = float(dev.sampleRate);
        char line[512];
        Params dp;
        for (int i = 0; i < kParamCount; ++i)
            *ParamSlot(dp, i) = g_eng.GetParam(i);
        dp.Derive();
        {
            const float erL  = dp.er_sz - dp.er_sym;
            const float erR  = dp.er_sz + dp.er_sym;
            const float off[3] = { 8.f, 4.f, 0.f };
            char a[128] = {0}, b[128] = {0};
            for (int i = 0; i < 3; ++i) {
                char t[32];
                snprintf(t, sizeof t, "%s%.1f", i ? "/" : "",
                         pitch_to_samples(erL + off[i], fs) * 1000.f / fs);
                strncat(a, t, sizeof(a) - strlen(a) - 1);
                snprintf(t, sizeof t, "%s%.1f", i ? "/" : "",
                         pitch_to_samples(erR + off[i], fs) * 1000.f / fs);
                strncat(b, t, sizeof(b) - strlen(b) - 1);
            }
            snprintf(line, sizeof line,
                     "EARLY  predelay %.1f/%.1f ms   allpass L %s ms   R %s ms",
                     dp.predelay_ms - dp.pd_sym, dp.predelay_ms + dp.pd_sym, a, b);
            put(y++, line);
        }
        {
            const float szL  = dp.lt_sz - dp.lt_sym;
            const float szR  = dp.lt_sz + dp.lt_sym;
            float D = 0.f;
            for (int k = 0; k < 3; ++k) {
                const float o = float(9 - 3 * k);
                D += 0.5f * (pitch_to_samples(szL + o, fs) + pitch_to_samples(szR + o, fs));
            }
            D += 0.5f * (pitch_to_samples(szL, fs) + pitch_to_samples(szR, fs));
            const float rt = dp.rt60_s;
            const float bl = dp.bloom;
            float fb = powf(10.f, -3.f * (D / fs) / (rt * bl));
            const bool capped = fb > 0.995f;
            if (capped) fb = 0.995f;
            snprintf(line, sizeof line,
                     "LATE   loop %.1f ms   fb %.5f%s   rt60 %.1f s   target decay %.1f s",
                     D * 1000.f / fs, fb, capped ? " CAP" : "", rt, rt * bl);
            put(y++, line);
        }
        {
            constexpr float kHpHz = 4186.f, kLpHz = 65.f;  // pitch 108, pitch 36
            const float eq = g_eng.GetParam(kP_dmp_eq);
            const float hb_loop = fminf(eq *  18.f,    0.f);
            const float lb_loop = fminf(eq * -25.92f,  0.f);
            const float hb_out  = eq *  36.f;
            const float lb_out  = eq * -36.f;
            snprintf(line, sizeof line,
                     "EQ     loop %.0f/%+.1f dB  %.0f/%+.1f dB    out %.0f/%+.1f dB  %.0f/%+.1f dB",
                     kHpHz, hb_loop, kLpHz, lb_loop, kHpHz, hb_out, kLpHz, lb_out);
            put(y++, line);
        }

        // ── meters ───────────────────────────────────────────────────────────
        {
            char nn[16]; note_name(ui.pitch, nn, sizeof nn);
            char t60s[24], edts[24];
            const float prog = g_eng.capture_progress();
            if (g_eng.t60() > 0.f)  snprintf(t60s, sizeof t60s, "%.2f s", g_eng.t60());
            else if (prog >= 0.f)   snprintf(t60s, sizeof t60s, "%2.0f%%..", prog * 100.f);
            else                    snprintf(t60s, sizeof t60s, "--");
            if (g_eng.edt() > 0.f) snprintf(edts, sizeof edts, "%.2f s", g_eng.edt());
            else                   snprintf(edts, sizeof edts, "--");
            snprintf(line, sizeof line,
                     "SRC    %-7s %s %.0f Hz  oct %d  |  peak %6.1f dB  rms %6.1f dB  |  "
                     "T60 %s   EDT %s",
                     kGenList[int(lrintf(ui.gen_type))], nn, pitch_to_hz(ui.pitch), oct,
                     g_eng.peak_db(), g_eng.rms_db(), t60s, edts);
            put(y++, line);
        }
        {
            // Tail envelope, 20 ms per cell, 0 dB at top and −60 dB at the floor.
            g_eng.CopyHistory(hist.data());
            static const char    ramp[9]  = " .:-=+*#";
#ifdef VERB_TUI_WIDE
            static const wchar_t blocks[9] = L" \u2581\u2582\u2583\u2585\u2586\u2587\u2588";
#endif
            const int w = (COLS - 10 < Engine::kHistLen) ? COLS - 10 : Engine::kHistLen;
            // Normalised to the loudest cell on screen: this is a decay *shape*
            // readout, so the useful thing is the curve, not the absolute level.
            float top_db = -120.f;
            for (int i = 0; i < w; ++i)
                if (hist[Engine::kHistLen - w + i] > top_db)
                    top_db = hist[Engine::kHistLen - w + i];
            mvprintw(y, 1, "TAIL   ");
            const bool quiet = (top_db < -100.f);      // nothing to show yet
            for (int i = 0; i < w && !quiet; ++i) {
                const float d = hist[Engine::kHistLen - w + i] - top_db;
                int lvl = int(((d + 60.f) / 60.f) * 7.f + 0.5f);   // 60 dB span
                if (lvl < 0) lvl = 0;
                if (lvl > 7) lvl = 7;
#ifdef VERB_TUI_WIDE
                if (g_utf8) {
                    const wchar_t ws[2] = { blocks[lvl], 0 };
                    mvaddwstr(y, 8 + i, ws);
                    continue;
                }
#endif
                mvaddch(y, 8 + i, (chtype)ramp[lvl]);
            }
            ++y;
        }

        // ── status flags ─────────────────────────────────────────────────────
        {
            const int nb = g_eng.blowups();
            snprintf(line, sizeof line,
                     "%s%s%s%s%s%s",
                     ui.repeat  > 0.5f ? "[REPEAT] "  : "",
                     ui.file_on > 0.5f ? "[FILE] "    : "",
                     ui.safety  > 0.5f ? "" : "[NO CLIP GUARD] ",
                     g_bypass ? "[BYPASS] " : "",
                     g_mute   ? "[MUTE] "   : "",
                     nb ? "[NaN — auto-cleared] " : "");
            if (nb) attron(COLOR_PAIR(kColWarn) | A_BOLD);
            put(y, line);
            if (nb) attroff(COLOR_PAIR(kColWarn) | A_BOLD);
            mvprintw(y, (int)strlen(line) + 2, "%s", msg);
            ++y;
        }

        mvhline(y++, 1, ACS_HLINE, COLS - 2);
        attron(COLOR_PAIR(kColDim));
        put(y++, "up/dn select   left/right adjust (shift=coarse, Home=fine)   "
                 "Enter type value   Del reset");
        put(y++, "SPACE trigger   a w s e d f t g y h u j k = play   z/x octave   1-4 signal   "
                 "r repeat");
        put(y++, "c clear   m mute   b bypass   i file   v A/B   F2 save   F3 reload   F4 rate   [ ] period   q quit");
        attroff(COLOR_PAIR(kColDim));

        refresh();

        // ── input ────────────────────────────────────────────────────────────
        napms(25);
        int ch = getch();
        while (ch != ERR) {
            bool handled = true;
            switch (ch) {
                case 'q': case 'Q': running = false; break;

                case KEY_UP:
                    do { --sel; } while (sel > 0 && g_rows[sel].kind == kRowHeader);
                    if (sel < 0) sel = 0;
                    if (g_rows[sel].kind == kRowHeader)
                        do { ++sel; } while (sel < nrows && g_rows[sel].kind == kRowHeader);
                    break;
                case KEY_DOWN:
                    do { ++sel; } while (sel < nrows && g_rows[sel].kind == kRowHeader);
                    if (sel >= nrows) sel = nrows - 1;
                    if (g_rows[sel].kind == kRowHeader)
                        do { --sel; } while (sel > 0 && g_rows[sel].kind == kRowHeader);
                    break;

                case KEY_LEFT:   row_adjust(g_rows[sel], -1, 1); break;
                case KEY_RIGHT:  row_adjust(g_rows[sel], +1, 1); break;
                case KEY_SLEFT:  row_adjust(g_rows[sel], -1, 2); break;
                case KEY_SRIGHT: row_adjust(g_rows[sel], +1, 2); break;
                case KEY_PPAGE:  row_adjust(g_rows[sel], +1, 2); break;
                case KEY_NPAGE:  row_adjust(g_rows[sel], -1, 2); break;
                case ',':        row_adjust(g_rows[sel], -1, 0); break;
                case '.':        row_adjust(g_rows[sel], +1, 0); break;

                case KEY_DC:     // reset to default
                    if (g_rows[sel].kind == kRowParam)
                        row_set(g_rows[sel], ParamTable()[g_rows[sel].pidx].def);
                    break;

                case '\n': case KEY_ENTER: {
                    echo(); curs_set(1); nodelay(stdscr, FALSE);
                    mvprintw(LINES - 1, 1, "%-*s", COLS - 2, "");
                    mvprintw(LINES - 1, 1, "%s = ", g_rows[sel].label);
                    char buf[64] = {0};
                    getnstr(buf, sizeof buf - 1);
                    noecho(); curs_set(0); nodelay(stdscr, TRUE);
                    if (buf[0]) row_set(g_rows[sel], strtof(buf, nullptr));
                    break;
                }

                case ' ': g_eng.Trigger(); break;

                case '1': ui.gen_type = 0.f; break;
                case '2': ui.gen_type = 1.f; break;
                case '3': ui.gen_type = 2.f; break;
                case '4': ui.gen_type = 3.f; break;

                case 'z': if (oct > -1) --oct; break;
                case 'x': if (oct <  7) ++oct; break;

                case 'r': ui.repeat = (ui.repeat > 0.5f) ? 0.f : 1.f; break;
                case 'i': ui.file_on = (ui.file_on > 0.5f) ? 0.f : 1.f;
                          if (!g_eng.has_file()) { ui.file_on = 0.f;
                              snprintf(msg, sizeof msg, "no --in file loaded"); }
                          break;
                case 'c': g_eng.Panic(); snprintf(msg, sizeof msg, "cleared"); break;
                case 'm': g_mute = !g_mute;   g_eng.SetMute(g_mute);     break;
                case 'b': g_bypass = !g_bypass; g_eng.SetBypass(g_bypass); break;

                case 'v': {   // A/B: stash the live set, recall the other one
                    for (int i = 0; i < kParamCount; ++i)
                        *ParamSlot(slot[cur_slot], i) = g_eng.GetParam(i);
                    cur_slot ^= 1;
                    for (int i = 0; i < kParamCount; ++i)
                        g_eng.SetParam(i, *ParamSlot(slot[cur_slot], i));
                    snprintf(msg, sizeof msg, "recalled %c", cur_slot ? 'B' : 'A');
                    break;
                }

                case KEY_F(2):
                    snprintf(msg, sizeof msg, save_params(params_path)
                             ? "saved %s" : "could not write %s", params_path);
                    break;
                case KEY_F(3): {
                    bool k = false;
                    const Params np = load_params(params_path, &k);
                    if (k) { for (int i = 0; i < kParamCount; ++i)
                                 g_eng.SetParam(i, *ParamSlot(const_cast<Params&>(np), i));
                             snprintf(msg, sizeof msg, "reloaded %s", params_path); }
                    break;
                }
                case KEY_F(5):
                    for (int i = 0; i < kParamCount; ++i)
                        g_eng.SetParam(i, ParamTable()[i].def);
                    snprintf(msg, sizeof msg, "defaults restored");
                    break;

                case '[':   // halve period
                    if (period > 64) { period /= 2; restart_audio(false); }
                    break;
                case ']':   // double period
                    if (period < 4096) { period *= 2; restart_audio(false); }
                    break;

                case KEY_F(4): {  // cycle sample rate (reinits DSP)
                    int idx = 0;
                    for (int i = 0; i < kNRates; ++i)
                        if (kRates[i] == rate) { idx = i; break; }
                    rate = kRates[(idx + 1) % kNRates];
                    restart_audio(true);
                    break;
                }

                default: handled = false; break;
            }

            if (!handled) {
                const int st = piano_semitone(ch);
                if (st >= 0) {
                    ui.pitch = clampf(float((oct + 1) * 12 + st), 12.f, 96.f);
                    push_ui();
                    g_eng.Trigger();
                }
            }
            ch = getch();
        }
    }

    endwin();
    ma_device_uninit(&dev);
    ma_context_uninit(&ctx);
    return 0;
}
