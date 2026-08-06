#include "auto_loop_finder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace autoloopdsp {
namespace {

// Mean absolute difference between windows [a,a+len) and [b,b+len) across
// all channels, sampling every 'stride' frames (stride=1 = exhaustive).
// Lower = more alike. Same idea as autolooper's find_difference(), just on
// float samples instead of shorts.
double windowDiff(const AudioData& audio, int64_t a, int64_t b, int64_t len, int64_t stride) {
    const int nch = audio.channels();
    double sum = 0.0;
    int64_t count = 0;
    for (int64_t i = 0; i < len; i += stride) {
        for (int c = 0; c < nch; ++c) {
            double d = (double)audio.ch[c][(size_t)(a + i)] - (double)audio.ch[c][(size_t)(b + i)];
            sum += std::fabs(d);
            ++count;
        }
    }
    return count ? sum / (double)count : std::numeric_limits<double>::max();
}

// Bounds on the coarse (start,end) grid and the local refine search --
// keep the whole thing fast enough for a synchronous button click no
// matter how long the file or how small the requested window is.
constexpr int64_t kMaxCoarsePositions = 220;
constexpr int64_t kCoarseStrideDivisor = 64;
constexpr int64_t kRefineRadiusCap = 4000; // samples
constexpr double kRefineWidthSec = 0.05;

} // namespace

Result findLoopPoints(const AudioData& audio, int64_t searchStart, int64_t searchEnd,
                      const Params& params) {
    Result res;
    if (audio.frames() <= 0 || audio.channels() == 0) return res;
    searchStart = std::max<int64_t>(0, searchStart);
    searchEnd = std::min<int64_t>(audio.frames(), searchEnd);
    const int64_t range = searchEnd - searchStart;
    if (range < 4) return res;

    const double sr = audio.sampleRate > 0.0 ? audio.sampleRate : 44100.0;
    const double minSec = std::max(0.001, std::min(params.minLoopSec, params.maxLoopSec));
    const double maxSec = std::max(minSec, params.maxLoopSec);
    int64_t minW = std::max<int64_t>(2, (int64_t)std::llround(minSec * sr));
    int64_t maxW = std::max<int64_t>(minW, (int64_t)std::llround(maxSec * sr));
    // A candidate loop region needs room for its own window plus at least
    // one more window's worth of audio ahead of it (end is searched
    // starting at start+window, see the loop below).
    maxW = std::min(maxW, range / 2);
    if (maxW < minW) maxW = minW;
    if (minW > range / 2) return res; // not enough audio for even the shortest window

    const int numWindows = std::max(1, params.numWindows);
    double bestScore = std::numeric_limits<double>::max();
    int64_t bestStart = 0, bestEnd = 0;
    bool found = false;

    for (int wi = 0; wi < numWindows; ++wi) {
        const double t = numWindows == 1 ? 0.0 : (double)wi / (double)(numWindows - 1);
        int64_t W = minW + (int64_t)std::llround(t * (double)(maxW - minW));
        W = std::max<int64_t>(2, std::min(W, range / 2));

        // --- phase 1: coarse scan for the most alike (start,end) pair ---
        int64_t step = std::max<int64_t>(1, W / 16);
        const int64_t nStarts = (range - W) / step + 1;
        if (nStarts > kMaxCoarsePositions)
            step = std::max<int64_t>(1, (range - W) / kMaxCoarsePositions);
        const int64_t coarseStride = std::max<int64_t>(1, W / kCoarseStrideDivisor);

        int64_t coarseStart = -1, coarseEnd = -1;
        double coarseScore = std::numeric_limits<double>::max();
        for (int64_t s = searchStart; s + W <= searchEnd; s += step) {
            for (int64_t e = s + W; e + W <= searchEnd; e += step) {
                double sc = windowDiff(audio, s, e, W, coarseStride);
                // <= (not <): prefer the FURTHEST matching pair among ties,
                // same reasoning as the original -- otherwise it tends to
                // lock onto two adjacent, near-identical sub-windows of the
                // same phrase instead of a genuine loop boundary.
                if (sc <= coarseScore) {
                    coarseScore = sc;
                    coarseStart = s;
                    coarseEnd = e;
                }
            }
        }
        if (coarseStart < 0) continue;

        // --- phase 2: sample-accurate local refine of the end point ---
        const int64_t refineRadius = std::min(step, kRefineRadiusCap);
        const int64_t refineW =
            std::max<int64_t>(2, std::min(W, (int64_t)std::llround(kRefineWidthSec * sr)));
        int64_t bestOffset = 0;
        double bestLocalScore = std::numeric_limits<double>::max();
        for (int64_t off = -refineRadius; off <= refineRadius; ++off) {
            int64_t e = coarseEnd + off;
            // Bounded by W (not just refineW): refinedEnd is re-scored over
            // the full window below, so it must stay valid for that too.
            // off=0 (e==coarseEnd) always satisfies both bounds since
            // coarseEnd was chosen from phase 1's e-loop, which enforces
            // them already -- so this loop always finds at least one
            // candidate.
            if (e < coarseStart + W || e + W > searchEnd) continue;
            double sc = windowDiff(audio, coarseStart, e, refineW, 1);
            if (sc <= bestLocalScore) {
                bestLocalScore = sc;
                bestOffset = off;
            }
        }
        const int64_t refinedEnd = coarseEnd + bestOffset;

        // Re-score the refined pair over the full window so window sizes
        // are compared on equal footing.
        const int64_t finalStride = std::max<int64_t>(1, W / 256);
        const double finalScore = windowDiff(audio, coarseStart, refinedEnd, W, finalStride);
        if (finalScore <= bestScore) {
            bestScore = finalScore;
            bestStart = coarseStart;
            bestEnd = refinedEnd;
            found = true;
        }
    }

    if (!found) return res;
    res.ok = true;
    res.start = bestStart;
    res.end = bestEnd;
    res.score = bestScore;
    return res;
}

} // namespace autoloopdsp
