#include "crossfade_inside_core.hpp"

#include <cmath>

#include "loop_module.hpp" // xfadeGains

namespace ccinside {

int64_t fadeLen(int64_t loopLen, float pct) {
    const int64_t maxAvail = loopLen / 2; // head and tail can't overlap
    return (int64_t)std::llround((double)maxAvail * (double)pct / 100.0);
}

std::vector<float> blendTail(const std::vector<float>& head, const std::vector<float>& tail,
                             float shape) {
    const int64_t avail = (int64_t)tail.size();
    std::vector<float> out((size_t)avail);
    for (int64_t j = 0; j < avail; ++j) {
        float t = (float)(j + 1) / (float)avail;
        float gIn, gOut;
        xfadeGains(t, shape, gIn, gOut);
        out[(size_t)j] = tail[(size_t)j] * gOut + head[(size_t)j] * gIn;
    }
    return out;
}

} // namespace ccinside
