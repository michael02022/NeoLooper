// DSP core wrapping Rubber Band Library (breakfastquay.com/rubberband,
// RubberBand::RubberBandStretcher), with no ImGui dependency (so it can be
// tested from core_test without dragging in the GUI).
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace rubberbandstretchdsp {

enum class Engine { Faster, Finer };

// Time-stretches src[s0,s1) per channel using Rubber Band's offline mode
// (study() then process()/retrieve(), the whole region in a single call
// each -- Rubber Band explicitly supports passing the input "as a single
// large block"). percent: target duration as % of the original (100 =
// unchanged), same convention as stretchdsp::StretchSpec::percent.
// Finer = the R3 engine (higher quality, the recommended choice for
// offline/non-realtime use); Faster = R2 (lower CPU, tuned for real-time).
// Empty on failure (bad range, no channels, or percent<=0).
std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      float percent, Engine engine);

} // namespace rubberbandstretchdsp
