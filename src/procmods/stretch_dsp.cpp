#include "stretch_dsp.hpp"

#include <cmath>

#include "time_stretch_core.hpp"

namespace stretchdsp {

std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      const StretchSpec& spec, int nFft) {
    std::vector<std::vector<float>> out;
    const int64_t len = s1 - s0;
    const int nch = src.channels();
    if (len <= 0 || nch == 0) return out;

    const double f = factor(spec);
    if (f <= 0.0) return out;
    if (std::fabs(f - 1.0) < 1e-9) {
        // No stretch requested: exact passthrough, no FFT spent.
        out.resize((size_t)nch);
        for (int c = 0; c < nch; ++c)
            out[(size_t)c].assign(src.ch[(size_t)c].begin() + s0, src.ch[(size_t)c].begin() + s1);
        return out;
    }

    out.resize((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        const std::vector<float> region(src.ch[(size_t)c].begin() + s0, src.ch[(size_t)c].begin() + s1);
        std::vector<double> stretched;
        int64_t realStart = 0, realLen = 0;
        if (!stretchcore::timeStretch(region, len, nFft, f, stretched, realStart, realLen) ||
            realLen <= 0) {
            out.clear();
            return out;
        }
        // Unlike Pitch's stretch mode, there's no resample-back step here:
        // the stretched slice itself IS the result -- pitch stays fixed,
        // duration is exactly what we came here to change.
        std::vector<float> slice((size_t)realLen);
        for (int64_t i = 0; i < realLen; ++i)
            slice[(size_t)i] = (float)stretched[(size_t)(realStart + i)];
        out[(size_t)c] = std::move(slice);
    }
    return out;
}

} // namespace stretchdsp
