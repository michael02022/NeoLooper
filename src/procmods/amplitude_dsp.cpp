#include "amplitude_dsp.hpp"

#include <algorithm>

namespace amplitudedsp {

float peakAbs(const AudioData& src, int64_t s0, int64_t s1) {
    if (s1 <= s0) return 0.f;
    float p = 0.f;
    for (auto& v : src.ch)
        for (int64_t i = s0; i < s1; ++i) p = std::max(p, std::fabs(v[(size_t)i]));
    return p;
}

std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      const AmplitudeSpec& spec) {
    std::vector<std::vector<float>> out;
    const int nch = src.channels();
    if (nch <= 0 || s1 <= s0) return out;

    const float g = (float)gainLinear(spec);
    out.resize((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        const float* s = src.ch[(size_t)c].data() + s0;
        auto& v = out[(size_t)c];
        v.resize((size_t)(s1 - s0));
        for (int64_t i = 0; i < s1 - s0; ++i) {
            float x = s[i] * g;
            if (spec.clip) x = std::clamp(x, -1.f, 1.f);
            v[(size_t)i] = x;
        }
    }
    return out;
}

} // namespace amplitudedsp
