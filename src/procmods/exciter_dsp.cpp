// M/S Exciter port -- original code by Kevin Kopczynski, MIT
// (https://github.com/KevinToodlepoot/MS-Exciter, Source/PluginProcessor.cpp,
// audio/MS.h, config.h). JUCE's pieces it relied on are re-implemented here:
// the RBJ high-pass (filterdsp::makeHighpass, same formula as JUCE's
// IIR::Coefficients::makeHighPass), the delay lines, and the 2x half-band
// oversampling (a zero-phase windowed-sinc half-band, since this runs
// offline -- upstream's FIR oversampler adds latency to the exciter branch
// only).
#include "exciter_dsp.hpp"

#include <algorithm>
#include <cmath>

#include "filter_dsp.hpp"

namespace exciterdsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Range [c0, c1) processed (region + context before it), clamped.
struct Span {
    int64_t c0, s0, s1;
};

Span spanFor(const AudioData& src, int64_t s0, int64_t s1) {
    const int64_t ctx = (int64_t)(kContextSec * src.sampleRate);
    return {std::max<int64_t>(0, s0 - ctx), s0, s1};
}

// Crossfades the edges of the processed region with the untouched audio
// where it doesn't reach the start/end of the sample.
void fadeEdges(const AudioData& src, int64_t s0, int64_t s1, std::vector<std::vector<float>>& out) {
    const int64_t len = s1 - s0;
    const int64_t fadeN = std::min<int64_t>((int64_t)(kEdgeFadeSec * src.sampleRate), len / 2);
    if (fadeN <= 0) return;
    for (int c = 0; c < (int)out.size(); ++c) {
        const auto& dry = src.ch[(size_t)c];
        auto& o = out[(size_t)c];
        for (int64_t i = 0; i < fadeN; ++i) {
            const float t = (float)(i + 1) / (float)(fadeN + 1);
            if (s0 > 0) o[(size_t)i] = dry[(size_t)(s0 + i)] * (1.f - t) + o[(size_t)i] * t;
            if (s1 < src.frames()) {
                const int64_t k = len - 1 - i;
                o[(size_t)k] = dry[(size_t)(s0 + k)] * (1.f - t) + o[(size_t)k] * t;
            }
        }
    }
}

// ---- 2x half-band oversampling (zero phase) --------------------------------
// h[k], k = -M..M, at the 2x rate: 0.5 * sinc(k/2) * Kaiser. Every even
// k != 0 is zero (half-band), so only the odd taps are stored.
struct HalfBand {
    static constexpr int M = 31; // 63 taps at the 2x rate
    std::vector<double> odd;     // h[1], h[3], ..., h[M] (symmetric)
    HalfBand() {
        auto bessel0 = [](double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k) {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
            }
            return sum;
        };
        const double beta = 8.0;
        for (int k = 1; k <= M; k += 2) {
            const double r = (double)k / (M + 1);
            const double w = bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel0(beta);
            const double x = 0.5 * k;
            odd.push_back(0.5 * std::sin(kPi * x) / (kPi * x) * w);
        }
    }
    // n base samples -> 2n: even outputs are the input itself (h[0] = 0.5,
    // times the zero-stuffing gain of 2), odd ones interpolated.
    std::vector<float> up(const std::vector<float>& x) const {
        const int64_t n = (int64_t)x.size();
        std::vector<float> y((size_t)(2 * n));
        auto at = [&](int64_t i) { return (i >= 0 && i < n) ? (double)x[(size_t)i] : 0.0; };
        for (int64_t i = 0; i < n; ++i) {
            y[(size_t)(2 * i)] = x[(size_t)i];
            // y[2i+1] = 2 * sum_k h[k] * stuffed[2i+1-k]; nonzero for odd k
            // landing on even stuffed positions: x[i - (k-1)/2] and x[i + (k+1)/2]
            double acc = 0.0;
            for (size_t j = 0; j < odd.size(); ++j) {
                const int64_t k = 2 * (int64_t)j + 1;
                acc += odd[j] * (at(i - (k - 1) / 2) + at(i + (k + 1) / 2));
            }
            y[(size_t)(2 * i + 1)] = (float)(2.0 * acc);
        }
        return y;
    }
    // 2n -> n: low-pass at the 2x rate, keep the even samples.
    std::vector<float> down(const std::vector<float>& v) const {
        const int64_t n2 = (int64_t)v.size(), n = n2 / 2;
        std::vector<float> y((size_t)n);
        auto at = [&](int64_t i) { return (i >= 0 && i < n2) ? (double)v[(size_t)i] : 0.0; };
        for (int64_t i = 0; i < n; ++i) {
            double acc = 0.5 * at(2 * i);
            for (size_t j = 0; j < odd.size(); ++j) {
                const int64_t k = 2 * (int64_t)j + 1;
                acc += odd[j] * (at(2 * i - k) + at(2 * i + k));
            }
            y[(size_t)i] = (float)acc;
        }
        return y;
    }
};

float sigmoid(float x) { return 2.f / (1.f + std::exp(-x)) - 1.f; }
float arraya(float x) { return ((3.f * x) / 2.f) * (1.f - (x * x) / 3.f); }

} // namespace

