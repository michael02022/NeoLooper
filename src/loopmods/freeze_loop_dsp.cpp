#include "freeze_loop_dsp.hpp"

#include <algorithm>
#include <cmath>

#include "../fft.hpp"
#include "../resampler.hpp"
#include "alt_loopers_dsp.hpp"

namespace freezedsp {

void sizes(int64_t frames, double sampleRate, double freezePos, float durationMs,
          float xfadeMs, int nFft, int64_t& freezeStart, int64_t& xfadeNReq,
          int64_t& xfadeN, int64_t& nLoopSamples, bool extendXfade) {
    const int64_t lastIdx = std::max<int64_t>(0, frames - 1);
    freezePos = std::clamp(freezePos, 0.0, 1.0);
    freezeStart = (int64_t)std::llround(freezePos * (double)lastIdx);

    const int hop = nFft / 4;
    const int64_t durSamples = (int64_t)std::llround(sampleRate * (double)durationMs / 1000.0);
    const int64_t nLoopFrames = std::max<int64_t>(1, (int64_t)std::llround((double)durSamples / hop));
    nLoopSamples = nLoopFrames * (int64_t)hop;

    xfadeNReq = (int64_t)std::llround(sampleRate * (double)xfadeMs / 1000.0);
    xfadeN = extendXfade ? std::max<int64_t>(0, std::min<int64_t>(xfadeNReq, freezeStart))
                         : std::max<int64_t>(0, std::min<int64_t>({xfadeNReq, freezeStart, nLoopSamples}));
}

bool buildFreeze(const AudioData& src, double freezePos, float durationMs,
                 float offsetFrac, float xfadeMs, float xfadeCurve, float tuneCents,
                 int nFft, bool cycleMode, int rootNote, Result& out, const Extras& x) {
    const float averageMs = x.averageMs;
    const bool autoPhase = x.autoPhase;
    const float loopGainDb = x.loopGainDb;
    const bool extendXfade = x.extendXfade;
    int64_t freezeStart = 0, xfadeNReq = 0, xfadeN = 0, nLoopSamples = 0;
    sizes(src.frames(), src.sampleRate, freezePos, durationMs, xfadeMs, nFft,
          freezeStart, xfadeNReq, xfadeN, nLoopSamples, extendXfade);
    out.freezeStart = freezeStart;
    out.xfadeNRequested = xfadeNReq;
    out.cyclePeriod = 0;
    out.loopCycles = 0;
    out.averagedCycles = 0;

    const int nch = src.channels();
    // Real audio right before the freeze point: blended into the frozen
    // chunk, and what the automatic phase (autoPhase) aligns it to.
    auto referenceAt = [&](int64_t n) {
        std::vector<std::vector<double>> ref((size_t)nch, std::vector<double>((size_t)n));
        for (int c = 0; c < nch; ++c)
            for (int64_t i = 0; i < n; ++i)
                ref[(size_t)c][(size_t)i] = (double)src.ch[c][(size_t)(freezeStart - n + i)];
        return ref;
    };
    // loopGainDb: user level of the frozen chunk (not the real attack),
    // applied before the entry blend so the crossfade lands on it; loopPeak
    // reports the resulting peak of the frozen chunk.
    auto gainAndBlend = [&](std::vector<std::vector<double>>& bufs,
                            const std::vector<std::vector<double>>& reference, int64_t shift) {
        const double g = std::pow(10.0, (double)loopGainDb / 20.0);
        out.ch.resize(nch);
        out.loopPeak = 0.f;
        for (int c = 0; c < nch; ++c) {
            for (auto& v : bufs[c]) v *= g;
            // tiledBlend == rotateAndBlend while xfadeN <= the chunk; with
            // extendXfade it also tiles the chunk under a longer crossfade.
            altloop::tiledBlend(bufs[c], reference[(size_t)c], shift, out.xfadeN, xfadeCurve,
                                out.ch[c]);
            for (float v : out.ch[c].loopRegion) out.loopPeak = std::max(out.loopPeak, std::fabs(v));
        }
    };

    if (cycleMode) {
        // --- "Exact cycle" mode, see the comment in the .hpp ---
        // Target period in WHOLE samples derived from the MIDI note;
        // tuneCents adjusts the target (does NOT resample afterward:
        // resampling would break the integer period, the essence of this
        // mode).
        const double f0 = 440.0 * std::pow(2.0, ((double)rootNote - 69.0 +
                                                 (double)tuneCents / 100.0) / 12.0);
        int64_t period = (int64_t)std::llround(src.sampleRate / f0);
        period = std::max<int64_t>(2, period);

        specloop::CoreResult core;
        core.loopBuf.resize((size_t)nch);
        std::vector<std::vector<double>> cycle((size_t)nch);
        if (averageMs > 0.f) {
            // "Average": the cycle is rebuilt from a
            // SECTION starting at the freeze point instead of one STFT
            // frame -- altloop's HarmonicFreeze analysis (alt_loopers_dsp.hpp): every
            // whole cycle of the section in one FFT, each harmonic band's
            // energy collapsed onto its exact harmonic with the phase the
            // audio has at the freeze point, a single cycle of exactly
            // 'period' samples out. The timbre is the section's average;
            // at least 2 cycles are analysed.
            const double exactPeriod = altloop::notePeriod(src.sampleRate, rootNote, tuneCents);
            const int64_t sectionReq =
                (int64_t)std::llround(src.sampleRate * (double)averageMs / 1000.0);
            const int64_t section =
                std::max<int64_t>(sectionReq, (int64_t)std::ceil(2.0 * exactPeriod) + 1);
            if (freezeStart + section > src.frames()) return false;
            altloop::Params ap;
            ap.method = altloop::Method::HarmonicFreeze;
            ap.rootNote = rootNote;
            ap.tuneCents = tuneCents;
            ap.loopCycles = 1;
            altloop::Result ar;
            if (!altloop::rebuildLoop(src, freezeStart, freezeStart + section, ap, ar, cycle))
                return false;
            for (auto& cyc : cycle) cyc.resize((size_t)period, 0.0); // same rounding, defensive
            out.averagedCycles = ar.cycles;
            out.seamError = 0.0;
        } else {
            // The synthesis needs several full cycles to fold; it's enlarged if
            // the requested duration doesn't reach that (rounding to the hop,
            // same criterion as sizes()).
            const int hop = nFft / 4;
            const int64_t minSynth = ((4 * period + hop - 1) / hop) * (int64_t)hop;
            const int64_t nSynth = std::max(nLoopSamples, minSynth);

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
        }
        const int64_t durSamples =
            (int64_t)std::llround(src.sampleRate * (double)durationMs / 1000.0);

        // fractionalCycles (see Extras): k cycles of the EXACT period in
        // round(k*P) samples. The whole-sample cycle is re-laid harmonic by
        // harmonic -- bin h of its period-point DFT goes to bin h*k of a
        // unitLen-point frame (amplitude rescaled by unitLen/period) -- so
        // the result is exactly k cycles of the same waveform, band-limited,
        // with no interpolation; its phase at t=0 is kept.
        int64_t unitCycles = 1, unitLen = period;
        if (x.fractionalCycles) {
            const double exactPeriod = src.sampleRate / f0;
            unitCycles = x.loopCycles > 0
                             ? x.loopCycles
                             : std::max<int64_t>(1, (int64_t)std::llround((double)durSamples /
                                                                          exactPeriod));
            if (unitCycles > 1) {
                unitLen = std::max<int64_t>(2, (int64_t)std::llround((double)unitCycles * exactPeriod));
                const double scale = (double)unitLen / (double)period;
                for (int c = 0; c < nch; ++c) {
                    const auto X = dsp::rfft(cycle[(size_t)c]);
                    std::vector<dsp::Complex> Y((size_t)(unitLen / 2 + 1), dsp::Complex(0.0, 0.0));
                    Y[0] = X[0] * scale;
                    for (int64_t h = 1; 2 * h < period; ++h) {
                        const int64_t dst = h * unitCycles;
                        if (2 * dst >= unitLen) break;
                        Y[(size_t)dst] = X[(size_t)h] * scale;
                    }
                    cycle[(size_t)c] = dsp::irfft(Y, (size_t)unitLen);
                }
            }
        }
        out.cyclePeriod = unitLen;
        out.loopCycles = unitCycles;

        // Tiling: N bit-identical cycles until the requested duration is
        // covered (durationMs ~0 -> exactly ONE cycle, the minimum memory
        // footprint). fractionalCycles: Duration already sized the k-cycle
        // unit, which is the whole chunk.
        const int64_t nCyclesOut =
            x.fractionalCycles ? 1
            : x.loopCycles > 0 ? x.loopCycles
                               : std::max<int64_t>(1, (int64_t)std::llround((double)durSamples /
                                                                            (double)period));
        nLoopSamples = nCyclesOut * unitLen;
        for (int c = 0; c < nch; ++c) {
            auto& buf = core.loopBuf[(size_t)c];
            buf.resize((size_t)nLoopSamples);
            for (int64_t j = 0; j < nCyclesOut; ++j)
                for (int64_t t = 0; t < unitLen; ++t)
                    buf[(size_t)(j * unitLen + t)] = cycle[(size_t)c][(size_t)t];
        }
        out.nLoopSamples = nLoopSamples;
        if (!extendXfade) xfadeN = std::min(xfadeN, nLoopSamples);
        out.xfadeN = xfadeN;

        // The rotation spans a SINGLE cycle across the full slider (rotating
        // past one period would be identity: the cycles are identical).
        // autoPhase: the rotation (within one cycle, the body is periodic)
        // that best aligns the cycle with the attack; Offset then adds a
        // manual fine adjustment on top.
        const auto reference = referenceAt(xfadeN);
        int64_t shift = (int64_t)std::llround((double)offsetFrac * (double)unitLen /
                                              (double)unitCycles);
        if (autoPhase && xfadeN > 0) shift += altloop::bestShift(cycle, reference);
        shift %= nLoopSamples;
        if (shift < 0) shift += nLoopSamples;
        out.shift = shift;

        gainAndBlend(core.loopBuf, reference, shift);
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
    if (!extendXfade)
        xfadeN = std::min(xfadeN, nLoopSamples); // resampling may have shortened the period
    out.xfadeN = xfadeN;

    // MANUAL rotation: fraction [0,1) of the ALREADY-TUNED period, no
    // correlation search -- the user adjusts it by ear/eye with the offset
    // slider.
    // autoPhase adds the correlation-found rotation under the manual one.
    const auto reference = referenceAt(xfadeN);
    int64_t shift = (int64_t)std::llround((double)offsetFrac * (double)nLoopSamples);
    if (autoPhase && xfadeN > 0) shift += altloop::bestShift(core.loopBuf, reference);
    shift %= nLoopSamples;
    if (shift < 0) shift += nLoopSamples;
    out.shift = shift;

    gainAndBlend(core.loopBuf, reference, shift);

    return true;
}

} // namespace freezedsp
