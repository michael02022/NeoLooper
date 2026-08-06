// 16-point sinc interpolator (8 samples on each side), Kaiser window, for
// real-time pitched playback (root note + finetune, or "resample-free"
// resampling of the sample rate heard through the piano). Much more
// faithful than cubic interpolation: aliasing/brightness loss when playing
// the sample sharper than its original pitch stops being audible.
//
// The table is built once on the main thread (never on the audio thread)
// and the per-sample computation is just lookups + products: RT-safe.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

class SincInterp16 {
public:
    static constexpr int kTaps = 16;
    static constexpr int kHalf = kTaps / 2; // 8 samples on each side

    SincInterp16() { build(); }

    // t in [0,1): fraction between floor(pos) and floor(pos)+1. Fills
    // w[0..15], where w[k] weights the sample at floor(pos) + (k - (kHalf-1)),
    // i.e. the range floor(pos)-7 .. floor(pos)+8.
    void weights(float t, float w[kTaps]) const {
        for (int k = 0; k < kTaps; ++k) {
            float dist = (float)(k - (kHalf - 1)) - t;
            w[k] = lookup(dist);
        }
    }

private:
    static constexpr int kOversample = 512;
    std::vector<float> table_; // sinc*kaiser sampled over [0, kHalf]

    static double besselI0(double x) {
        double sum = 1.0, term = 1.0, half = x * 0.5;
        for (int k = 1; k < 32; ++k) {
            term *= (half / k) * (half / k);
            sum += term;
            if (term < 1e-16 * sum) break;
        }
        return sum;
    }

    void build() {
        const double beta = 8.0; // sidelobe attenuation suited to 16 taps
        const int n = kHalf * kOversample + 2;
        table_.resize((size_t)n);
        const double i0b = besselI0(beta);
        for (int i = 0; i < n; ++i) {
            double u = (double)i / kOversample; // 0..kHalf
            double x = u / kHalf;
            double win = (x <= 1.0)
                             ? besselI0(beta * std::sqrt(std::max(0.0, 1.0 - x * x))) / i0b
                             : 0.0;
            double s = (u == 0.0) ? 1.0 : std::sin(M_PI * u) / (M_PI * u);
            table_[(size_t)i] = (float)(s * win);
        }
        table_[(size_t)(n - 1)] = 0.f;
    }

    float lookup(float dist) const {
        float ad = std::fabs(dist);
        if (ad >= (float)kHalf) return 0.f;
        float f = ad * kOversample;
        int i = (int)f;
        if (i >= (int)table_.size() - 1) return 0.f;
        float frac = f - i;
        return table_[(size_t)i] + (table_[(size_t)i + 1] - table_[(size_t)i]) * frac;
    }
};