std::vector<std::vector<float>> applyMS(const AudioData& src, int64_t s0, int64_t s1, const MSSpec& spec) {
    s0 = std::clamp<int64_t>(s0, 0, src.frames());
    s1 = std::clamp<int64_t>(s1, 0, src.frames());
    if (s1 <= s0 || src.channels() < 1) return {};
    const Span sp = spanFor(src, s0, s1);
    const int64_t n = s1 - sp.c0;
    const double sr = src.sampleRate;
    const bool stereo = src.channels() > 1;

    const float msMix = std::clamp(spec.msMixPct, 0.f, 100.f) / 100.f;
    const float excMix = std::clamp(spec.excMixPct, 0.f, 100.f) / 100.f;
    const float width = stereo ? std::clamp(spec.width, 0.f, 4.f) : 1.f;
    const float stereoize = stereo ? std::clamp(spec.stereoize, 0.f, 1.f) : 0.f;
    const float drive = std::clamp(spec.drive, 1.f, 20.f);
    const float bias = std::clamp(spec.bias, -1.f, 1.f);

    // Input, L/R -- a mono document runs as L = R.
    std::vector<float> in[2];
    for (int c = 0; c < 2; ++c) {
        const auto& x = src.ch[(size_t)std::min(c, src.channels() - 1)];
        in[c].resize((size_t)n);
        for (int64_t i = 0; i < n; ++i) in[c][(size_t)i] = x[(size_t)(sp.c0 + i)];
    }

    // ---- M/S (upstream MS::processSample) ----
    // ps = mid - 0.4 * mid[-2 ms] (comb), delayed 6 ms, added to the side.
    const int64_t combOrder = (int64_t)std::floor(2.0 * sr * 0.001);
    const int64_t psDelay = (int64_t)std::floor(6.0 * sr * 0.001);
    std::vector<float> ms[2] = {std::vector<float>((size_t)n), std::vector<float>((size_t)n)};
    std::vector<float> ps((size_t)n);
    for (int64_t i = 0; i < n; ++i) {
        const float mid = in[0][(size_t)i] + in[1][(size_t)i];
        const int64_t j = i - combOrder;
        const float midPast = j >= 0 ? in[0][(size_t)j] + in[1][(size_t)j] : 0.f;
        ps[(size_t)i] = mid - 0.4f * midPast;
        const int64_t k = i - psDelay;
        float side = in[0][(size_t)i] - in[1][(size_t)i];
        side += (k >= 0 ? ps[(size_t)k] : 0.f) * stereoize;
        side *= width;
        const float l = 0.5f * (mid + side), r = 0.5f * (mid - side);
        ms[0][(size_t)i] = l * msMix + in[0][(size_t)i] * (1.f - msMix);
        ms[1][(size_t)i] = r * msMix + in[1][(size_t)i] * (1.f - msMix);
    }

    // ---- exciter branch ----
    static const HalfBand hb;
    const float comp = 1.f / (2.f / (1.f + std::exp(-drive)) - 1.f); // 1 / sig(1, drive)
    std::vector<float> exc[2];
    const int nch = stereo ? 2 : 1;
    for (int c = 0; c < nch; ++c) {
        // 12 dB/oct high-pass, Q 0.5 (upstream HPF_Q; HPF_CUTOFF was a fixed 2 kHz)
        auto hp = filterdsp::makeHighpass(clampHz(spec.hpHz, sr), sr, 0.5);
        std::vector<float> h((size_t)n);
        for (int64_t i = 0; i < n; ++i) h[(size_t)i] = hp.process(ms[c][(size_t)i]) * drive + bias;
        // sigmoid -> arraya -> arraya at 2x
        auto up = hb.up(h);
        for (auto& v : up) v = arraya(arraya(sigmoid(v)));
        exc[c] = hb.down(up);
        // DC blocker (upstream blockDC, coefficient 0.995) and level compensation
        double x1 = 0.0, y1 = 0.0;
        for (auto& v : exc[c]) {
            const double y = (double)v - x1 + 0.995 * y1;
            x1 = v;
            y1 = y;
            v = (float)y * comp;
        }
    }

    std::vector<std::vector<float>> out((size_t)src.channels());
    for (int c = 0; c < src.channels(); ++c) {
        auto& o = out[(size_t)c];
        o.resize((size_t)(s1 - s0));
        const int b = std::min(c, nch - 1);
        for (int64_t i = s0 - sp.c0; i < n; ++i) {
            // Harmonics added on top (upstream crossfades: ms * (1 - excMix) + exc * excMix)
            o[(size_t)(i - (s0 - sp.c0))] = ms[c][(size_t)i] + exc[b][(size_t)i] * excMix;
        }
    }
    // Level matching: same RMS as the region had, one gain for all channels.
    double eIn = 0.0, eOut = 0.0;
    for (int c = 0; c < src.channels(); ++c)
        for (int64_t i = s0; i < s1; ++i) {
            const double x = src.ch[(size_t)c][(size_t)i], y = out[(size_t)c][(size_t)(i - s0)];
            eIn += x * x;
            eOut += y * y;
        }
    const float match = eOut > 1e-20 ? (float)std::sqrt(eIn / eOut) : 1.f;
    for (auto& o : out)
        for (auto& v : o) v *= match;
    fadeEdges(src, s0, s1, out);
    return out;
}

} // namespace exciterdsp
