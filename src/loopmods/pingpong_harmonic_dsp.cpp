#include "pingpong_harmonic_dsp.hpp"

#include <algorithm>
#include <cmath>

#include "../fft.hpp"
#include "loop_module.hpp" // xfadeGains

namespace pingpongdsp {

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

// Number of STFT frames (including the context one) for a segment of length
// segLen, same as stft() in freeze_loop.py/mirror_loop.py.
int64_t frameCount(int64_t segLen, int nFft, int hop) {
    if (segLen < nFft) return 0;
    return 1 + (segLen - nFft) / hop;
}

// mag/omega_true/phase_raw per REAL frame (STFT frame 0 is only phase
// context for the first one, see estimate_frame_frequencies in
// mirror_loop.py). false if the region doesn't reach at least 3 STFT frames.
struct AnalysisFrames {
    int nBins = 0;
    std::vector<std::vector<double>> mag;       // [nFrames][nBins]
    std::vector<std::vector<double>> omegaTrue;  // [nFrames][nBins]
    std::vector<std::vector<double>> phaseRaw;   // [nFrames][nBins]
};

bool analyzeChannel(const std::vector<float>& x, int64_t ls, int64_t le, int nFft, int hop,
                    const std::vector<double>& win, AnalysisFrames& out) {
    const int64_t segStart = ls - hop;
    const int64_t segLen = le - segStart;
    const int64_t M0 = frameCount(segLen, nFft, hop);
    if (M0 < 3) return false;
    const int nBins = nFft / 2 + 1;

    std::vector<std::vector<double>> mag0((size_t)M0), phase0((size_t)M0);
    for (int64_t m = 0; m < M0; ++m) {
        std::vector<double> windowed((size_t)nFft);
        const int64_t base = segStart + m * hop;
        for (int i = 0; i < nFft; ++i) windowed[(size_t)i] = (double)x[(size_t)(base + i)] * win[(size_t)i];
        const auto X = dsp::rfft(windowed);
        mag0[(size_t)m].resize(nBins);
        phase0[(size_t)m].resize(nBins);
        for (int k = 0; k < nBins; ++k) {
            mag0[(size_t)m][(size_t)k] = std::abs(X[(size_t)k]);
            phase0[(size_t)m][(size_t)k] = std::arg(X[(size_t)k]);
        }
    }

    const int64_t nFrames = M0 - 1;
    out.nBins = nBins;
    out.mag.resize((size_t)nFrames);
    out.omegaTrue.resize((size_t)nFrames);
    out.phaseRaw.resize((size_t)nFrames);
    for (int64_t m = 1; m < M0; ++m) {
        out.mag[(size_t)(m - 1)] = mag0[(size_t)m];
        out.phaseRaw[(size_t)(m - 1)] = phase0[(size_t)m];
        auto& om = out.omegaTrue[(size_t)(m - 1)];
        om.resize(nBins);
        for (int k = 0; k < nBins; ++k) {
            const double omegaNominal = 2.0 * M_PI * k / nFft;
            const double expected = omegaNominal * hop;
            const double dev = princarg(phase0[(size_t)m][(size_t)k] - phase0[(size_t)(m - 1)][(size_t)k] - expected);
            om[(size_t)k] = omegaNominal + dev / hop;
        }
    }
    return true;
}

// Port of find_spectral_peaks (mirror_loop.py): local peaks over the
// magnitude averaged across frames, with a minimum distance between peaks
// and a minimum height relative to the tallest peak (same defaults as the
// script: distance=3, height_ratio=0.02). If no bin clears the threshold,
// every bin is its own "peak" -- no phase-locking at all, same fallback the
// script uses for unusual or very quiet input.
void findSpectralPeaks(const std::vector<double>& avgMag, int minBinDistance, double heightRatio,
                       std::vector<int>& peakBins, std::vector<int>& regionOf) {
    const int nBins = (int)avgMag.size();
    double mx = 0.0;
    for (double v : avgMag) mx = std::max(mx, v);

    std::vector<int> candidates;
    for (int i = 1; i < nBins - 1; ++i)
        if (avgMag[(size_t)i] > avgMag[(size_t)(i - 1)] && avgMag[(size_t)i] > avgMag[(size_t)(i + 1)] &&
            avgMag[(size_t)i] >= mx * heightRatio)
            candidates.push_back(i);
    std::sort(candidates.begin(), candidates.end(),
             [&](int a, int b) { return avgMag[(size_t)a] > avgMag[(size_t)b]; });

    std::vector<int> accepted;
    for (int c : candidates) {
        bool ok = true;
        for (int p : accepted)
            if (std::abs(p - c) < minBinDistance) { ok = false; break; }
        if (ok) accepted.push_back(c);
    }

    if (accepted.empty()) {
        peakBins.resize((size_t)nBins);
        regionOf.resize((size_t)nBins);
        for (int i = 0; i < nBins; ++i) { peakBins[(size_t)i] = i; regionOf[(size_t)i] = i; }
        return;
    }
    std::sort(accepted.begin(), accepted.end());
    peakBins = accepted;
    regionOf.resize((size_t)nBins);
    for (int b = 0; b < nBins; ++b) {
        int best = 0, bestDist = std::abs(b - peakBins[0]);
        for (int p = 1; p < (int)peakBins.size(); ++p) {
            const int d = std::abs(b - peakBins[(size_t)p]);
            if (d < bestDist) { bestDist = d; best = p; }
        }
        regionOf[(size_t)b] = best;
    }
}

// [0, 1, ..., n-1, n-2, ..., 1] -- forward then backward, frame 0 and frame
// n-1 each appear once per lap (port of mirror_frame_sequence).
std::vector<int64_t> mirrorSequence(int64_t nFrames) {
    std::vector<int64_t> seq;
    seq.reserve((size_t)(2 * nFrames - 2));
    for (int64_t i = 0; i < nFrames; ++i) seq.push_back(i);
    for (int64_t i = nFrames - 2; i >= 1; --i) seq.push_back(i);
    return seq;
}

// Port of synthesize_frame_sequence: renders 'lapsToRender' full laps of
// 'sequence'. mag follows the sequence as-is; phase is identity-locked
// (peakBins accumulates freely via omegaTrue, already quantized to close
// exactly per lap; the rest follows its nearest peak plus its own original
// phase offset in the frame currently being played).
std::vector<float> synthesizeBody(const AnalysisFrames& af, const std::vector<int>& peakBins,
                                  const std::vector<int>& regionOf, const std::vector<int64_t>& sequence,
                                  int64_t lapsToRender, int nFft, int hop, const std::vector<double>& win) {
    const int64_t L = (int64_t)sequence.size();
    const int64_t totalSteps = L * lapsToRender;
    const int64_t padFrames = (int64_t)std::ceil((double)nFft / hop) + 1;
    const int64_t totalLen = totalSteps + 2 * padFrames;
    const int64_t i0 = padFrames;
    const int nBins = af.nBins;
    const int nPeaks = (int)peakBins.size();

    std::vector<int64_t> fullIdx((size_t)totalLen);
    for (int64_t s = 0; s < totalLen; ++s) {
        const int64_t step = s - padFrames;
        const int64_t m = ((step % L) + L) % L;
        fullIdx[(size_t)s] = sequence[(size_t)m];
    }

    // Quantization: each PEAK bin's real frequency is rescaled so its total
    // phase advance over one lap is an exact multiple of 2*pi (same trick
    // as Harmonic, generalized from "one analysis frame" to "one lap").
    const int64_t nFrames = (int64_t)af.mag.size();
    std::vector<std::vector<double>> omegaPeaks((size_t)nFrames, std::vector<double>((size_t)nPeaks));
    for (int64_t m = 0; m < nFrames; ++m)
        for (int p = 0; p < nPeaks; ++p)
            omegaPeaks[(size_t)m][(size_t)p] = af.omegaTrue[(size_t)m][(size_t)peakBins[(size_t)p]];

    std::vector<double> naiveLapAdvance((size_t)nPeaks, 0.0);
    for (int64_t s = 0; s < L; ++s) {
        const int64_t m = sequence[(size_t)s];
        for (int p = 0; p < nPeaks; ++p) naiveLapAdvance[(size_t)p] += omegaPeaks[(size_t)m][(size_t)p] * hop;
    }
    std::vector<double> ratio((size_t)nPeaks, 1.0);
    for (int p = 0; p < nPeaks; ++p) {
        if (std::fabs(naiveLapAdvance[(size_t)p]) >= 1e-9) {
            const double nCycles = std::round(naiveLapAdvance[(size_t)p] / (2.0 * M_PI));
            ratio[(size_t)p] = (nCycles * 2.0 * M_PI) / naiveLapAdvance[(size_t)p];
        }
    }
    for (int64_t m = 0; m < nFrames; ++m)
        for (int p = 0; p < nPeaks; ++p) omegaPeaks[(size_t)m][(size_t)p] *= ratio[(size_t)p];

    std::vector<double> phasePeak0((size_t)nPeaks);
    for (int p = 0; p < nPeaks; ++p) phasePeak0[(size_t)p] = af.phaseRaw[0][(size_t)peakBins[(size_t)p]];

    std::vector<std::vector<double>> cumsumPeak((size_t)totalLen, std::vector<double>((size_t)nPeaks));
    {
        std::vector<double> acc((size_t)nPeaks, 0.0);
        for (int64_t s = 0; s < totalLen; ++s) {
            const int64_t m = fullIdx[(size_t)s];
            for (int p = 0; p < nPeaks; ++p) {
                acc[(size_t)p] += omegaPeaks[(size_t)m][(size_t)p] * hop;
                cumsumPeak[(size_t)s][(size_t)p] = acc[(size_t)p];
            }
        }
    }
    const std::vector<double> cumsumAtI0 = cumsumPeak[(size_t)i0];

    std::vector<int> peakBinForBin((size_t)nBins);
    for (int b = 0; b < nBins; ++b) peakBinForBin[(size_t)b] = peakBins[(size_t)regionOf[(size_t)b]];

    const int64_t outLen = (totalLen - 1) * hop + nFft;
    std::vector<double> yFull((size_t)outLen, 0.0), normFull((size_t)outLen, 0.0);
    std::vector<double> win2((size_t)nFft);
    for (int i = 0; i < nFft; ++i) win2[(size_t)i] = win[(size_t)i] * win[(size_t)i];

    std::vector<dsp::Complex> Xf((size_t)nBins);
    for (int64_t s = 0; s < totalLen; ++s) {
        const int64_t m = fullIdx[(size_t)s];
        for (int b = 0; b < nBins; ++b) {
            const int p = regionOf[(size_t)b];
            const double phasePeak = phasePeak0[(size_t)p] + (cumsumPeak[(size_t)s][(size_t)p] - cumsumAtI0[(size_t)p]);
            const double offset = af.phaseRaw[(size_t)m][(size_t)b] - af.phaseRaw[(size_t)m][(size_t)peakBinForBin[(size_t)b]];
            Xf[(size_t)b] = std::polar(af.mag[(size_t)m][(size_t)b], phasePeak + offset);
        }
        const std::vector<double> frame = dsp::irfft(Xf, (size_t)nFft);
        const int64_t base = s * hop;
        for (int i = 0; i < nFft; ++i) {
            yFull[(size_t)(base + i)] += frame[(size_t)i] * win[(size_t)i];
            normFull[(size_t)(base + i)] += win2[(size_t)i];
        }
    }
    for (auto& v : normFull)
        if (v < 1e-8) v = 1e-8;

    const int64_t start = padFrames * hop;
    std::vector<float> body((size_t)(totalSteps * hop));
    for (int64_t i = 0; i < totalSteps * hop; ++i)
        body[(size_t)i] = (float)(yFull[(size_t)(start + i)] / normFull[(size_t)(start + i)]);
    return body;
}

} // namespace

