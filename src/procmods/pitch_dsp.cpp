#include "pitch_dsp.hpp"

#include <algorithm>
#include <cmath>

#include "../resampler.hpp"
#include "time_stretch_core.hpp"

namespace pitchdsp {

std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      const PitchSpec& spec, int nFft, bool preserveDuration) {
    std::vector<std::vector<float>> out;
    const int64_t len = s1 - s0;
    const int nch = src.channels();
    if (len <= 0 || nch == 0) return out;

    const double r = ratio(spec);
    if (std::fabs(r - 1.0) < 1e-9) {
        // No shift requested: exact passthrough, no FFT/resample spent.
        out.resize((size_t)nch);
        for (int c = 0; c < nch; ++c)
            out[(size_t)c].assign(src.ch[(size_t)c].begin() + s0, src.ch[(size_t)c].begin() + s1);
        return out;
    }

    if (!preserveDuration) {
        // Traditional pitch shift: resample the whole region in one call
        // (guarantees every channel comes out the same length) by the
        // pitch ratio, same trick Freeze already uses for its own tuning
        // (freeze_loop_dsp.cpp) -- duration changes together with pitch.
        std::vector<std::vector<float>> region((size_t)nch);
        for (int c = 0; c < nch; ++c)
            region[(size_t)c].assign(src.ch[(size_t)c].begin() + s0, src.ch[(size_t)c].begin() + s1);
        return resampleHQ(region, r, 1.0);
    }

    out.resize((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        const std::vector<float> region(src.ch[(size_t)c].begin() + s0, src.ch[(size_t)c].begin() + s1);
        std::vector<double> stretched;
        int64_t realStart = 0, realLen = 0;
        if (!stretchcore::timeStretch(region, len, nFft, r, stretched, realStart, realLen) ||
            realLen <= 0) {
            out.clear();
            return out;
        }
        std::vector<float> slice((size_t)realLen);
        for (int64_t i = 0; i < realLen; ++i)
            slice[(size_t)i] = (float)stretched[(size_t)(realStart + i)];

        // resampleHQ(in, srIn, srOut) at ratio 1/r: the same trick Freeze
        // uses for its own manual tuning (freeze_loop_dsp.cpp), but applied
        // here to the ALREADY-STRETCHED buffer -- compressing it back down
        // to the original duration multiplies the pitch by 'r' without
        // touching duration again (the stretch step already compensated
        // for it).
        std::vector<std::vector<float>> asChans(1, std::move(slice));
        auto resampled = resampleHQ(asChans, r, 1.0);
        const auto& res = resampled[0];

        // Forces EXACTLY 'len' samples: stretch/resample rounding can drift
        // the result by a couple of samples.
        std::vector<float> fixed((size_t)len);
        for (int64_t i = 0; i < len; ++i)
            fixed[(size_t)i] = (i < (int64_t)res.size()) ? res[(size_t)i]
                                                          : (res.empty() ? 0.f : res.back());
        out[(size_t)c] = std::move(fixed);
    }
    return out;
}

} // namespace pitchdsp
