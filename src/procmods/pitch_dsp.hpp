// DSP core of the Pitch module (non-loop processing, see src/loopmods/ for
// that), with no ImGui dependency so it can be tested from core_test. Two
// modes, both transposing by semitones+cents without touching the
// document's declared sample rate:
//
// - Traditional (preserveDuration=false, the default): plain resample of
//   the region by the pitch ratio -- the exact same "resample by pitch
//   ratio" trick Freeze already uses for its own tuneCents (see
//   freeze_loop_dsp.cpp), just applied directly to real audio instead of a
//   synthesized loop period. Duration changes together with pitch (higher
//   pitch = shorter, like speeding up tape/vinyl) -- classic, simple pitch
//   shifting, no FFT involved.
//
// - Stretch (preserveDuration=true): stretchcore::timeStretch() (see
//   time_stretch_core.hpp -- the same phase-vocoder core the Stretch
//   module uses on its own, without the resample step, to change duration
//   while keeping pitch fixed) followed by the same resample step as the
//   traditional mode, which compresses/expands the stretched buffer back
//   to the original length. Result: only the tonal content changes,
//   duration and sample rate stay untouched.
//
// Neither mode does phase-locking to spectral peaks (unlike PingPong
// Harmonic): a plain per-bin phase vocoder, the same sophistication level
// as Harmonic/Freeze. In stretch mode, large shifts (several semitones) can
// show some "phasiness"/smearing on transients -- an inherent limitation of
// this simple technique, not a bug.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace pitchdsp {

struct PitchSpec {
    float semitones = 0.f; // -24..24 (+-2 octaves)
    float cents = 0.f;     // -99..99, fine adjustment (same range as the document's Tune)
};

// Playback-rate multiplier for the requested shift (>1 = higher pitch).
inline double ratio(const PitchSpec& spec) {
    return std::pow(2.0, ((double)spec.semitones + (double)spec.cents / 100.0) / 12.0);
}

// Pitch-shifts src[s0,s1), per channel (all channels always come out the
// same length as each other, since resampling is done for every channel in
// a single resampleHQ() call). With semitones==0 && cents==0 this is an
// exact passthrough copy of length (s1-s0), in either mode.
//
// preserveDuration=false (default/traditional): output length is
// approximately round((s1-s0)/ratio(spec)) -- shorter for a higher pitch,
// longer for a lower one. The caller is expected to splice a possibly
// different-length piece back into the document (see Document::
// applyPitch), not overwrite in place.
//
// preserveDuration=true (stretch): output length is exactly (s1-s0), same
// as the input range -- nFft controls the STFT window (same kFftSizes
// convention as the other spectral modules): larger = better frequency
// resolution/less phasiness on sustained tones, smaller = less smearing on
// transients. Unused when preserveDuration=false.
//
// Empty if s1<=s0 or there are no channels.
std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      const PitchSpec& spec, int nFft, bool preserveDuration);

} // namespace pitchdsp
