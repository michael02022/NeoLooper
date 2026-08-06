#include "freeze_stretch_dsp.hpp"

#include <algorithm>
#include <cmath>

#include "../procmods/signalsmith_stretch_dsp.hpp"
#include "crossfade_inside_core.hpp"

namespace freezestretchdsp {

Result generate(const AudioData& audio, const Params& params) {
    Result res;
    if (audio.frames() <= 0 || audio.channels() == 0) return res;
    const double sr = audio.sampleRate > 0.0 ? audio.sampleRate : 44100.0;

    int64_t anchor = (int64_t)std::llround(params.positionFrac * (double)audio.frames());
    anchor = std::max<int64_t>(0, std::min(anchor, audio.frames() - 1));

    int64_t grainLen = (int64_t)std::llround(kGrainSec * sr);
    grainLen = std::min(grainLen, audio.frames() - anchor);
    if (grainLen < 8) return res; // not enough audio left at this position for a grain

    int64_t durationSamples = (int64_t)std::llround(std::max(0.0, params.durationSec) * sr);
    if (durationSamples <= 0) return res;

    // percent can run into the thousands here (grain is short, duration
    // isn't) -- that's the point: pushing well past Signalsmith's own
    // "clean stretch" ratio is what turns the grain into an ambiance
    // texture instead of a literal time-stretch. No UI slider clamps this
    // call the way the regular Signalsmith Stretch tool's does.
    float percent = (float)(100.0 * (double)durationSamples / (double)grainLen);
    auto piece = signalsmithstretchdsp::apply(audio, anchor, anchor + grainLen, percent, false);
    if (piece.empty()) return res;

    // Final step: Classic Crossfade (Inside) over the freshly-generated
    // chunk itself (there's no material before it to blend against, the
    // attack was discarded on purpose) -- blends its tail into its own
    // head and reports the loop that results, same idiom as
    // ClassicCrossfadeInside/RotationLoop.
    const int nch = (int)piece.size();
    const int64_t n = (int64_t)piece[0].size();
    const int64_t avail = ccinside::fadeLen(n, params.ccPct);
    res.ambiance.resize(nch);
    for (int c = 0; c < nch; ++c) {
        std::vector<float> head(piece[c].begin(), piece[c].begin() + avail);
        std::vector<float> tail(piece[c].end() - avail, piece[c].end());
        std::vector<float> blendedTail = ccinside::blendTail(head, tail, params.ccShape);
        auto& out = res.ambiance[c];
        out.assign(piece[c].begin(), piece[c].end() - avail);
        out.insert(out.end(), blendedTail.begin(), blendedTail.end());
    }

    res.ok = true;
    res.loopStart = avail;
    res.loopEnd = n;
    return res;
}

} // namespace freezestretchdsp
