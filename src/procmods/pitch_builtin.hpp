#pragma once

#include "pitch_method.hpp"

// The original Pitch method (see pitch_dsp.hpp): traditional resample by
// the pitch ratio (default), or an opt-in "preserve duration" phase-
// vocoder stretch mode. Kept as the default/always-available entry in the
// Process > Pitch menu now that Signalsmith is also an option.
class PitchBuiltin : public PitchMethod {
public:
    const char* name() const override { return "Pitch (built-in)"; }

protected:
    bool drawParams(const Document& doc) override;
    double lengthFactor() const override {
        return preserveDuration_ ? 1.0 : 1.0 / pitchdsp::ratio(spec_);
    }
    std::vector<std::vector<float>> render(const AudioData& src, int64_t s0, int64_t s1) override;

private:
    bool preserveDuration_ = false; // off = traditional (default), on = stretch
    static constexpr int kFftSizes[7] = {128, 256, 512, 1024, 2048, 4096, 8192};
    int fftSizeIdx_ = 4; // 2048, only used when preserveDuration_
};
