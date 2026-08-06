#pragma once

#include "pitch_method.hpp"

// Signalsmith Stretch (signalsmith-audio.co.uk/code/stretch), vendored
// header-only under third_party/signalsmith-stretch (MIT license, same
// dependency the Stretch submenu's own Signalsmith method already uses) --
// always duration-preserving (see pitch_signalsmith_dsp.hpp), with two
// extra controls the built-in phase vocoder doesn't offer: a tonality
// limit (better behavior on noisy/broadband material for large shifts) and
// formant preservation (avoids the "chipmunk" effect).
class PitchSignalsmith : public PitchMethod {
public:
    const char* name() const override { return "Pitch (Signalsmith)"; }

protected:
    bool drawParams(const Document& doc) override;
    double lengthFactor() const override { return 1.0; } // always duration-preserving
    std::vector<std::vector<float>> render(const AudioData& src, int64_t s0, int64_t s1) override;

private:
    float tonalityLimitHz_ = 0.f; // 0 = disabled (full multiplicative shift, the library default)
    bool preserveFormants_ = false;
};
