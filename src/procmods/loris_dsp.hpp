// DSP core of the Loris module (non-loop processing, see src/loopmods/ for
// that), with no ImGui dependency so it can be tested from core_test.
// Transposes via additive resynthesis (third_party/loris-2.0, Kelly Fitz &
// Lippold Haken's Reassigned Bandwidth-Enhanced Additive Sound Model)
// instead of a phase vocoder: the audio is decomposed into individual
// sinusoidal "partials" (independent time-varying frequency/amplitude/
// noise trajectories), each partial's frequency is scaled directly, and
// the result is resynthesized from oscillators. For monophonic, quasi-
// harmonic material (a single melodic note) this preserves timbre and
// transients much better than Pitch/Stretch's per-bin phase vocoder, at
// the cost of assuming exactly that kind of source -- chords, percussion
// or noisy material will sound worse than with Pitch, not better.
//
// Pipeline (see loris_dsp.cpp): Analyzer::analyze -> Channelizer::channelize
// (using a FLAT reference envelope at refFreqHz, i.e. assumes a fixed
// fundamental for the whole region, no vibrato tracking) -> Distiller::
// distill (collapses to one partial per harmonic, removing overlap
// clutter) -> PartialUtils::shiftPitch (cents, same unit as pitchdsp::
// PitchSpec's ratio()) -> Synthesizer::synthesize.
//
// Unlike Pitch/Stretch, this is ALWAYS a full analysis+resynthesis, even
// at 0 semitones/0 cents -- there's no bit-identical passthrough shortcut,
// because resynthesizing from tracked partials is inherently lossy (that's
// the nature of the technique, not a bug).
//
// refFreqHz is the fundamental frequency (Hz) Loris is told to expect for
// the region being processed -- callers resolve it before calling apply()
// (LorisTool defaults it to the document's Root Note + Tune, see
// editor_ui/procmods/loris.cpp, with a manual override checkbox); this
// function doesn't know or care where it came from. Analysis resolution is
// derived from it (half the fundamental, the standard Loris recipe for
// separating adjacent harmonics into their own channel).
//
// Like Pitch/Stretch, each channel is analyzed and resynthesized
// independently (no cross-channel phase coherence guarantee for wide
// stereo content).
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace lorisdsp {

struct LorisSpec {
    float semitones = 0.f;   // -24..24 (+-2 octaves)
    float cents = 0.f;       // -99..99, fine adjustment (same range as pitchdsp::PitchSpec)
    double refFreqHz = 220.0; // expected fundamental of the region, > 0
};

// Total pitch shift in cents (Loris's own unit for PartialUtils::shiftPitch).
inline double totalCents(const LorisSpec& spec) {
    return (double)spec.semitones * 100.0 + (double)spec.cents;
}

// Resynthesizes src[s0,s1), per channel: output length is forced to EXACTLY
// (s1-s0) samples (zero-padded or truncated -- Loris's own analysis/
// synthesis boundary handling can drift the raw result by a few ms), same
// fixed-length convention as pitchdsp::apply's preserveDuration mode, so
// callers can replace the region in place without remapping loop markers.
// Empty if s1<=s0, there are no channels, or refFreqHz<=0; also empty (per
// channel) if the Loris analysis throws (e.g. a region too short to
// contain a single analysis window).
std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      const LorisSpec& spec);

} // namespace lorisdsp
