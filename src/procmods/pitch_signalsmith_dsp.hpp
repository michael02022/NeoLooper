// DSP core wrapping Signalsmith Stretch (signalsmith-audio.co.uk/code/
// stretch, signalsmith::stretch::SignalsmithStretch) for PITCH shifting --
// unlike signalsmith_stretch_dsp.hpp (which changes duration via ratio),
// this always keeps input and output at the SAME length (Stretch::exact()
// called with inputSamples==outputSamples) and instead configures the
// library's own frequency map (setTransposeSemitones/setFormantSemitones).
// No ImGui dependency (so it can be tested from core_test without dragging
// in the GUI).
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"
#include "pitch_dsp.hpp"

namespace pitchsignalsmithdsp {

// Pitch-shifts src[s0,s1) per channel by spec (semitones+cents, see
// pitch_dsp.hpp), duration unchanged. tonalityLimitHz: passed through to
// Stretch::setTransposeSemitones's own tonalityLimit parameter (there
// specified as a fraction of sample rate; here in Hz for a friendlier
// slider) -- 0 (the default) disables it, meaning the WHOLE spectrum gets
// a plain multiplicative (fully "harmonic") shift, same as a naive pitch
// shifter. Above 0, content past that frequency instead gets an additive
// shift, which keeps its relative spacing more natural -- useful for large
// shifts on broadband/noisy material, where a pure multiplicative shift
// can smear inharmonic/noisy high frequencies. preserveFormants: keeps the
// spectral envelope (vowel/timbre character) anchored to its ORIGINAL
// frequencies instead of moving together with the pitch (avoids the
// "chipmunk"/"Darth Vader" effect on voice-like material) -- implemented
// via setFormantSemitones(0, /*compensatePitch=*/true), which (per the
// vendored header's own updateFormants()) re-targets the formant energy
// lookup at the PRE-shift frequency for every transposed bin.
//
// Empty on failure (bad range, no channels, or the region is too short for
// even one analysis block -- see the library's own exact()).
std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      const pitchdsp::PitchSpec& spec, float tonalityLimitHz,
                                      bool preserveFormants);

} // namespace pitchsignalsmithdsp
