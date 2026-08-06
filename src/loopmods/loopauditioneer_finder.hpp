// DSP core of "LoopAuditioneer" (github.com/GrandOrgue/LoopAuditioneer,
// AutoLooping.cpp/FileHandling.cpp, by Lars Palo), with no ImGui dependency
// (so it can be tested from core_test without dragging in the GUI).
//
// Unlike milselarch/autolooper (auto_loop_finder.hpp), which compares two
// LONG windows of audio for overall similarity, LoopAuditioneer is built
// for pipe organ sustain samples and works very differently:
//   1. Picks the "sustain section" -- the stable part of the sample, after
//      the attack transient and before any release -- either automatically
//      (an RMS-window heuristic, port of FileHandling::
//      CalculateSustainStartAndEnd) or, if autoSustain is off, takes
//      [searchStart,searchEnd) as-is (the original's manual sustain
//      sliders; here that's whatever the caller passed in, i.e. the
//      selection or the whole sample).
//   2. Within the sustain section, finds every sample whose derivative
//      (sample-to-sample change) is small relative to the section's peak
//      derivative -- candidate splice points where the waveform isn't
//      moving fast, so a cut there is less likely to click.
//   3. Matches candidate pairs (a start against ends at least
//      minLoopDurationSec away, tried longest-first) by comparing just 5
//      samples of context around each edge (port of FileHandling::
//      CalculateLoopQuality) -- cheap, local "does the trajectory
//      approaching each point match" check, not a comparison of the whole
//      prospective loop.
//   4. Returns several ranked candidates (not just the best one) so they
//      can be auditioned -- this multiplicity is the tool's whole point,
//      per its name and its own help text ("only your ears will be able
//      to tell if a loop is good enough").
//
// The original's real-world defaults (maxCandidates=50000, brute force
// optionally comparing every candidate against every other) assume a
// background search with a progress dialog; NeoLooper has no such
// infrastructure (this runs synchronously on a button click), so
// kHardCandidateCap bounds the candidate list regardless of what's
// requested, keeping the O(candidates^2) matching phase fast.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../audio_data.hpp"

namespace loopauditioneerdsp {

struct Params {
    double derivativeThreshold = 0.03;        // fraction of the sustain section's peak |derivative|
    double minLoopDurationSec = 1.0;
    double minDistanceBetweenLoopsSec = 0.3;
    double qualityFactor = 0.01;              // max allowed 5-sample edge difference (amplitude -1..1)
    int maxCandidates = 2000;                 // clamped to kHardCandidateCap regardless
    int loopsToReturn = 6;
    int maxLoopsMultiple = 10;                // candidate pool size = loopsToReturn * maxLoopsMultiple
    bool useBruteForce = false;               // match every candidate against every other; no early exit
    bool autoSustain = true;                  // false: use [searchStart,searchEnd) as the sustain section as-is
};

struct Candidate {
    int64_t start = 0;
    int64_t end = 0;
    double quality = 0.0; // lower = better (see CalculateLoopQuality port)
};

struct Result {
    bool ok = false;
    int64_t sustainStart = 0, sustainEnd = 0; // the section actually searched, for UI feedback
    std::vector<Candidate> loops;             // best (lowest quality) first
};

// Hard ceiling on the candidate list used by the matching phase (phase 3
// above), independent of Params::maxCandidates -- see the file comment.
constexpr int kHardCandidateCap = 2500;

Result findLoops(const AudioData& audio, int64_t searchStart, int64_t searchEnd, const Params& params);

} // namespace loopauditioneerdsp
