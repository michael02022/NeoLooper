// DSP core of the Amplitude module (non-loop processing, see src/loopmods/
// for that), with no ImGui dependency so it can be tested from core_test.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace amplitudedsp {

struct AmplitudeSpec {
    float gainDb = 0.f; // -60..+24 dB, same headroom convention as Filter's bell band
    bool clip = true;   // hard-clamp to [-1,1] after the gain; ON by default
};

inline double gainLinear(const AmplitudeSpec& spec) {
    return std::pow(10.0, (double)spec.gainDb / 20.0);
}

// Peak absolute sample value within src[s0,s1) across all channels -- used
// to show "region peak" / "peak after gain" readouts. 0 if s1<=s0 or there
// are no channels.
float peakAbs(const AudioData& src, int64_t s0, int64_t s1);

// Multiplies src[s0,s1) per channel by gainLinear(spec). If spec.clip,
// hard-clamps the result to [-1,1] (digital clipping, the classic
// distortion from pushing a signal too hot). If not, samples are left
// as-is, possibly beyond +-1.0 -- the same "hot" float headroom other
// tools in this codebase already tolerate mid-edit (e.g. Stretch); a later
// bit-depth conversion or export handles that by SCALING the whole file
// down instead of clipping (see Document::toggleBits / wav_io.cpp's
// exportWavSmpl).
//
// Same length as the input region, per channel. Empty if s1<=s0 or there
// are no channels.
std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      const AmplitudeSpec& spec);

} // namespace amplitudedsp
