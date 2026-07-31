// Offline renderer: pushes an impulse through the Schroeder reverb and writes
// a WAV file. No audio device involved — this is the fast iteration path for
// tuning delay times, feedback, and damping. Rebuild + run in well under a
// second; every reverb parameter is a CLI flag so most tweaks don't even
// need a recompile.
#include "schroeder.h"
#include "wav.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

static constexpr unsigned int kSampleRate = 48000;

int main(int argc, char** argv) {
    float fb     = 0.84f;
    float damp   = 0.2f;
    float apGain = 0.5f;
    int   d0 = 1687, d1 = 1601, d2 = 2053, d3 = 2251;
    int   da0 = 347, da1 = 113;
    float amp     = 0.5f;
    float seconds = 3.0f;
    std::string out = "/tmp/reverb_ir.wav";

    auto nextFloat = [&](int& i) { return std::atof(argv[++i]); };
    auto nextInt   = [&](int& i) { return std::atoi(argv[++i]); };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fb")        fb      = nextFloat(i);
        else if (a == "--damp") damp    = nextFloat(i);
        else if (a == "--ap")   apGain  = nextFloat(i);
        else if (a == "--d0")   d0      = nextInt(i);
        else if (a == "--d1")   d1      = nextInt(i);
        else if (a == "--d2")   d2      = nextInt(i);
        else if (a == "--d3")   d3      = nextInt(i);
        else if (a == "--da0")  da0     = nextInt(i);
        else if (a == "--da1")  da1     = nextInt(i);
        else if (a == "--amp")  amp     = nextFloat(i);
        else if (a == "--sec")  seconds = nextFloat(i);
        else if (a == "--out")  out     = argv[++i];
        else if (a == "-h" || a == "--help") {
            std::cout <<
                "usage: render [--fb F] [--damp F] [--ap F] [--d0..d3 N] [--da0/--da1 N]\n"
                "               [--amp F] [--sec F] [--out path.wav]\n"
                "defaults: fb=" << fb << " damp=" << damp << " ap=" << apGain
                << " amp=" << amp << " sec=" << seconds << " out=" << out << "\n";
            return 0;
        } else {
            std::cerr << "unknown arg: " << a << " (--help for usage)\n";
            return 1;
        }
    }

    Schroeder reverb(fb, damp, apGain, d0, d1, d2, d3, da0, da1);

    unsigned int nSamples = static_cast<unsigned int>(seconds * kSampleRate);
    std::vector<float> samples(nSamples);
    for (unsigned int i = 0; i < nSamples; ++i) {
        float dry = (i == 0) ? amp : 0.0f;
        samples[i] = reverb.process(dry);
    }

    writeWav(out.c_str(), samples, kSampleRate);
    std::cout << "wrote " << out << " (" << seconds << "s, fb=" << fb
              << " damp=" << damp << " ap=" << apGain << ")\n";
    return 0;
}
