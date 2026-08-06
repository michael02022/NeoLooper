#include "freeze_loop_dsp.hpp"

#include <algorithm>
#include <cmath>

#include "../resampler.hpp"

namespace freezedsp {

void sizes(int64_t frames, double sampleRate, double freezePos, float durationMs,
          float xfadeMs, int nFft, int64_t& freezeStart, int64_t& xfadeNReq,
          int64_t& xfadeN, int64_t& nLoopSamples) {
    const int64_t lastIdx = std::max<int64_t>(0, frames - 1);
    freezePos = std::clamp(freezePos, 0.0, 1.0);
    freezeStart = (int64_t)std::llround(freezePos * (double)lastIdx);

    const int hop = nFft / 4;
    const int64_t durSamples = (int64_t)std::llround(sampleRate * (double)durationMs / 1000.0);
    const int64_t nLoopFrames = std::max<int64_t>(1, (int64_t)std::llround((double)durSamples / hop));
    nLoopSamples = nLoopFrames * (int64_t)hop;

    xfadeNReq = (int64_t)std::llround(sampleRate * (double)xfadeMs / 1000.0);
    xfadeN = std::max<int64_t>(0, std::min<int64_t>({xfadeNReq, freezeStart, nLoopSamples}));
}

bool buildFreeze(const AudioData& src, double freezePos, float durationMs,
                 float offsetFrac, float xfadeMs, float xfadeCurve, float tuneCents,
                 int nFft, bool cycleMode, int rootNote, Result& out) {
    int64_t freezeStart = 0, xfadeNReq = 0, xfadeN = 0, nLoopSamples = 0;
    sizes(src.frames(), src.sampleRate, freezePos, durationMs, xfadeMs, nFft,
          freezeStart, xfadeNReq, xfadeN, nLoopSamples);
    out.freezeStart = freezeStart;
    out.xfadeNRequested = xfadeNReq;
    out.cyclePeriod = 0;

    const int nch = src.channels();

    if (cycleMode) {
        // --- "Exact cycle" mode (Infinity), see the comment in the .hpp ---
        // Target period in WHOLE samples derived from the MIDI note;
        // tuneCents adjusts the target (does NOT resample afterward:
        // resampling would break the integer period, the essence of this
        // mode).
        const double f0 = 440.0 * std::pow(2.0, ((double)rootNote - 69.0 +
                                                 (double)tuneCents / 100.0) / 12.0);
        int64_t period = (int64_t)std::llround(src.sampleRate / f0);
        period = std::max<int64_t>(2, period);

        // The synthesis needs several full cycles to fold; it's enlarged if
        // the requested duration doesn't reach that (rounding to the hop,
        // same criterion as sizes()).
        const int hop = nFft / 4;
        const int64_t minSynth = ((4 * period + hop - 1) / hop) * (int64_t)hop;
        const int64_t nSynth = std::max(nLoopSamples, minSynth);

        specloop::CoreResult core;
        if (!specloop::synthesize(src, freezeStart, nSynth, nFft,
                                  specloop::Method::PhaseVocoder, core, period))
            return false;

        // Folding: averages all of the synthesis's full cycles into a
        // SINGLE canonical cycle (the signal is already periodic at
        // 'period' thanks to the anchored quantization; the average removes
        // the residual overlap-add ripple, which isn't periodic at
        // 'period'). The maximum residual is reported as seamError
        // (purity diagnostic).
        const int64_t nCyclesSynth = std::max<int64_t>(1, nSynth / period);
        double foldErr = 0.0;
        std::vector<std::vector<double>> cycle((size_t)nch);
        for (int c = 0; c < nch; ++c) {
            auto& cyc = cycle[(size_t)c];
            cyc.assign((size_t)period, 0.0);
            const auto& buf = core.loopBuf[(size_t)c];
            for (int64_t j = 0; j < nCyclesSynth; ++j)
                for (int64_t t = 0; t < period; ++t)
                    cyc[(size_t)t] += buf[(size_t)(j * period + t)];
            for (auto& v : cyc) v /= (double)nCyclesSynth;
            for (int64_t j = 0; j < nCyclesSynth; ++j)
                for (int64_t t = 0; t < period; ++t)
                    foldErr = std::max(foldErr, std::fabs(buf[(size_t)(j * period + t)] -
                                                          cyc[(size_t)t]));
        }
        out.seamError = foldErr;
        out.cyclePeriod = period;

        // Tiling: N bit-identical cycles until the requested duration is
        // covered (durationMs ~0 -> exactly ONE cycle, the minimum memory
        // footprint).
        const int64_t durSamples =
            (int64_t)std::llround(src.sampleRate * (double)durationMs / 1000.0);
        const int64_t nCyclesOut = std::max<int64_t>(1, (int64_t)std::llround(
                                                            (double)durSamples / (double)period));
        nLoopSamples = nCyclesOut * period;
        for (int c = 0; c < nch; ++c) {
            auto& buf = core.loopBuf[(size_t)c];
            buf.resize((size_t)nLoopSamples);
            for (int64_t j = 0; j < nCyclesOut; ++j)
                for (int64_t t = 0; t < period; ++t)
                    buf[(size_t)(j * period + t)] = cycle[(size_t)c][(size_t)t];
        }
        out.nLoopSamples = nLoopSamples;
        xfadeN = std::min(xfadeN, nLoopSamples);
        out.xfadeN = xfadeN;

        // The rotation spans a SINGLE cycle across the full slider (rotating
        // past one period would be identity: the cycles are identical).
        int64_t shift = (int64_t)std::llround((double)offsetFrac * (double)period) % nLoopSamples;
        if (shift < 0) shift += nLoopSamples;
        out.shift = shift;

        out.ch.resize(nch);
        for (int c = 0; c < nch; ++c) {
            const auto& x = src.ch[c];
            std::vector<double> reference((size_t)xfadeN);
            for (int64_t i = 0; i < xfadeN; ++i)
                reference[(size_t)i] = (double)x[(size_t)(freezeStart - xfadeN + i)];
            specloop::rotateAndBlend(core.loopBuf[c], reference, shift, xfadeN, xfadeCurve,
                                     out.ch[c]);
        }
        return true;
    }

    specloop::CoreResult core;
    if (!specloop::synthesize(src, freezeStart, nLoopSamples, nFft,
                              specloop::Method::Naive, core))
        return false;
    out.seamError = core.seamError;

    // Manual tuning by resampling (see the comment in the .hpp: method=Naive
    // has no per-bin frequency to shift before quantizing). Changes the
    // period's real duration, so nLoopSamples is recomputed afterward from
    // the already-resampled size.
    if (tuneCents != 0.f) {
        const double pitchRatio = std::pow(2.0, (double)tuneCents / 1200.0);
        std::vector<std::vector<float>> asFloat(nch);
        for (int c = 0; c < nch; ++c)
            asFloat[c].assign(core.loopBuf[c].begin(), core.loopBuf[c].end());
        // resampleHQ(in, srIn, srOut): resamples at ratio srOut/srIn. For
        // the period to sound 'pitchRatio' times sharper (shorter, if
        // pitchRatio>1) when played back without changing speed, it's
        // treated as if its real sample rate were 'pitchRatio' and
        // resampled to 1 -- same principle as the engine's pitch-shift
        // (Engine::process), only here it's baked into the buffer instead
        // of applied live.
        const auto retuned = resampleHQ(asFloat, pitchRatio, 1.0);
        for (int c = 0; c < nch; ++c)
            core.loopBuf[c].assign(retuned[c].begin(), retuned[c].end());
        nLoopSamples = (int64_t)core.loopBuf[0].size();
    }
    out.nLoopSamples = nLoopSamples;
    xfadeN = std::min(xfadeN, nLoopSamples); // resampling may have shortened the period
    out.xfadeN = xfadeN;

    // MANUAL rotation: fraction [0,1) of the ALREADY-TUNED period, no
    // correlation search -- the user adjusts it by ear/eye with the offset
    // slider.
    int64_t shift = (int64_t)std::llround((double)offsetFrac * (double)nLoopSamples) % nLoopSamples;
    if (shift < 0) shift += nLoopSamples;
    out.shift = shift;

    out.ch.resize(nch);
    for (int c = 0; c < nch; ++c) {
        const auto& x = src.ch[c];
        std::vector<double> reference((size_t)xfadeN);
        for (int64_t i = 0; i < xfadeN; ++i)
            reference[(size_t)i] = (double)x[(size_t)(freezeStart - xfadeN + i)];
        specloop::rotateAndBlend(core.loopBuf[c], reference, shift, xfadeN, xfadeCurve, out.ch[c]);
    }

    return true;
}

} // namespace freezedsp
