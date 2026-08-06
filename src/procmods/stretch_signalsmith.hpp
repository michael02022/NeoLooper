#pragma once

#include "stretch_method.hpp"

// Signalsmith Stretch (signalsmith-audio.co.uk/code/stretch), vendored
// header-only under third_party/signalsmith-stretch (MIT license, see
// VENDORED_FROM.txt there) -- no system dependency, unlike Rubber Band.
class StretchSignalsmith : public StretchMethod {
public:
    const char* name() const override { return "Signalsmith Stretch"; }

protected:
    bool drawParams() override;
    double lengthFactor() const override { return (double)percent_ / 100.0; }
    std::vector<std::vector<float>> render(const AudioData& src, int64_t s0, int64_t s1) override;

private:
    float percent_ = 100.f;
    bool cheaper_ = false;
};
