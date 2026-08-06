// DSP core of "Auto Loop", with no ImGui dependency (so it can be tested
// from core_test without dragging in the GUI). Adapted from milselarch/
// autolooper (github.com/milselarch/autolooper, autoloop.c): given a range
// of audio, search for the pair of same-length windows that sound most
// alike so that region can repeat seamlessly.
//
// The original tool is built for whole songs: it tries window sizes from
// 10 to 25 seconds with a fixed 1/6-second step (both hardcoded), then
// refines the end point with an exhaustive sample-by-sample search over a
// full extra second. NeoLooper's samples are usually much shorter than
// that (one-shots, sustain loops a few hundred ms to a couple of seconds),
// so here window size AND step scale with the caller's own
// minLoopSec/maxLoopSec instead of being fixed durations, and the coarse
// grid / refine radius are capped (kMaxCoarsePositions, kRefineRadiusCap)
// so the search stays fast enough to run synchronously on a button click
// regardless of how long the source file or how fine the requested window
// is -- the original has no such cap and is meant to be run offline.
#pragma once

#include <cstdint>

#include "../audio_data.hpp"

namespace autoloopdsp {

struct Params {
    double minLoopSec = 0.03;
    double maxLoopSec = 2.0;
    int numWindows = 6; // candidate loop lengths tried between min and max
};

struct Result {
    bool ok = false;
    int64_t start = 0;
    int64_t end = 0;
    double score = 0.0; // mean abs sample difference of the winning pair; lower = more alike
};

// Searches [searchStart, searchEnd) and returns the best-matching loop
// points found in that range. ok=false if there isn't enough audio in
// range for even the shortest requested window.
Result findLoopPoints(const AudioData& audio, int64_t searchStart, int64_t searchEnd,
                      const Params& params);

} // namespace autoloopdsp
