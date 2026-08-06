#include "loris_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <exception>

#include "Analyzer.h"
#include "Channelizer.h"
#include "Distiller.h"
#include "LinearEnvelope.h"
#include "PartialList.h"
#include "PartialUtils.h"
#include "Synthesizer.h"

namespace lorisdsp {

std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      const LorisSpec& spec) {
    std::vector<std::vector<float>> out;
    const int64_t len = s1 - s0;
    const int nch = src.channels();
    if (len <= 0 || nch == 0 || spec.refFreqHz <= 0.0) return out;

    const double srate = src.sampleRate;
    const double cents = totalCents(spec);
    // Standard Loris recipe: resolution = half the fundamental, so adjacent
    // harmonics of a harmonic series fall into non-overlapping channels;
    // the single-arg Analyzer constructor derives window width (= 2x
    // resolution) and every other analysis parameter from this.
    const double resolutionHz = spec.refFreqHz * 0.5;

    out.resize((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        std::vector<double> region((size_t)len);
        for (int64_t i = 0; i < len; ++i) region[(size_t)i] = (double)src.ch[(size_t)c][(size_t)(s0 + i)];

        std::vector<float> fixed((size_t)len, 0.f);
        try {
            Loris::Analyzer analyzer(resolutionHz);
            Loris::PartialList partials = analyzer.analyze(region, srate);

            Loris::LinearEnvelope refEnv(spec.refFreqHz);
            Loris::Channelizer::channelize(partials, refEnv, 1);
            Loris::Distiller distiller;
            distiller.distill(partials);

            if (std::fabs(cents) > 1e-9) {
                Loris::LinearEnvelope centsEnv(cents);
                Loris::PartialUtils::shiftPitch(partials.begin(), partials.end(), centsEnv);
            }

            std::vector<double> buf;
            Loris::Synthesizer synth(srate, buf);
            synth.synthesize(partials.begin(), partials.end());

            // Force EXACTLY 'len' samples: Loris's own fade-in/out and crop
            // handling can drift the raw synthesized length by a few ms
            // relative to the analyzed region.
            const int64_t n = std::min(len, (int64_t)buf.size());
            for (int64_t i = 0; i < n; ++i) fixed[(size_t)i] = (float)buf[(size_t)i];
        } catch (const std::exception&) {
            out.clear();
            return out;
        }
        out[(size_t)c] = std::move(fixed);
    }
    return out;
}

} // namespace lorisdsp
