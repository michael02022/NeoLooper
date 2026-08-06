#include "granular_loop_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace granulardsp {

void sizes(int64_t ls, int64_t le, float headPct, float grainMinPct, float grainMaxPct,
          int nGrains, int64_t& headLen, int64_t& splitPoint, int64_t& estLoopMin,
          int64_t& estLoopMax) {
    const int64_t width = std::max<int64_t>(0, le - ls);
    const float hp = std::clamp(headPct, 5.f, 95.f);
    headLen = std::clamp<int64_t>((int64_t)std::llround((double)width * (double)hp / 100.0), 0, width);
    splitPoint = ls + headLen;
    const int64_t seedLen =
        std::clamp<int64_t>(width / 4, 0, std::max<int64_t>(0, le - splitPoint));

    const float gMinPct = std::clamp(std::min(grainMinPct, grainMaxPct), 0.f, 100.f);
    const float gMaxPct = std::clamp(std::max(grainMinPct, grainMaxPct), 0.f, 100.f);
    const int64_t grainMin = std::max<int64_t>(1, (int64_t)std::llround((double)width * gMinPct / 100.0));
    const int64_t grainMax =
        std::max<int64_t>(grainMin, (int64_t)std::llround((double)width * gMaxPct / 100.0));

    const int64_t n = std::max(0, nGrains);
    // Only the LAST grain gets recomputed to close the loop (it doesn't draw
    // a length); the n-1 before it draw in [grainMin, grainMax]. This is an
    // estimate -- the real value depends on the seed and each grain's
    // phase search.
    const int64_t randomGrains = std::max<int64_t>(0, n - 1);
    estLoopMin = seedLen + randomGrains * grainMin;
    estLoopMax = seedLen + randomGrains * grainMax;
}

namespace {

// Parabolic fit of the autocorrelation peak (adapted from 'parabolic' in
// the reference script): refines the integer position 'x' to sub-sample
// precision using its 2 neighbors.
double parabolicPeakX(const std::vector<double>& f, int64_t x) {
    if (x <= 0 || x >= (int64_t)f.size() - 1) return (double)x;
    const double denom = f[(size_t)(x - 1)] - 2.0 * f[(size_t)x] + f[(size_t)(x + 1)];
    if (denom == 0.0) return (double)x;
    return 0.5 * (f[(size_t)(x - 1)] - f[(size_t)(x + 1)]) / denom + (double)x;
}

} // namespace

int64_t estimatePeriod(const AudioData& src, int64_t start, int64_t len) {
    if (src.channels() < 1 || len < 64 || start < 0) return 0;
    // Direct autocorrelation (no FFT): O(analysisLen * maxLag). Called only
    // once per render() (not per grain), so it can afford to be more
    // generous than the per-grain phase search, but it's still capped so it
    // doesn't hang the GUI on regions several seconds long.
    const int64_t analysisLen = std::min<int64_t>(len, 16384);
    const auto& x = src.ch[0];
    const int64_t maxLag =
        std::min<int64_t>(analysisLen / 2, (int64_t)(src.sampleRate / 20.0));
    if (maxLag < 2) return 0;

    std::vector<double> corr((size_t)maxLag + 1, 0.0);
    for (int64_t lag = 0; lag <= maxLag; ++lag) {
        double s = 0.0;
        for (int64_t i = 0; i < analysisLen - lag; ++i)
            s += (double)x[(size_t)(start + i)] * (double)x[(size_t)(start + i + lag)];
        corr[(size_t)lag] = s;
    }

    // First lag where the autocorrelation rises again (end of the initial
    // drop from the peak at lag=0), then the maximum from there on -- same
    // criterion as 'start'/'peak' in freq_from_autocorr.
    int64_t d = 1;
    while (d <= maxLag && corr[(size_t)d] <= corr[(size_t)(d - 1)]) ++d;
    if (d > maxLag) return 0; // no clear periodicity (silence or noise)

    int64_t peak = d;
    for (int64_t i = d; i <= maxLag; ++i)
        if (corr[(size_t)i] > corr[(size_t)peak]) peak = i;

    const double px = parabolicPeakX(corr, peak);
    return std::max<int64_t>(1, (int64_t)std::llround(px));
}

namespace {
struct Grain {
    std::vector<std::vector<float>> ch;
    int64_t frames() const { return ch.empty() ? 0 : (int64_t)ch[0].size(); }
};
} // namespace

