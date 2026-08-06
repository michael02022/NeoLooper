#include "signalsmith_stretch_dsp.hpp"

#include <cmath>

#include "signalsmith-stretch.h"

namespace signalsmithstretchdsp {
namespace {

using Stretch = signalsmith::stretch::SignalsmithStretch<float>;

// Lightweight offset view over AudioData::ch so Stretch::exact() can index
// input as inputs[c][i] starting at s0, without copying the region first.
struct InputView {
    const AudioData& src;
    int64_t s0;
    struct Channel {
        const float* base;
        float operator[](int i) const { return base[i]; }
    };
    Channel operator[](int c) const { return Channel{src.ch[(size_t)c].data() + s0}; }
};

} // namespace

std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      float percent, bool cheaper) {
    std::vector<std::vector<float>> out;
    const int nch = src.channels();
    if (nch <= 0 || s1 <= s0) return out;
    const int64_t nIn = s1 - s0;
    const double ratio = (double)percent / 100.0;
    if (ratio <= 0.0) return out;
    const int64_t nOut = (int64_t)std::llround((double)nIn * ratio);
    if (nOut <= 0) return out;

    Stretch stretch;
    if (cheaper)
        stretch.presetCheaper(nch, (float)src.sampleRate);
    else
        stretch.presetDefault(nch, (float)src.sampleRate);

    out.assign((size_t)nch, std::vector<float>((size_t)nOut, 0.f));
    InputView input{src, s0};
    bool ok = stretch.exact(input, (int)nIn, out, (int)nOut);
    if (!ok) return {};
    return out;
}

} // namespace signalsmithstretchdsp
