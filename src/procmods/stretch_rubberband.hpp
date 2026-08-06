#pragma once

#include "stretch_method.hpp"

// Rubber Band Library (breakfastquay.com/rubberband) time-stretch, via the
// system librubberband (GPLv2+ -- same licensing situation this project
// already has with Loris, see third_party/loris-2.0). Requires
// librubberband-dev at build time (see CMakeLists.txt).
class StretchRubberBand : public StretchMethod {
public:
    const char* name() const override { return "Rubber Band"; }

protected:
    bool drawParams() override;
    double lengthFactor() const override { return (double)percent_ / 100.0; }
    std::vector<std::vector<float>> render(const AudioData& src, int64_t s0, int64_t s1) override;

private:
    float percent_ = 100.f;
    bool finerEngine_ = true; // R3 "Finer": higher quality, the recommended choice offline
};
