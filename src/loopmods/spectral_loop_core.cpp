#include "spectral_loop_core.hpp"

#include <algorithm>
#include <cmath>

#include "../fft.hpp"
#include "loop_module.hpp" // xfadeGains

namespace {

double princarg(double phase) {
    const double twoPi = 2.0 * M_PI;
    double x = std::fmod(phase + M_PI, twoPi);
    if (x < 0.0) x += twoPi;
    return x - M_PI;
}

std::vector<double> hannPeriodic(int n) {
    std::vector<double> w(n);
    for (int i = 0; i < n; ++i) w[i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / n);
    return w;
}

} // namespace

namespace specloop {

bool synthesize(const AudioData& src, int64_t anchorSample, int64_t nLoopSamples,
                int nFft, Method method, CoreResult& out, int64_t lockPeriod) {
    const int hop = nFft / 4;
    const int64_t xlen = src.frames();
    if (xlen < nFft) return false;
    const int64_t nFrames = 1 + (xlen - nFft) / hop;
    if (nFrames < 2) return false;

    int64_t mf = (int64_t)std::llround((double)anchorSample / hop);
    mf = std::clamp<int64_t>(mf, 1, nFrames - 1);

    const std::vector<double> win = hannPeriodic(nFft);
    std::vector<double> win2(nFft);
    for (int i = 0; i < nFft; ++i) win2[i] = win[i] * win[i];

    const int64_t nLoopFrames = nLoopSamples / hop;
    const int64_t padFrames = (int64_t)std::ceil((double)nFft / hop) + 1;
    const int64_t frameLo = -padFrames;
    const int64_t totalFrames = 2 * nLoopFrames + 2 * padFrames;
    const int64_t outLen = (totalFrames - 1) * hop + nFft;
    const int64_t start = padFrames * hop;
    const int nBins = nFft / 2 + 1;

    const int nch = src.channels();
    out.loopBuf.resize(nch);
    double seamErr = 0.0;

    for (int c = 0; c < nch; ++c) {
        const auto& x = src.ch[c];

        auto frameAt = [&](int64_t frameIdx) {
            std::vector<double> windowed(nFft);
            const int64_t base = frameIdx * hop;
            for (int i = 0; i < nFft; ++i) windowed[i] = (double)x[(size_t)(base + i)] * win[i];
            return dsp::rfft(windowed);
        };
        const auto Xnow = frameAt(mf);
        // Naive doesn't need the previous frame (it doesn't estimate the
        // real frequency, see Method): avoids the extra FFT.
        const auto Xprev = (method == Method::PhaseVocoder) ? frameAt(mf - 1)
                                                             : std::vector<dsp::Complex>();

        std::vector<double> mag(nBins), phaseNow(nBins), quantOmega(nBins);
        for (int k = 0; k < nBins; ++k) {
            mag[k] = std::abs(Xnow[k]);
            phaseNow[k] = std::arg(Xnow[k]);
            double trueOmega = 0.0;
            if (method == Method::PhaseVocoder) {
                const double phasePrev = std::arg(Xprev[k]);
                const double omegaNominal = 2.0 * M_PI * k / nFft;
                const double expected = omegaNominal * hop;
                const double dev = princarg(phaseNow[k] - phasePrev - expected);
                trueOmega = omegaNominal + dev / hop;
            }
            // method == Naive: trueOmega stays at 0 for every bin -> so does
            // quantOmega -> the analysis frame repeats as-is, with no phase
            // advance (see the comment in the .hpp).
            if (lockPeriod > 0) {
                // Exact harmonic series of a lockPeriod-sample period (see
                // .hpp); the harmonic is capped at the period's Nyquist to
                // avoid aliasing (with an odd lockPeriod, rounding could
                // overshoot pi).
                double harm = std::round(trueOmega * (double)lockPeriod / (2.0 * M_PI));
                harm = std::min(harm, std::floor((double)lockPeriod / 2.0));
                quantOmega[k] = 2.0 * M_PI * harm / (double)lockPeriod;
            } else {
                const double nCycles = std::round(trueOmega * (double)nLoopSamples / (2.0 * M_PI));
                quantOmega[k] = 2.0 * M_PI * nCycles / (double)nLoopSamples;
            }
        }
        quantOmega[0] = 0.0;
        quantOmega[nBins - 1] = 0.0;

        std::vector<double> yFull(outLen, 0.0), normFull(outLen, 0.0);
        std::vector<dsp::Complex> Xf(nBins);
        for (int64_t fi = 0; fi < totalFrames; ++fi) {
            const int64_t frameIdx = frameLo + fi;
            for (int k = 0; k < nBins; ++k) {
                const double phase = phaseNow[k] + (double)frameIdx * (quantOmega[k] * hop);
                Xf[k] = std::polar(mag[k], phase);
            }
            const std::vector<double> frame = dsp::irfft(Xf, (size_t)nFft);
            const int64_t base = fi * hop;
            for (int i = 0; i < nFft; ++i) {
                yFull[(size_t)(base + i)] += frame[(size_t)i] * win[i];
                normFull[(size_t)(base + i)] += win2[i];
            }
        }
        for (auto& v : normFull)
            if (v < 1e-8) v = 1e-8;

        std::vector<double> loopBuf(nLoopSamples);
        for (int64_t i = 0; i < nLoopSamples; ++i)
            loopBuf[(size_t)i] = yFull[(size_t)(start + i)] / normFull[(size_t)(start + i)];
        for (int64_t i = 0; i < nLoopSamples; ++i) {
            const int64_t j = start + nLoopSamples + i;
            const double next = yFull[(size_t)j] / normFull[(size_t)j];
            seamErr = std::max(seamErr, std::fabs(loopBuf[(size_t)i] - next));
        }

        out.loopBuf[c] = std::move(loopBuf);
    }

    out.seamError = seamErr;
    return true;
}

int64_t circularBestShift(const std::vector<double>& loopBuf,
                          const std::vector<double>& reference) {
    const int64_t nLoopSamples = (int64_t)loopBuf.size();
    const int64_t xfadeN = (int64_t)reference.size();
    if (xfadeN <= 0 || nLoopSamples <= 0) return 0;

    std::vector<dsp::Complex> a(loopBuf.begin(), loopBuf.end());
    std::vector<dsp::Complex> b((size_t)nLoopSamples, dsp::Complex(0.0, 0.0));
    for (int64_t i = 0; i < xfadeN; ++i) b[(size_t)i] = reference[(size_t)i];
    dsp::dft(a, false);
    dsp::dft(b, false);
    std::vector<dsp::Complex> corr((size_t)nLoopSamples);
    for (int64_t i = 0; i < nLoopSamples; ++i)
        corr[(size_t)i] = a[(size_t)i] * std::conj(b[(size_t)i]);
    dsp::dft(corr, true);

    int64_t shift = 0;
    double best = corr[0].real();
    for (int64_t i = 1; i < nLoopSamples; ++i) {
        const double v = corr[(size_t)i].real();
        if (v > best) { best = v; shift = i; }
    }
    return shift;
}

void rotateAndBlend(const std::vector<double>& loopBuf, const std::vector<double>& reference,
                    int64_t shift, int64_t xfadeN, float xfadeCurve, RotatedBlend& out) {
    const int64_t nLoopSamples = (int64_t)loopBuf.size();

    std::vector<double> lb(nLoopSamples);
    for (int64_t i = 0; i < nLoopSamples; ++i)
        lb[(size_t)i] = loopBuf[(size_t)(((i + shift) % nLoopSamples + nLoopSamples) % nLoopSamples)];

    out.attackTail.resize((size_t)xfadeN);
    for (int64_t j = 0; j < xfadeN; ++j) {
        const float t = (xfadeN > 1) ? (float)j / (float)(xfadeN - 1) : 0.f;
        float gIn, gOut;
        xfadeGains(t, xfadeCurve, gIn, gOut);
        out.attackTail[(size_t)j] = (float)reference[(size_t)j] * gOut + (float)lb[(size_t)j] * gIn;
    }
    out.loopRegion.resize((size_t)nLoopSamples);
    for (int64_t i = 0; i < nLoopSamples; ++i)
        out.loopRegion[(size_t)i] = (float)lb[(size_t)((i + xfadeN) % nLoopSamples)];
}

} // namespace specloop
