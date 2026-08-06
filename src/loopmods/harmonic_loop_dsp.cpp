#include "harmonic_loop_dsp.hpp"

#include <algorithm>

namespace harmonicdsp {

void sizes(int64_t ls, int64_t le, double sampleRate, int nFft, float xfadeMs,
          bool noteMode, int rootNote, float tuneCents, int64_t& xfadeNReq,
          int64_t& xfadeN, int64_t& nLoopSamples, int64_t& notePeriod) {
    const int hop = nFft / 4;
    const int64_t loopDurTarget = le - ls;
    if (noteMode) {
        const double f0 = noteFreq(rootNote, tuneCents);
        notePeriod = std::max<int64_t>(2, (int64_t)std::llround(sampleRate / f0));
        const int64_t nCycles =
            std::max<int64_t>(1, (int64_t)std::llround((double)loopDurTarget / (double)notePeriod));
        nLoopSamples = nCycles * notePeriod;
    } else {
        notePeriod = 0;
        const int64_t nLoopFrames = std::max<int64_t>(1, (int64_t)std::llround((double)loopDurTarget / hop));
        nLoopSamples = nLoopFrames * (int64_t)hop;
    }
    xfadeNReq = (int64_t)std::llround(sampleRate * xfadeMs / 1000.0);
    xfadeN = std::max<int64_t>(0, std::min<int64_t>({xfadeNReq, ls, nLoopSamples}));
}

bool buildLoop(const AudioData& src, int64_t ls, int64_t le, int nFft, float xfadeMs,
              float xfadeCurve, bool noteMode, int rootNote, float tuneCents, Result& out) {
    int64_t xfadeNReq = 0, xfadeN = 0, nLoopSamples = 0, notePeriod = 0;
    sizes(ls, le, src.sampleRate, nFft, xfadeMs, noteMode, rootNote, tuneCents, xfadeNReq,
         xfadeN, nLoopSamples, notePeriod);
    out.xfadeNRequested = xfadeNReq;
    out.xfadeN = xfadeN;
    out.nLoopSamples = nLoopSamples;
    out.notePeriod = notePeriod;

    specloop::CoreResult core;
    if (!specloop::synthesize(src, ls, nLoopSamples, nFft, specloop::Method::PhaseVocoder, core,
                              notePeriod))
        return false;
    out.seamError = core.seamError;

    const int nch = src.channels();
    out.ch.resize(nch);
    for (int c = 0; c < nch; ++c) {
        const auto& x = src.ch[c];
        // rotation search: the one that best aligns the loop with the real
        // material right before ls (circular correlation via FFT).
        std::vector<double> reference((size_t)xfadeN);
        for (int64_t i = 0; i < xfadeN; ++i)
            reference[(size_t)i] = (double)x[(size_t)(ls - xfadeN + i)];
        const int64_t shift = (xfadeN > 0)
                                  ? specloop::circularBestShift(core.loopBuf[c], reference)
                                  : 0;
        specloop::rotateAndBlend(core.loopBuf[c], reference, shift, xfadeN, xfadeCurve, out.ch[c]);
    }

    return true;
}

} // namespace harmonicdsp
