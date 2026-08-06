// DSP core wrapping Signalsmith Stretch (signalsmith-audio.co.uk/code/
// stretch, signalsmith::stretch::SignalsmithStretch), with no ImGui
// dependency (so it can be tested from core_test without dragging in the
// GUI).
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace signalsmithstretchdsp {

// Time-stretches src[s0,s1) per channel using Signalsmith Stretch's
// exact() (a complete-buffer offline call: seek + process + flush in one
// go). percent: target duration as % of the original (100 = unchanged),
// same convention as stretchdsp::StretchSpec::percent. cheaper=true uses
// the library's lower-latency/lower-quality preset instead of its
// default. Empty on failure (bad range, no channels, percent<=0, or the
// region is too short for even one analysis block -- see the library's
// own exact(), which fails gracefully rather than crashing in that case).
std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      float percent, bool cheaper);

} // namespace signalsmithstretchdsp
