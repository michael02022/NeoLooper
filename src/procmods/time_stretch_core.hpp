// Shared phase-vocoder time-stretch core, used by both Pitch's "preserve
// duration" mode (pitch_dsp.cpp: stretch + resample back = pitch shift with
// fixed duration) and the Stretch module (stretch_dsp.cpp: stretch alone,
// no resample = duration change with fixed pitch) -- same relationship as
// spectral_loop_core.hpp between Harmonic and Freeze. No ImGui dependency,
// so it can be tested from core_test.
#pragma once

#include <cstdint>
#include <vector>

namespace stretchcore {

// Phase-vocoder time-stretch of a single channel: reads 'in' (len samples)
// zero-padded internally (so it works regardless of where [0,len) sits in
// the document, with no need for extra context outside it) and produces a
// version that is 'stretch' times longer, at the SAME pitch as the input.
//
// Both analysis AND synthesis use the SAME fixed hop (nFft/4, 75% overlap)
// -- unlike a naive hop-warping trick (scaling ONLY the synthesis hop by
// 'stretch'), which only works up to modest stretch factors: once the
// synthesis hop grows past roughly nFft/2, overlap between consecutive
// synthesized frames gets too thin for the phase accumulation to keep the
// reconstruction locked to the right frequency, and the result comes out
// at a wrong, sometimes wildly wrong, pitch (verified empirically while
// building this: a +1 octave shift via that approach, synthesis hop =
// nFft/2, reconstructed a tone at 1/5th the expected frequency instead of
// double). Keeping the OUTPUT hop fixed means overlap -- and therefore
// tracking quality -- never degrades regardless of 'stretch'; what changes
// instead is which (generally fractional, linearly interpolated) analysis
// frame feeds each synthesis step: reading through the analysis frames
// slower than 1:1 (stretch>1) or faster (stretch<1) is what actually
// produces the stretch.
//
// realStart/realLen (out) mark where the unpadded region ends up living in
// 'out', so the caller can slice out exactly the stretched version of
// [0,len) without the padding. false if the region (after padding) doesn't
// reach at least 2 analysis frames for this nFft.
bool timeStretch(const std::vector<float>& in, int64_t len, int nFft, double stretch,
                 std::vector<double>& out, int64_t& realStart, int64_t& realLen);

} // namespace stretchcore
