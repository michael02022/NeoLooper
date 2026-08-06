#include "rotation_loop_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "crossfade_inside_core.hpp"

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
// in [0, loopLen). seed<0 -> fresh OS entropy on every call (equivalent to
// np.random.default_rng(None) in the script); seed>=0 -> reproducible. The
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
                       float xfadeMs, float xfadeCurve, float ccPct, float ccShape, Result& out) {
    if (le <= ls || nLayers < 2 || nLayers > 100) return false;
    const int64_t loopLen = le - ls;

    int64_t xfadeNReq = 0, xfadeN = 0, nLoopSamples = 0, ccAvail = 0;
    sizes(ls, le, src.sampleRate, xfadeMs, ccPct, xfadeNReq, xfadeN, nLoopSamples, ccAvail);
    out.xfadeNRequested = xfadeNReq;
    out.xfadeN = xfadeN;
    out.nLoopSamples = nLoopSamples;
    out.ccAvail = ccAvail;
    out.offsets = layerOffsets(loopLen, nLayers, seed);
    out.safetyNormalized = false;

    // equal-power: see compute_layer_gains(falloff_db=None) in the script --
    // each layer is the same period at a different phase, so for loudness
    // purposes they behave as decorrelated sources: n copies at 1/sqrt(n)
    // sum to ~the same RMS as one copy at unity gain, regardless of
    // nLayers.
    const double gain = 1.0 / std::sqrt((double)nLayers);

    const int nch = src.channels();
    out.ch.resize((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        const auto& x = src.ch[c];

        std::vector<double> loopBuffer((size_t)loopLen);
        double referencePeak = 0.0;
        for (int64_t i = 0; i < loopLen; ++i) {
            const double v = (double)x[(size_t)(ls + i)];
            loopBuffer[(size_t)i] = v;
            referencePeak = std::max(referencePeak, std::fabs(v));
        }

        std::vector<double> combined((size_t)loopLen, 0.0);
        for (int l = 0; l < nLayers; ++l) {
            const int64_t o = out.offsets[(size_t)l];
            for (int64_t i = 0; i < loopLen; ++i)
                combined[(size_t)i] += gain * loopBuffer[(size_t)((i + o) % loopLen)];
        }
        // Safety normalization ONLY if the combined peak exceeds the
        // original peak: never amplifies, only prevents clipping when the
        // random offsets happen to align several layers in phase (see
        // combine_layers in the script).
        double peak = 0.0;
        for (double v : combined) peak = std::max(peak, std::fabs(v));
        if (peak > referencePeak && peak > 0.0) {
            const double s = referencePeak / peak;
            for (double& v : combined) v *= s;
            out.safetyNormalized = true;
        }

        // Splice with the real attack: same technique as Harmonic (rotation
        // via circular correlation + blend), reusing specloop instead of
        // reimplementing it -- see spectral_loop_core.hpp.
        std::vector<double> reference((size_t)xfadeN);
        for (int64_t i = 0; i < xfadeN; ++i)
            reference[(size_t)i] = (double)x[(size_t)(ls - xfadeN + i)];
        const int64_t shift =
            (xfadeN > 0) ? specloop::circularBestShift(combined, reference) : 0;

        specloop::RotatedBlend blend;
        specloop::rotateAndBlend(combined, reference, shift, xfadeN, xfadeCurve, blend);

        // Last step: Classic Crossfade (Inside), reused as-is over the
        // loopRegion already spliced with the attack -- blends its own tail
        // into its own head; the head then gets excluded from the loop
        // (added to the attack, right after attackTail) same as
        // ClassicCrossfadeInside does over the document's marked loop.
        std::vector<float> ccHead(blend.loopRegion.begin(), blend.loopRegion.begin() + ccAvail);
        std::vector<float> ccTail(blend.loopRegion.end() - ccAvail, blend.loopRegion.end());
        std::vector<float> blendedTail = ccinside::blendTail(ccHead, ccTail, ccShape);

        Result::Channel rc;
        rc.attackTail = std::move(blend.attackTail);
        rc.loopBlended.assign(blend.loopRegion.begin() + ccAvail, blend.loopRegion.end() - ccAvail);
        rc.loopBlended.insert(rc.loopBlended.end(), blendedTail.begin(), blendedTail.end());
        rc.ccHead = std::move(ccHead);
        out.ch[(size_t)c] = std::move(rc);
    }

    return true;
}

} // namespace rotationdsp
