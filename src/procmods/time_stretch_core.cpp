#include "time_stretch_core.hpp"

#include <algorithm>
#include <cmath>

#include "../fft.hpp"

namespace stretchcore {

namespace {

double princarg(double phase) {
    const double twoPi = 2.0 * M_PI;
    double x = std::fmod(phase + M_PI, twoPi);
    if (x < 0.0) x += twoPi;
    return x - M_PI;
}

std::vector<double> hannPeriodic(int n) {
    std::vector<double> w((size_t)n);
    for (int i = 0; i < n; ++i) w[(size_t)i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / n);
    return w;
}

} // namespace

bool timeStretch(const std::vector<float>& in, int64_t len, int nFft, double stretch,
                 std::vector<double>& out, int64_t& realStart, int64_t& realLen) {
    const int hop = nFft / 4; // fixed for both analysis and synthesis
    const int64_t padFrames = (int64_t)std::ceil((double)nFft / hop) + 1;
    const int64_t pad = padFrames * hop; // exact multiple of hop
    const int64_t inLen = 2 * pad + len;

    std::vector<double> buf((size_t)inLen, 0.0);
    for (int64_t i = 0; i < len; ++i) buf[(size_t)(pad + i)] = (double)in[(size_t)i];

    const int64_t nFrames = 1 + (inLen - nFft) / hop;
    if (nFrames < 2) return false;

    const int nBins = nFft / 2 + 1;
    const std::vector<double> win = hannPeriodic(nFft);
    std::vector<double> win2((size_t)nFft);
    for (int i = 0; i < nFft; ++i) win2[(size_t)i] = win[(size_t)i] * win[(size_t)i];

    // Analysis: magnitude + raw phase per frame (frame m at input position m*hop).
    std::vector<std::vector<double>> mag((size_t)nFrames), phase((size_t)nFrames);
    for (int64_t m = 0; m < nFrames; ++m) {
        std::vector<double> windowed((size_t)nFft);
        const int64_t base = m * hop;
        for (int i = 0; i < nFft; ++i) windowed[(size_t)i] = buf[(size_t)(base + i)] * win[(size_t)i];
        const auto X = dsp::rfft(windowed);
        mag[(size_t)m].resize((size_t)nBins);
        phase[(size_t)m].resize((size_t)nBins);
        for (int k = 0; k < nBins; ++k) {
            mag[(size_t)m][(size_t)k] = std::abs(X[(size_t)k]);
            phase[(size_t)m][(size_t)k] = std::arg(X[(size_t)k]);
        }
    }

    // True instantaneous frequency per bin per frame, from the phase
    // difference between consecutive analysis frames -- same derivation
    // used across this project's spectral modules. Frame 0 has no
    // predecessor, so it just gets the bin's nominal frequency; harmless,
    // it sits entirely in the zero-padded lead-in where magnitude is ~0.
    std::vector<std::vector<double>> trueOmega((size_t)nFrames, std::vector<double>((size_t)nBins));
    for (int k = 0; k < nBins; ++k) trueOmega[0][(size_t)k] = 2.0 * M_PI * k / nFft;
    for (int64_t m = 1; m < nFrames; ++m) {
        for (int k = 0; k < nBins; ++k) {
            const double omegaNominal = 2.0 * M_PI * k / nFft;
            const double expected = omegaNominal * hop;
            const double dev = princarg(phase[(size_t)m][(size_t)k] -
                                        phase[(size_t)(m - 1)][(size_t)k] - expected);
            trueOmega[(size_t)m][(size_t)k] = omegaNominal + dev / hop;
        }
    }

    // Synthesis: OLA at the fixed hop. Step j reads (linearly interpolated)
    // magnitude and true frequency from fractional analysis position
    // j/stretch, and accumulates phase using that interpolated frequency
    // over one hop's worth of output time -- this is what actually
    // stretches or compresses the content while the OLA grid itself never
    // changes.
    const int64_t nSyn = std::max<int64_t>(2, (int64_t)std::ceil((double)(nFrames - 1) * stretch) + 1);
    const int64_t outLen = (nSyn - 1) * hop + nFft;
    std::vector<double> yFull((size_t)outLen, 0.0), normFull((size_t)outLen, 0.0);
    std::vector<double> synthPhase((size_t)nBins);
    for (int k = 0; k < nBins; ++k) synthPhase[(size_t)k] = phase[0][(size_t)k];

    std::vector<dsp::Complex> Xf((size_t)nBins);
    for (int64_t j = 0; j < nSyn; ++j) {
        const double srcPos = (double)j / stretch;
        int64_t m0 = std::clamp<int64_t>((int64_t)std::floor(srcPos), 0, nFrames - 1);
        const int64_t m1 = std::min<int64_t>(m0 + 1, nFrames - 1);
        const double frac = std::clamp(srcPos - (double)m0, 0.0, 1.0);

        if (j > 0) {
            for (int k = 0; k < nBins; ++k) {
                const double omegaI = trueOmega[(size_t)m0][(size_t)k] * (1.0 - frac) +
                                      trueOmega[(size_t)m1][(size_t)k] * frac;
                synthPhase[(size_t)k] += omegaI * hop;
            }
        }
        for (int k = 0; k < nBins; ++k) {
            const double magI = mag[(size_t)m0][(size_t)k] * (1.0 - frac) +
                                mag[(size_t)m1][(size_t)k] * frac;
            Xf[(size_t)k] = std::polar(magI, synthPhase[(size_t)k]);
        }
        const std::vector<double> frame = dsp::irfft(Xf, (size_t)nFft);
        const int64_t base = j * hop;
        for (int i = 0; i < nFft; ++i) {
            yFull[(size_t)(base + i)] += frame[(size_t)i] * win[(size_t)i];
            normFull[(size_t)(base + i)] += win2[(size_t)i];
        }
    }
    for (int64_t i = 0; i < outLen; ++i) {
        if (normFull[(size_t)i] < 1e-8) normFull[(size_t)i] = 1e-8;
        yFull[(size_t)i] /= normFull[(size_t)i];
    }

    out = std::move(yFull);
    // padFrames analysis frames of lead-in correspond to synthesis step
    // padFrames*stretch (fractional in general): round to the nearest
    // synthesis hop.
    const int64_t realStartFrame =
        std::clamp<int64_t>((int64_t)std::llround((double)padFrames * stretch), 0, nSyn - 1);
    realStart = realStartFrame * hop;
    const int64_t stretchedLen = (int64_t)std::llround((double)len * stretch);
    realLen = std::max<int64_t>(0, std::min(outLen - realStart, stretchedLen));
    return true;
}

} // namespace stretchcore