void sizes(int64_t ls, int64_t le, double sampleRate, int nFft, float xfadeMs,
          int64_t& xfadeNReq, int64_t& xfadeN, int64_t& oneLapSamples) {
    const int hop = nFft / 4;
    const int64_t segLen = (le - ls) + hop;
    const int64_t M0 = frameCount(segLen, nFft, hop);
    const int64_t nFrames = std::max<int64_t>(0, M0 - 1);
    const int64_t L = (nFrames >= 2) ? (2 * nFrames - 2) : 0;
    oneLapSamples = L * hop;
    xfadeNReq = (int64_t)std::llround(sampleRate * xfadeMs / 1000.0);
    xfadeN = std::max<int64_t>(0, std::min<int64_t>({xfadeNReq, ls, oneLapSamples}));
}

bool buildLoop(const AudioData& src, int64_t ls, int64_t le, int nFft, int laps,
              float xfadeMs, float xfadeCurve, Result& out) {
    if (le <= ls || laps < 1) return false;
    const int hop = nFft / 4;
    if (ls < hop) return false; // needs 1 hop of phase context before ls

    int64_t xfadeNReq = 0, xfadeN = 0, oneLapEstimate = 0;
    sizes(ls, le, src.sampleRate, nFft, xfadeMs, xfadeNReq, xfadeN, oneLapEstimate);
    if (oneLapEstimate <= 0) return false;

    out.xfadeNRequested = xfadeNReq;
    out.xfadeN = xfadeN;
    out.oneLapSamples = 0; // set after analyzing the first channel
    out.seamError = 0.0;

    const std::vector<double> win = hannPeriodic(nFft);
    // One extra lap beyond 'laps': gives room to discard the first xfadeN
    // samples (absorbed by attackTail, xfadeN <= oneLapSamples by
    // construction of sizes()) and to measure seamError by comparing two
    // full laps.
    const int64_t lapsToRender = (int64_t)laps + 1;

    const int nch = src.channels();
    out.ch.resize((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        AnalysisFrames af;
        if (!analyzeChannel(src.ch[c], ls, le, nFft, hop, win, af)) return false;

        const int64_t nFrames = (int64_t)af.mag.size();
        std::vector<double> avgMag((size_t)af.nBins, 0.0);
        for (int64_t m = 0; m < nFrames; ++m)
            for (int k = 0; k < af.nBins; ++k) avgMag[(size_t)k] += af.mag[(size_t)m][(size_t)k];
        for (double& v : avgMag) v /= (double)nFrames;

        std::vector<int> peakBins, regionOf;
        findSpectralPeaks(avgMag, /*minBinDistance=*/3, /*heightRatio=*/0.02, peakBins, regionOf);

        const auto sequence = mirrorSequence(nFrames);
        const int64_t L = (int64_t)sequence.size();
        const int64_t oneLap = L * hop;
        if (out.oneLapSamples == 0) out.oneLapSamples = oneLap;

        const auto bodyFull = synthesizeBody(af, peakBins, regionOf, sequence, lapsToRender, nFft, hop, win);

        double se = 0.0;
        for (int64_t i = 0; i < oneLap; ++i)
            se = std::max(se, std::fabs((double)bodyFull[(size_t)i] - (double)bodyFull[(size_t)(oneLap + i)]));
        out.seamError = std::max(out.seamError, se);

        Result::Channel rc;
        rc.attackTail.resize((size_t)xfadeN);
        const auto& x = src.ch[c];
        for (int64_t j = 0; j < xfadeN; ++j) {
            const float t = (xfadeN > 1) ? (float)j / (float)(xfadeN - 1) : 0.f;
            float gIn, gOut;
            xfadeGains(t, xfadeCurve, gIn, gOut);
            const float ref = x[(size_t)(ls - xfadeN + j)];
            rc.attackTail[(size_t)j] = ref * gOut + bodyFull[(size_t)j] * gIn;
        }
        rc.body.assign(bodyFull.begin() + xfadeN, bodyFull.begin() + xfadeN + laps * oneLap);
        out.ch[(size_t)c] = std::move(rc);
    }

    return true;
}

} // namespace pingpongdsp
