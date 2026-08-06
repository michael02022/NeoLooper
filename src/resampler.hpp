#pragma once

#include <vector>

// High-quality offline resampler (Kaiser-windowed sinc, oversampled table
// with linear interpolation). Alias rejection > 120 dB, comparable to
// Audacity's "High Quality" mode (libsoxr HQ).
std::vector<std::vector<float>> resampleHQ(
    const std::vector<std::vector<float>>& in, double srIn, double srOut);
