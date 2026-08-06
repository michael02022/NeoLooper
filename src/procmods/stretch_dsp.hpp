// DSP core of the Stretch module (non-loop processing, see src/loopmods/
// for that), with no ImGui dependency so it can be tested from core_test.
// Changes the DURATION of the audio (slower/longer or faster/shorter)
// while keeping its pitch fixed -- the mirror image of Pitch's traditional
// mode (which changes duration to achieve a pitch change); here duration
// is the goal and pitch is what's held constant.
//
// Pure phase-vocoder time-stretch (stretchcore::timeStretch, see
// time_stretch_core.hpp -- same core Pitch's "preserve duration" mode
// uses, just without the follow-up resample that turns a stretch into a
// pitch shift): the output IS the time-stretched result, nothing resamples
// it back. No phase-locking to spectral peaks (unlike PingPong Harmonic):
// a plain per-bin phase vocoder, the same sophistication level as
// Harmonic/Freeze/Pitch. Extreme ratios can show some phasiness/smearing
// on transients, an inherent limitation of this simple technique.
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace stretchdsp {

struct StretchSpec {
    float percent = 100.f; // target duration as % of the original (must be > 0; UI slider range lives in stretch_builtin.cpp)
};

// Output-length multiplier for the requested stretch (>1 = longer/slower).
inline double factor(const StretchSpec& spec) {
    return (double)spec.percent / 100.0;
}

// Time-stretches src[s0,s1), per channel: output length is approximately
// (s1-s0)*factor(spec) (exact rounding may differ by a sample or two
// between channels -- see stretchcore::timeStretch). nFft controls the
// STFT window (same kFftSizes convention as the other spectral modules):
// larger = better frequency resolution/less phasiness on sustained tones,
// smaller = less smearing on transients. With percent==100 this is an
// exact passthrough copy (no FFT spent). Empty if s1<=s0 or there are no
// channels.
std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      const StretchSpec& spec, int nFft);

} // namespace stretchdsp
