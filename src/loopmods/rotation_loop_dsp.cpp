#include "rotation_loop_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "alt_loopers_dsp.hpp" // bestShift
#include "crossfade_inside_core.hpp"
#include "loop_module.hpp"     // xfadeGains

namespace rotationdsp {

void sizes(int64_t ls, int64_t le, double sampleRate, float xfadeMs, float ccPct,
          int64_t& xfadeNReq, int64_t& xfadeN, int64_t& nLoopSamples, int64_t& ccAvail) {
    nLoopSamples = le - ls;
    xfadeNReq = (int64_t)std::llround(sampleRate * xfadeMs / 1000.0);
    xfadeN = std::max<int64_t>(0, std::min<int64_t>({xfadeNReq, ls, nLoopSamples}));
    ccAvail = ccinside::fadeLen(nLoopSamples, ccPct);
}

namespace {

// offsets[0]==0 (reference layer, at the original phase, see
// build_rotation_layers in rotation_loop.py); offsets[1..nLayers-1] random
// in [0, loopLen). seed<0 -> fresh OS entropy on every call; seed>=0 ->
// reproducible. The
// same offsets are used across all channels (a layer is "the same rotation"
// in L and R, so as not to decorrelate the original material's stereo
// image).
std::vector<int64_t> layerOffsets(int64_t loopLen, int nLayers, int64_t seed) {
    std::vector<int64_t> offsets((size_t)nLayers, 0);
    std::mt19937_64 rng(seed < 0 ? std::random_device{}() : (uint64_t)seed);
    std::uniform_int_distribution<int64_t> dist(0, loopLen - 1);
    for (int i = 1; i < nLayers; ++i) offsets[(size_t)i] = dist(rng);
    return offsets;
}

} // namespace

bool buildRotationLoop(const AudioData& src, int64_t ls, int64_t le, int nLayers, int64_t seed,
                       float xfadeMs, float xfadeCurve, float ccPct, float ccShape, Result& out,
                       bool alignPerChannel) {
    if (le <= ls || nLayers < 2 || nLayers > 100) return false;
    const int64_t loopLen = le - ls;

    int64_t xfadeNReq = 0, xfadeN = 0, nLoopSamples = 0, ccAvail = 0;
    sizes(ls, le, src.sampleRate, xfadeMs, ccPct, xfadeNReq, xfadeN, nLoopSamples, ccAvail);
    out.xfadeNRequested = xfadeNReq;
    out.xfadeN = xfadeN;
    out.nLoopSamples = nLoopSamples;
    out.ccAvail = ccAvail;
    // The loop that gets rotated: the region minus the head Classic
    // Crossfade (Inside) excludes.
    const int64_t m = loopLen - ccAvail;
    out.offsets = layerOffsets(m, nLayers, seed);

    const int nch = src.channels();
    std::vector<std::vector<double>> combined((size_t)nch), reference((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        const auto& x = src.ch[c];

        // 1. Seamless loop: [ls+ccAvail, le) with its tail blended toward
        // the head [ls, ls+ccAvail) (Classic Crossfade Inside).
        std::vector<float> head(x.begin() + ls, x.begin() + ls + ccAvail);
        std::vector<float> tail(x.begin() + le - ccAvail, x.begin() + le);
        const std::vector<float> blendedTail = ccinside::blendTail(head, tail, ccShape);
        std::vector<double> loop((size_t)m);
        for (int64_t i = 0; i < m - ccAvail; ++i) loop[(size_t)i] = x[(size_t)(ls + ccAvail + i)];
        for (int64_t i = 0; i < ccAvail; ++i) loop[(size_t)(m - ccAvail + i)] = blendedTail[(size_t)i];

        // 2. Layers summed without DC, scaled to the loop's own energy, DC
        // added back (see the .hpp).
        double dc = 0.0;
        for (double v : loop) dc += v;
        dc /= (double)m;
        double eLoop = 0.0;
        for (auto& v : loop) {
            v -= dc;
            eLoop += v * v;
        }
        auto& sum = combined[(size_t)c];
        sum.assign((size_t)m, 0.0);
        for (int64_t o : out.offsets)
            for (int64_t i = 0; i < m; ++i) sum[(size_t)i] += loop[(size_t)((i + o) % m)];
        double eSum = 0.0;
        for (double v : sum) eSum += v * v;
        const double g = eSum > 1e-30 ? std::sqrt(eLoop / eSum) : 0.0;
        for (double& v : sum) v = v * g + dc;

        reference[(size_t)c].resize((size_t)xfadeN);
        for (int64_t i = 0; i < xfadeN; ++i)
            reference[(size_t)c][(size_t)i] = (double)x[(size_t)(ls - xfadeN + i)];
    }

    // 3. Attack alignment: one rotation for every channel, or one each.
    std::vector<int64_t> shift((size_t)nch, 0);
    if (xfadeN > 0) {
        if (alignPerChannel)
            for (int c = 0; c < nch; ++c)
                shift[(size_t)c] = altloop::bestShift({combined[(size_t)c]}, {reference[(size_t)c]});
        else
            std::fill(shift.begin(), shift.end(), altloop::bestShift(combined, reference));
    }

    out.peak = 0.f;
    out.ch.resize((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        const auto& sum = combined[(size_t)c];
        const auto& ref = reference[(size_t)c];
        auto lb = [&](int64_t i) { return (float)sum[(size_t)((i + shift[(size_t)c]) % m)]; };
        Result::Channel rc;
        rc.attackTail.resize((size_t)xfadeN);
        for (int64_t j = 0; j < xfadeN; ++j) {
            const float t = (xfadeN > 1) ? (float)j / (float)(xfadeN - 1) : 0.f;
            float gIn, gOut;
            xfadeGains(t, xfadeCurve, gIn, gOut);
            rc.attackTail[(size_t)j] = (float)ref[(size_t)j] * gOut + lb(j) * gIn;
        }
        rc.ccHead.resize((size_t)ccAvail);
        for (int64_t j = 0; j < ccAvail; ++j) rc.ccHead[(size_t)j] = lb(xfadeN + j);
        rc.loopBlended.resize((size_t)m);
        for (int64_t i = 0; i < m; ++i) {
            rc.loopBlended[(size_t)i] = lb(xfadeN + ccAvail + i);
            out.peak = std::max(out.peak, std::fabs(rc.loopBlended[(size_t)i]));
        }
        out.ch[(size_t)c] = std::move(rc);
    }

    return true;
}

} // namespace rotationdsp
