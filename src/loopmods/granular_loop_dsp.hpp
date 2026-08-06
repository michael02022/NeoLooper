// DSP core of the Granular loop module, with no ImGui dependency (so it can
// be tested from core_test without dragging in the GUI). Port of
// proto/granaular sample looping.py (Peter Eastman's method): unlike
// Harmonic/Freeze (which synthesize a single pure period via phase vocoder)
// or Rotation (which reorders an ALREADY seamless loop into layers),
// Granular builds a LONGER loop body in the time domain, chaining several
// "grains" (fragments of random length taken from inside [ls, le)) joined
// by phase-aligned crossfades -- avoids the "electric"/static character of
// a single-exact-period loop, at the cost of no longer being exactly
// periodic grain by grain (only the WHOLE body is periodic at its own
// wrap).
//
// Unlike the original script (which auto-detects its working region via an
// amplitude threshold, meant to run headless over an isolated-note file),
// this port operates on [ls, le) already marked by the user -- same
// contract as ClassicCrossfadeInside, with no need for audio outside the
// region (unlike Harmonic/Freeze/Rotation).
//
// Structure of the built buffer (see buildGranularLoop):
//   head = src[ls, splitPoint)                    -- plays ONCE
//   body = grain0(seed, then blended) ++ grain1 ++ ... ++ grainN(tail)
//        -- REPEATS (previewLoopOffset() = headLen in the GUI module)
// where splitPoint = ls + round(width*headPct/100) (by default half the
// region, same as 'first_seg_end' in the script).
//
// The original script's "free" closure trick is preserved EXACTLY: on the
// last iteration the crossfade's start point is forced to 'ls' (not
// random) and the final grain's length is recomputed to end exactly at
// splitPoint. Since the blend (raised cosine, see the .cpp) has t=0 EXACT
// at each grain's first sample (gOut=1, gIn=0 with no floating-point
// error), this guarantees ALGEBRAICALLY -- not just approximately -- that
// body.front() == src[splitPoint] and body.back() == src[splitPoint-1]: the
// body's wrap (its own end -> its own start) reproduces two ADJACENT
// samples of the original, unedited audio. See the continuity test in
// core_test.cpp.
//
// The phase search (minimizing the sample-by-sample L1 error against the
// previous grain's tail, over a window of 'period' samples) is analogous to
// a single-degree-of-freedom WSOLA; the period is estimated via
// autocorrelation (adapted from freq_from_autocorr in the script, see
// estimatePeriod) over channel 0 of [ls, le).
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace granulardsp {

struct Result {
    int64_t headLen = 0;      // = splitPoint - ls; head plays once
    int64_t nLoopSamples = 0; // total body length (repeats)
    int64_t period = 0;       // estimated period (samples), diagnostic
    bool grainsClamped = false; // true if any grain got trimmed for lack of room in [ls,le)
    std::vector<std::vector<float>> head; // per channel, headLen samples
    std::vector<std::vector<float>> body; // per channel, nLoopSamples samples
};

// Cheap arithmetic (no autocorrelation or RNG), used for immediate feedback
// in the GUI while dragging a slider. estLoopMin/estLoopMax are an ESTIMATE
// of nLoopSamples's range (the real value depends on each grain's random
// length draw, it isn't deterministic).
void sizes(int64_t ls, int64_t le, float headPct, float grainMinPct, float grainMaxPct,
          int nGrains, int64_t& headLen, int64_t& splitPoint, int64_t& estLoopMin,
          int64_t& estLoopMax);

// Estimates the period (in samples) of channel 0 of src[start, start+len)
// via autocorrelation + parabolic interpolation of the peak (adapted from
// freq_from_autocorr/parabolic in the reference script). Caps the analysis
// window and the maximum lag searched to keep cost low (see .cpp). Returns
// 0 if no clear periodicity is found (e.g. silence or noise) -- the caller
// must apply a fallback.
int64_t estimatePeriod(const AudioData& src, int64_t start, int64_t len);

// nGrains grains chained by a phase-aligned crossfade within [ls, le);
// grainMinPct/grainMaxPct bound each grain's length as % of (le-ls). The
// blend is ALWAYS the reference script's raised cosine (no curve parameter:
// no value of xfadeGains's linear<->equal-power morph reproduces that
// shape). seed<0 -> random (random_device) on every call; >=0 ->
// reproducible -- same pattern as Rotation. Fails gracefully (returns
// false) if le<=ls, nGrains<1, or the region is too short to even fit
// head+seed.
bool buildGranularLoop(const AudioData& src, int64_t ls, int64_t le, int nGrains,
                       float grainMinPct, float grainMaxPct, float headPct,
                       int64_t seed, Result& out);

} // namespace granulardsp
