#pragma once

#include "stretch_method.hpp"
#include "stretch_dsp.hpp"

// The original Stretch method (see stretch_dsp.hpp): plain per-bin
// phase-vocoder time-stretch, no FFT-size auto-tuning, no third-party
// dependency. Kept as the default/always-available entry in the Process >
// Stretch menu now that Rubber Band and Signalsmith are also options.
class StretchBuiltin : public StretchMethod {
public:
    const char* name() const override { return "Stretch (built-in)"; }

protected:
    bool drawParams() override;
    double lengthFactor() const override { return stretchdsp::factor(spec_); }
    std::vector<std::vector<float>> render(const AudioData& src, int64_t s0, int64_t s1) override;

private:
    stretchdsp::StretchSpec spec_;
    static constexpr int kFftSizes[7] = {128, 256, 512, 1024, 2048, 4096, 8192};
    int fftSizeIdx_ = 4; // 2048
};
