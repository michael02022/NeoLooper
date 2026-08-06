#include "pitch_signalsmith_dsp.hpp"

#include "signalsmith-stretch.h"

namespace pitchsignalsmithdsp {
namespace {

using Stretch = signalsmith::stretch::SignalsmithStretch<float>;

// Lightweight offset view over AudioData::ch so Stretch::exact() can index
// input as inputs[c][i] starting at s0, without copying the region first --
// same helper as signalsmith_stretch_dsp.cpp's InputView.
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
                                      const pitchdsp::PitchSpec& spec, float tonalityLimitHz,
                                      bool preserveFormants) {
    std::vector<std::vector<float>> out;
    const int nch = src.channels();
    if (nch <= 0 || s1 <= s0) return out;
    const int64_t n = s1 - s0;

    Stretch stretch;
    stretch.presetDefault(nch, (float)src.sampleRate);

    const float semitones = spec.semitones + spec.cents / 100.f;
    // setTransposeSemitones's own tonalityLimit is a fraction of sample
    // rate (0..0.5 = Nyquist); tonalityLimitHz<=0 leaves it at the
    // library's default (no limit -- full multiplicative shift).
    const float tonalityLimit =
        (tonalityLimitHz > 0.f && src.sampleRate > 0.0)
            ? (float)((double)tonalityLimitHz / src.sampleRate)
            : 0.f;
    stretch.setTransposeSemitones(semitones, tonalityLimit);
    if (preserveFormants) stretch.setFormantSemitones(0.f, /*compensatePitch=*/true);

    out.assign((size_t)nch, std::vector<float>((size_t)n, 0.f));
    InputView input{src, s0};
    bool ok = stretch.exact(input, (int)n, out, (int)n);
    if (!ok) return {};
    return out;
}

} // namespace pitchsignalsmithdsp
