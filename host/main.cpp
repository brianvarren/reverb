#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "params.h"
#include "early.h"
#include "late.h"

// ── params parsing ────────────────────────────────────────────────────────────

static void apply_param(Params& p, const std::string& key, float val) {
    if      (key == "predelay_ms") p.predelay_ms = val;
    else if (key == "pd_sym")      p.pd_sym      = val;
    else if (key == "er_sz")       p.er_sz       = val;
    else if (key == "er_sym")      p.er_sym      = val;
    else if (key == "er_dffs")     p.er_dffs     = val;
    else if (key == "lt_sz")       p.lt_sz       = val;
    else if (key == "lt_sym")      p.lt_sym      = val;
    else if (key == "lt_theta")    p.lt_theta    = val;
    else if (key == "rt60_s")      p.rt60_s      = val;
    else if (key == "bloom")       p.bloom       = val;
    else if (key == "mod_ms")      p.mod_ms      = val;
    else if (key == "mod_hz")      p.mod_hz      = val;
    else if (key == "dmp_hf")      p.dmp_hf      = val;
    else if (key == "dmp_hb")      p.dmp_hb      = val;
    else if (key == "dmp_lf")      p.dmp_lf      = val;
    else if (key == "dmp_lb")      p.dmp_lb      = val;
    else if (key == "eo_hf")       p.eo_hf       = val;
    else if (key == "eo_hb")       p.eo_hb       = val;
    else if (key == "eo_lf")       p.eo_lf       = val;
    else if (key == "eo_lb")       p.eo_lb       = val;
    else if (key == "el_mix")      p.el_mix      = val;
    else if (key == "dw_mix")      p.dw_mix      = val;
    else fprintf(stderr, "warning: unknown param '%s'\n", key.c_str());
}

static Params parse_params(const char* path) {
    Params p;
    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "warning: params file '%s' not found, using defaults\n", path);
        return p;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val_str = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(key);
        trim(val_str);
        if (key.empty() || val_str.empty()) continue;
        try { apply_param(p, key, std::stof(val_str)); }
        catch (...) { fprintf(stderr, "warning: bad value for '%s'\n", key.c_str()); }
    }
    return p;
}

// ── stats ─────────────────────────────────────────────────────────────────────

