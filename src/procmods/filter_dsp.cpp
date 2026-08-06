#include "filter_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

#include "../fft.hpp"

namespace filterdsp {

namespace {

// Nyquist with margin: a w0 too close to pi makes the cookbook formulas
// unstable (alpha/denominators near zero).
double safeFreq(double freq, double sr) {
    return std::min(freq, sr * 0.49);
}

double biquadGain(const Biquad& bq, double w) {
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = std::polar(1.0, -2.0 * w);
    const std::complex<double> num = bq.b0 + bq.b1 * z1 + bq.b2 * z2;
    const std::complex<double> den = 1.0 + bq.a1 * z1 + bq.a2 * z2;
    return std::abs(num / den);
}

std::vector<double> hannPeriodic(int n) {
    std::vector<double> w((size_t)n);
    for (int i = 0; i < n; ++i) w[(size_t)i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / n);
    return w;
}

} // namespace

Biquad makeHighpass(double freq, double sr, double q) {
    Biquad bq;
    const double w0 = 2.0 * M_PI * safeFreq(freq, sr) / sr;
    const double cosw0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    bq.b0 = ((1.0 + cosw0) / 2.0) / a0;
    bq.b1 = (-(1.0 + cosw0)) / a0;
    bq.b2 = ((1.0 + cosw0) / 2.0) / a0;
    bq.a1 = (-2.0 * cosw0) / a0;
    bq.a2 = (1.0 - alpha) / a0;
    return bq;
}

Biquad makePeaking(double freq, double sr, double q, double gainDb) {
    Biquad bq;
    const double w0 = 2.0 * M_PI * safeFreq(freq, sr) / sr;
    const double cosw0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double a0 = 1.0 + alpha / A;
    bq.b0 = (1.0 + alpha * A) / a0;
    bq.b1 = (-2.0 * cosw0) / a0;
    bq.b2 = (1.0 - alpha * A) / a0;
    bq.a1 = (-2.0 * cosw0) / a0;
    bq.a2 = (1.0 - alpha / A) / a0;
    return bq;
}

namespace {
// Chain of biquads (stateless, for responseAt) or with fresh state (for
// apply, one per channel). buildChain() assembles the coefficient list from
// the spec; apply() instantiates its OWN copy per channel so z1/z2 aren't
// shared between channels.
std::vector<Biquad> buildChain(const FilterSpec& spec, double sr) {
    std::vector<Biquad> chain;
    // User Q only on the first stage; the rest use Butterworth (see the
    // comment in FilterSpec).
    if (spec.hpOn)
        for (int i = 0; i < spec.hpStages; ++i)
            chain.push_back(makeHighpass(spec.hpFreq, sr, i == 0 ? spec.hpQ : kButterworthQ));
    if (spec.bandOn)
        chain.push_back(makePeaking(spec.bandFreq, sr, spec.bandQ, spec.bandGainDb));
    return chain;
}
} // namespace

double responseAt(const FilterSpec& spec, double freq, double sr) {
    const auto chain = buildChain(spec, sr);
    if (chain.empty()) return 1.0;
    const double w = 2.0 * M_PI * freq / sr;
    double gain = 1.0;
    for (auto& bq : chain) gain *= biquadGain(bq, w);
    return gain;
}

std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      const FilterSpec& spec) {
    std::vector<std::vector<float>> out;
    const int64_t regionLen = s1 - s0;
    const int nch = src.channels();
    if (regionLen <= 0 || nch == 0) return out;

    const double sr = src.sampleRate;
    // Up to 200ms of real audio before s0 to settle the biquad's state
    // (plenty: at 20Hz, this module's worst case, a full period is 50ms --
    // the extra margin covers long high/low-pass cascades, up to kMaxStages
    // stages in series). If there isn't that much audio before s0, it
    // settles with whatever is available -- a shorter settling period at the
    // document's actual edge is acceptable, same criterion as the rest of
    // the modules with limited context.
    const int64_t preRoll = std::min<int64_t>(s0, (int64_t)std::llround(sr * 0.2));

    out.resize((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        auto chain = buildChain(spec, sr); // fresh copy -> zeroed state, own to this channel
        const auto& x = src.ch[(size_t)c];

        for (int64_t i = s0 - preRoll; i < s0; ++i) {
            float s = x[(size_t)i];
            for (auto& bq : chain) s = bq.process(s);
        }

        std::vector<float> chOut((size_t)regionLen);
        for (int64_t i = 0; i < regionLen; ++i) {
            float s = x[(size_t)(s0 + i)];
            for (auto& bq : chain) s = bq.process(s);
            chOut[(size_t)i] = s;
        }
        out[(size_t)c] = std::move(chOut);
    }
    return out;
}

void analyzeStaticSpectrum(const AudioData& src, int64_t s0, int64_t s1, int nFftIn,
                           int64_t maxFrames, SpectrumCurve& out) {
    out.db.clear();
    if (s1 <= s0) return;
    const int64_t len = s1 - s0;
    if (len < 64) return;
    int nFft = nFftIn;
    while (nFft > 64 && (int64_t)nFft > len) nFft /= 2;
    const int hop = nFft / 2;
    const int nBins = nFft / 2 + 1;
    const int64_t nFrames = std::max<int64_t>(1, 1 + (len - nFft) / hop);
    const int64_t stepFrames = std::max<int64_t>(1, nFrames / std::max<int64_t>(1, maxFrames));

    const std::vector<double> win = hannPeriodic(nFft);
    const int nch = src.channels();

    std::vector<double> acc((size_t)nBins, 0.0);
    std::vector<double> buf((size_t)nFft);
    int64_t used = 0;
    for (int64_t fi = 0; fi < nFrames; fi += stepFrames) {
        const int64_t base = s0 + fi * hop;
        if (base + nFft > s1) break;
        for (int i = 0; i < nFft; ++i) {
            double s = 0.0;
            for (int c = 0; c < nch; ++c) s += src.ch[(size_t)c][(size_t)(base + i)];
            buf[(size_t)i] = (s / nch) * win[(size_t)i];
        }
        const auto X = dsp::rfft(buf);
        for (int k = 0; k < nBins; ++k) acc[(size_t)k] += std::abs(X[(size_t)k]);
        ++used;
    }
    if (used == 0) return;

    // Same calibration as EditorUI::updateLiveSpectrum: with a Hann window
    // (coherent gain 0.5), the peak bin of a full-scale sinusoid lands
    // around nFft/4 -- so 0 dB ends up close to "full scale" (an absolute
    // level), not "the loudest bin in this view".
    const double ref = nFft / 4.0;
    out.db.resize((size_t)nBins);
    for (int k = 0; k < nBins; ++k)
        out.db[(size_t)k] = (float)(20.0 * std::log10(std::max(1e-9, (acc[(size_t)k] / used) / ref)));
    out.nFft = nFft;
    out.sr = src.sampleRate;
}

} // namespace filterdsp
