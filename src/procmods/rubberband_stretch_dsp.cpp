#include "rubberband_stretch_dsp.hpp"

#include <algorithm>
#include <cmath>

#include <rubberband/RubberBandStretcher.h>

namespace rubberbandstretchdsp {

std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      float percent, Engine engine) {
    std::vector<std::vector<float>> out;
    const int nch = src.channels();
    if (nch <= 0 || s1 <= s0) return out;
    const int64_t nIn = s1 - s0;
    const double ratio = (double)percent / 100.0;
    if (ratio <= 0.0) return out;
    const int64_t nOut = (int64_t)std::llround((double)nIn * ratio);
    if (nOut <= 0) return out;

    using RubberBand::RubberBandStretcher;
    RubberBandStretcher::Options options =
        RubberBandStretcher::OptionProcessOffline |
        (engine == Engine::Finer ? RubberBandStretcher::OptionEngineFiner
                                 : RubberBandStretcher::OptionEngineFaster);
    RubberBandStretcher stretcher((size_t)std::lround(src.sampleRate), (size_t)nch, options,
                                  ratio, 1.0 /* pitch unchanged */);

    // Rubber Band's offline mode accepts the whole input as a single
    // block in each pass (study, then process) -- no need to chunk it.
    std::vector<const float*> inPtrs((size_t)nch);
    for (int c = 0; c < nch; ++c) inPtrs[(size_t)c] = src.ch[(size_t)c].data() + s0;
    stretcher.study(inPtrs.data(), (size_t)nIn, true);
    stretcher.process(inPtrs.data(), (size_t)nIn, true);

    out.assign((size_t)nch, std::vector<float>());
    for (auto& v : out) v.reserve((size_t)nOut);

    constexpr size_t kChunk = 8192;
    std::vector<std::vector<float>> chunkBuf((size_t)nch, std::vector<float>(kChunk));
    std::vector<float*> outPtrs((size_t)nch);
    // Offline process() above already did all the work synchronously, so
    // available() shouldn't linger at 0 here; stallGuard is just a safety
    // net against spinning forever if that assumption is ever wrong.
    int stallGuard = 0;
    for (;;) {
        int avail = stretcher.available();
        if (avail < 0) break;
        if (avail == 0) {
            if (++stallGuard > 1'000'000) break;
            continue;
        }
        stallGuard = 0;
        size_t toRead = std::min((size_t)avail, kChunk);
        for (int c = 0; c < nch; ++c) outPtrs[(size_t)c] = chunkBuf[(size_t)c].data();
        size_t got = stretcher.retrieve(outPtrs.data(), toRead);
        for (int c = 0; c < nch; ++c)
            out[(size_t)c].insert(out[(size_t)c].end(), chunkBuf[(size_t)c].begin(),
                                  chunkBuf[(size_t)c].begin() + (long)got);
    }
    return out;
}

} // namespace rubberbandstretchdsp
