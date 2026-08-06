#include "resampler.hpp"

#include <cmath>
#include <cstdint>

namespace {

// Bessel I0 (series), for the Kaiser window.
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    double half = x * 0.5;
    for (int k = 1; k < 64; ++k) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term < 1e-18 * sum) break;
    }
    return sum;
}

// Windowed-sinc table: h(u) = sinc(u) * kaiser(u / ZC), u in [0, ZC].
constexpr int kZeroCrossings = 24;   // filter half-width in zero crossings
constexpr int kTableOversample = 512;
constexpr double kKaiserBeta = 13.0; // sidelobe attenuation ~ -130 dB

struct SincTable {
    std::vector<double> t;
    SincTable() {
        const int n = kZeroCrossings * kTableOversample + 2;
        t.resize(n);
        const double i0b = besselI0(kKaiserBeta);
        for (int i = 0; i < n; ++i) {
            double u = (double)i / kTableOversample; // 0..ZC
            double x = u / kZeroCrossings;           // 0..1
            double w = (x <= 1.0)
                ? besselI0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - x * x))) / i0b
                : 0.0;
            double s = (u == 0.0) ? 1.0 : std::sin(M_PI * u) / (M_PI * u);
            t[i] = s * w;
        }
        t[n - 1] = 0.0;
    }
    // h(u) with linear interpolation; u >= 0
    inline double operator()(double u) const {
        double f = u * kTableOversample;
        int i = (int)f;
        if (i >= (int)t.size() - 1) return 0.0;
        double frac = f - i;
        return t[i] + (t[i + 1] - t[i]) * frac;
    }
};

} // namespace

std::vector<std::vector<float>> resampleHQ(
    const std::vector<std::vector<float>>& in, double srIn, double srOut) {
    std::vector<std::vector<float>> out(in.size());
    if (in.empty() || in[0].empty() || srIn <= 0 || srOut <= 0) return out;
    if (srIn == srOut) { out = in; return out; }

    static const SincTable table; // shared, immutable once constructed

    const double ratio = srOut / srIn;
    // When lowering the sample rate the cutoff sits below the new Nyquist to
    // remove aliasing; a small margin leaves room for the filter transition.
    const double cutoff = std::min(1.0, ratio) * 0.945;
    const double halfWidth = kZeroCrossings / cutoff; // in input samples
    const int64_t nIn = (int64_t)in[0].size();
    const int64_t nOut = (int64_t)std::llround((double)nIn * ratio);

    for (size_t c = 0; c < in.size(); ++c) {
        const std::vector<float>& src = in[c];
        std::vector<float>& dst = out[c];
        dst.resize((size_t)nOut);
        for (int64_t k = 0; k < nOut; ++k) {
            const double t = (double)k / ratio; // position in the input
            const int64_t i0 = (int64_t)std::ceil(t - halfWidth);
            const int64_t i1 = (int64_t)std::floor(t + halfWidth);
            double acc = 0.0;
            const int64_t a = std::max<int64_t>(0, i0);
            const int64_t b = std::min<int64_t>(nIn - 1, i1);
            for (int64_t i = a; i <= b; ++i) {
                double u = std::fabs(t - (double)i) * cutoff;
                acc += (double)src[(size_t)i] * table(u);
            }
            dst[(size_t)k] = (float)(acc * cutoff);
        }
    }
    return out;
}