static void print_stats(const float* L, const float* R, size_t n) {
    float peak = 0.f, sum2 = 0.f;
    int bad = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(L[i]) || !std::isfinite(R[i])) { ++bad; continue; }
        peak  = std::max(peak, std::max(std::abs(L[i]), std::abs(R[i])));
        sum2 += L[i]*L[i] + R[i]*R[i];
    }
    float rms = (n > 0) ? std::sqrt(sum2 / float(2 * n)) : 0.f;
    fprintf(stderr, "frames=%-8zu  peak=%.6f  rms=%.6f  nan/inf=%d\n",
            n, peak, rms, bad);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    float impulse_secs = -1.f;
    float noise_secs   = -1.f;
    std::vector<const char*> pos;

    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--impulse") == 0 && i+1 < argc) impulse_secs = std::stof(argv[++i]);
        else if (std::strcmp(argv[i], "--noise")   == 0 && i+1 < argc) noise_secs   = std::stof(argv[++i]);
        else pos.push_back(argv[i]);
    }

    const bool gen_mode = impulse_secs >= 0.f || noise_secs >= 0.f;
    const size_t need_pos = gen_mode ? 2 : 3;
    if (pos.size() < need_pos) {
        fprintf(stderr,
            "usage: verb in.wav params.txt out.wav\n"
            "       verb --impulse N params.txt out.wav\n"
            "       verb --noise N params.txt out.wav\n");
        return 1;
    }

    const char* in_path     = gen_mode ? nullptr : pos[0];
    const char* params_path = gen_mode ? pos[0]  : pos[1];
    const char* out_path_in = gen_mode ? pos[1]  : pos[2];

    // Auto-number: if the requested output path already exists, insert _NNN before
    // the extension so successive renders don't overwrite each other.
    std::string out_owned;
    const char* out_path;
    {
        std::string base(out_path_in);
        if (std::ifstream(base).good()) {
            auto dot = base.rfind('.');
            std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
            std::string ext  = (dot == std::string::npos) ? ""   : base.substr(dot);
            int n = 1;
            do {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "_%03d", n++);
                out_owned = stem + buf + ext;
            } while (std::ifstream(out_owned).good());
            fprintf(stderr, "output: %s\n", out_owned.c_str());
            out_path = out_owned.c_str();
        } else {
            out_path = out_path_in;
        }
    }

    // params are re-read on every invocation (no cache)
    const Params p = parse_params(params_path);

    constexpr float kFS = 48000.f;
    const size_t tail_frames = (size_t)std::ceil(p.rt60_s * p.bloom * 1.5f * kFS);

    // ── load / generate input ─────────────────────────────────────────────────
    std::vector<float> L, R;

    if (impulse_secs >= 0.f) {
        size_t n = (size_t)std::ceil(impulse_secs * kFS);
        L.assign(n, 0.f);  R.assign(n, 0.f);
        if (n > 0) { L[0] = 1.f;  R[0] = 1.f; }
    } else if (noise_secs >= 0.f) {
        size_t n = (size_t)std::ceil(noise_secs * kFS);
        L.resize(n);  R.resize(n);
        for (size_t i = 0; i < n; ++i) {
            L[i] = (float)rand() / RAND_MAX * 2.f - 1.f;
            R[i] = (float)rand() / RAND_MAX * 2.f - 1.f;
        }
    } else {
        drwav wav;
        if (!drwav_init_file(&wav, in_path, nullptr)) {
            fprintf(stderr, "error: cannot open '%s'\n", in_path);
            return 1;
        }
        const size_t frames = (size_t)wav.totalPCMFrameCount;
        const uint32_t ch   = wav.channels;
        std::vector<float> interleaved(frames * ch);
        drwav_read_pcm_frames_f32(&wav, frames, interleaved.data());
        drwav_uninit(&wav);
        L.resize(frames);  R.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            L[i] = interleaved[i * ch];
            R[i] = (ch > 1) ? interleaved[i * ch + 1] : interleaved[i * ch];
        }
    }

    // append tail silence so the decay is fully captured
    L.resize(L.size() + tail_frames, 0.f);
    R.resize(R.size() + tail_frames, 0.f);
    const size_t total = L.size();

    // ── DSP ───────────────────────────────────────────────────────────────────
    constexpr size_t kEarlyBuf = 8192;  // ~170 ms headroom at 48 kHz
    constexpr size_t kDiffBuf  = 4096;
    constexpr size_t kLongBuf  = 8192;

    static float early_storage[8][kEarlyBuf];
    static float diff_storage [6][kDiffBuf];
    static float long_storage [2][kLongBuf];
    memset(early_storage, 0, sizeof(early_storage));
    memset(diff_storage,  0, sizeof(diff_storage));
    memset(long_storage,  0, sizeof(long_storage));

    float* early_bufs[8];
    float* late_bufs[8];
    for (int i = 0; i < 8; ++i) early_bufs[i] = early_storage[i];
    for (int i = 0; i < 6; ++i) late_bufs[i]  = diff_storage[i];
    late_bufs[6] = long_storage[0];
    late_bufs[7] = long_storage[1];

    Early early;
    Late  late;
    OutputShelf out_shelfL, out_shelfR;

    early.Init(early_bufs, kEarlyBuf, kFS);
    late .Init(late_bufs,  kDiffBuf, kLongBuf, kFS);
    early.SnapParams(p, kFS);
    late .SnapParams(p, kFS);

    const float eo_hf = pitch_to_hz(p.eo_hf);
    const float eo_hb = db_to_lin(p.eo_hb);
    const float eo_lf = pitch_to_hz(p.eo_lf);
    const float eo_lb = db_to_lin(p.eo_lb);
    out_shelfL.SetParams(eo_hf, eo_hb, eo_lf, eo_lb, kFS);
    out_shelfR.SetParams(eo_hf, eo_hb, eo_lf, eo_lb, kFS);

    std::vector<float> outL(total), outR(total);
    for (size_t i = 0; i < total; ++i) {
        float eL, eR, ltL, ltR;
        early.Process(L[i], R[i], eL, eR);
        late .Process(eL,   eR,   ltL, ltR);
        float wetL = (1.f - p.el_mix) * eL + p.el_mix * ltL;
        float wetR = (1.f - p.el_mix) * eR + p.el_mix * ltR;
        wetL = out_shelfL.Process(wetL);
        wetR = out_shelfR.Process(wetR);
        outL[i] = (1.f - p.dw_mix) * L[i] + p.dw_mix * wetL;
        outR[i] = (1.f - p.dw_mix) * R[i] + p.dw_mix * wetR;
    }

    // ── stats + write ─────────────────────────────────────────────────────────
    print_stats(outL.data(), outR.data(), total);

    std::vector<float> interleaved(total * 2);
    for (size_t i = 0; i < total; ++i) {
        interleaved[i*2]   = outL[i];
        interleaved[i*2+1] = outR[i];
    }

    drwav_data_format fmt{};
    fmt.container     = drwav_container_riff;
    fmt.format        = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels      = 2;
    fmt.sampleRate    = (drwav_uint32)kFS;
    fmt.bitsPerSample = 32;

    drwav out_wav;
    if (!drwav_init_file_write(&out_wav, out_path, &fmt, nullptr)) {
        fprintf(stderr, "error: cannot write '%s'\n", out_path);
        return 1;
    }
    drwav_write_pcm_frames(&out_wav, total, interleaved.data());
    drwav_uninit(&out_wav);

    return 0;
}
