#pragma once

struct Params {
    float predelay_ms = 40.f,  pd_sym   = 0.3f;
    float er_sz       = 30.f,  er_sym   = 1.f,   er_dffs  = 0.68f;
    float lt_sz       = 24.f,  lt_sym   = 1.2f,  lt_theta = 0.7853982f;
    float rt60_s      =  4.f,  bloom    = 3.14f;
    float mod_ms      =  0.6f, mod_hz   = 0.7f;
    float dmp_hf      = 96.f,  dmp_hb   = -7.f,  dmp_lf   = 60.f,  dmp_lb   = 0.f;
    float eo_hf       = 100.f, eo_hb    = -3.f,  eo_lf    = 65.f,  eo_lb    = 3.f;
    float el_mix      = 0.85f, dw_mix   = 1.f;
};