bool buildGranularLoop(const AudioData& src, int64_t ls, int64_t le, int nGrains,
                       float grainMinPct, float grainMaxPct, float headPct,
                       int64_t seed, Result& out) {
    if (le <= ls || nGrains < 1 || src.channels() < 1) return false;
    const int64_t width = le - ls;
    if (width < 2) return false; // needs at least 1 head sample and 1 body sample
    const int nch = src.channels();
    const int64_t srcFrames = src.frames();
    if (le > srcFrames) return false;

    const float hp = std::clamp(headPct, 5.f, 95.f);
    int64_t headLen = (int64_t)std::llround((double)width * (double)hp / 100.0);
    headLen = std::clamp<int64_t>(headLen, 1, width - 1);
    const int64_t splitPoint = ls + headLen;
    if (splitPoint >= le) return false;

    int64_t seedLen = std::clamp<int64_t>(width / 4, 1, le - splitPoint);

    const float gMinPct = std::clamp(std::min(grainMinPct, grainMaxPct), 1.f, 100.f);
    const float gMaxPct = std::clamp(std::max(grainMinPct, grainMaxPct), 1.f, 100.f);
    int64_t grainMin =
        std::max<int64_t>(1, (int64_t)std::llround((double)width * (double)gMinPct / 100.0));
    int64_t grainMax =
        std::max<int64_t>(grainMin, (int64_t)std::llround((double)width * (double)gMaxPct / 100.0));
    // No grain (including the one that becomes the last iteration's
    // 'overlap') can be longer than headLen: if it were, the last iteration
    // would start with no room left to close at splitPoint (see the
    // additional searchMax cap below) and the exact-closure guarantee would
    // break. The seed is trimmed for the same reason (it can become the
    // 'overlap' of the last iteration itself if nGrains==1). If this pushes
    // grainMin above grainMax (very small headPct + very large Grain max, a
    // contradictory slider combination), grainMin is trimmed too -- exact
    // closure matters more than honoring Grain min to the letter in that
    // edge case.
    grainMax = std::max<int64_t>(1, std::min(grainMax, headLen - 1));
    grainMin = std::min(grainMin, grainMax);
    seedLen = std::max<int64_t>(1, std::min(seedLen, headLen - 1));

    int64_t period = estimatePeriod(src, ls, width);
    if (period < 1) period = 1; // degenerate fallback: the phase search falls back to a single candidate
    // No artificial ceiling on 'period': the original script searches the
    // FULL estimated wavelength (range(int(wavelength)+1)), without
    // trimming it for cost -- fidelity to the original algorithm takes
    // priority over preview latency.

    std::mt19937_64 rng(seed < 0 ? std::random_device{}() : (uint64_t)seed);
    std::uniform_int_distribution<int64_t> grainLenDist(grainMin, grainMax);

    out.headLen = headLen;
    out.period = period;
    out.grainsClamped = false;
    out.head.assign((size_t)nch, {});
    for (int c = 0; c < nch; ++c)
        out.head[(size_t)c].assign(src.ch[(size_t)c].begin() + ls,
                                   src.ch[(size_t)c].begin() + splitPoint);

    // Grain chain: chain[0] = seed (analogous to segments[1] in the
    // script); each iteration blends chain.back() IN PLACE with a new
    // phase-aligned fragment and appends a new raw fragment, which becomes
    // the tail for the next iteration.
    std::vector<Grain> chain;
    chain.reserve((size_t)nGrains + 1);
    {
        Grain g;
        g.ch.resize((size_t)nch);
        for (int c = 0; c < nch; ++c)
            g.ch[(size_t)c].assign(src.ch[(size_t)c].begin() + splitPoint,
                                   src.ch[(size_t)c].begin() + splitPoint + seedLen);
        chain.push_back(std::move(g));
    }

    for (int i = 0; i < nGrains; ++i) {
        Grain& cur = chain.back();
        const int64_t overlap = cur.frames();
        const bool lastIter = (i == nGrains - 1);
        int64_t nextLen = 0;
        int64_t start;

        if (!lastIter) {
            nextLen = grainLenDist(rng);
            const int64_t maxNext = std::max<int64_t>(1, width - overlap - 1);
            if (nextLen > maxNext) {
                nextLen = maxNext;
                out.grainsClamped = true;
            }
            int64_t hi = le - overlap - nextLen;
            if (hi < ls) {
                hi = ls; // region too short for the requested parameters: degrades, doesn't fail
                out.grainsClamped = true;
            }
            std::uniform_int_distribution<int64_t> startDist(ls, hi);
            start = startDist(rng);
        } else {
            start = ls; // forced anchor -- same trick as 'first' in the original script
        }

        // Phase search: a single offset (0..searchMax), shared by all
        // channels (so as not to decorrelate the stereo image, same
        // criterion as Rotation), that minimizes the L1 error against the
        // current tail. Also capped against srcFrames so it never reads
        // outside the real buffer if the parameters force a grain past
        // [ls,le) (see 'grainsClamped'). On the LAST iteration it's further
        // capped by splitPoint: without this limit, an offset that pushes
        // start+overlap past splitPoint would force nextLen to 0 below and
        // silently break the exact-closure guarantee (the body would end
        // on an arbitrary grain, not on the anchor sample) -- with grainMax
        // already capped below headLen, there's always >=1 of margin left
        // for this case.
        const int64_t searchMax = std::max<int64_t>(
            0, std::min({period, le - start - overlap, srcFrames - start - overlap,
                        lastIter ? (splitPoint - start - overlap) : (le - ls)}));
        // The search costs O(searchMax * overlap * nch); with long grains
        // this can take a while -- the comparison is deliberately NOT
        // trimmed (Peastman's original algorithm compares the FULL overlap
        // on every candidate, not a partial window): trimming it would
        // degrade the phase-alignment quality, which is the heart of the
        // method. Fidelity to the original algorithm is prioritized over
        // preview latency.
        int64_t bestOffset = 0;
        double bestErr = std::numeric_limits<double>::infinity();
        for (int64_t o = 0; o <= searchMax; ++o) {
            double err = 0.0;
            for (int c = 0; c < nch; ++c) {
                const auto& cx = src.ch[(size_t)c];
                const auto& cb = cur.ch[(size_t)c];
                for (int64_t k = 0; k < overlap; ++k)
                    err += std::fabs((double)cb[(size_t)k] - (double)cx[(size_t)(start + o + k)]);
            }
            if (err < bestErr) {
                bestErr = err;
                bestOffset = o;
            }
        }
        start += bestOffset;

        // In-place blend of cur (outgoing) with src[start:start+overlap)
        // (incoming): raised cosine (Hann) equal-gain, IDENTICAL to the
        // reference script (blend = 0.5 - 0.5*cos(linspace(0, pi, overlap))
        // -- linspace includes both endpoints, hence t=k/(overlap-1)). Sums
        // to 1 at every t (no +3dB hump) and has zero derivative at the
        // endpoints; this is NOT the other modules' xfadeGains() -- no
        // value of the linear<->equal-power morph reproduces this curve.
        // t=0 EXACT at k=0 (gIn=0) and t=1 EXACT at k=overlap-1 (gIn=1) --
        // essential for the exact-closure guarantee documented in the .hpp
        // (do NOT use ccinside::blendTail's t=(k+1)/overlap parametrization
        // here, which never touches 0).
        for (int c = 0; c < nch; ++c) {
            const auto& cx = src.ch[(size_t)c];
            auto& cb = cur.ch[(size_t)c];
            for (int64_t k = 0; k < overlap; ++k) {
                const float t = (overlap > 1) ? (float)k / (float)(overlap - 1) : 0.f;
                const float gIn = 0.5f - 0.5f * std::cos((float)M_PI * t);
                cb[(size_t)k] = (1.f - gIn) * cb[(size_t)k] + gIn * cx[(size_t)(start + k)];
            }
        }

        if (lastIter) {
            // Recomputes the last grain's length to end EXACTLY at
            // splitPoint -- see the comment in the .hpp: this is what turns
            // the body's wrap into two adjacent samples of the original,
            // not a synthetic seam.
            nextLen = std::max<int64_t>(0, splitPoint - (start + overlap));
        }
        nextLen = std::min(nextLen, std::max<int64_t>(0, le - (start + overlap)));
        nextLen = std::min(nextLen, std::max<int64_t>(0, srcFrames - (start + overlap)));

        Grain g;
        g.ch.resize((size_t)nch);
        for (int c = 0; c < nch; ++c) {
            const auto& cx = src.ch[(size_t)c];
            g.ch[(size_t)c].assign(cx.begin() + start + overlap, cx.begin() + start + overlap + nextLen);
        }
        chain.push_back(std::move(g));
    }

    int64_t total = 0;
    for (auto& g : chain) total += g.frames();
    out.nLoopSamples = total;
    out.body.assign((size_t)nch, {});
    for (int c = 0; c < nch; ++c) {
        auto& v = out.body[(size_t)c];
        v.reserve((size_t)total);
        for (auto& g : chain) v.insert(v.end(), g.ch[(size_t)c].begin(), g.ch[(size_t)c].end());
    }

    return true;
}

} // namespace granulardsp
