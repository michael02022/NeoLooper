// Minimal stand-ins for the few JUCE pieces Snaptune's DSP uses (jlimit,
// MathConstants, SmoothedValue, AbstractFifo, dsp::FFT), so NeoLooper can
// build that DSP without pulling in JUCE. Same names and semantics as JUCE
// for exactly the calls Snaptune makes -- nothing more. Written for
// NeoLooper; not part of Snaptune.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace juce {

template <typename T>
constexpr T jlimit(T lowerLimit, T upperLimit, T value) noexcept {
    return value < lowerLimit ? lowerLimit : (upperLimit < value ? upperLimit : value);
}

template <typename T>
struct MathConstants {
    static constexpr T pi = static_cast<T>(3.141592653589793238L);
    static constexpr T twoPi = static_cast<T>(2 * 3.141592653589793238L);
    static constexpr T halfPi = static_cast<T>(3.141592653589793238L / 2);
};

// Linear ramp toward the target over rampLengthInSeconds (JUCE's default
// ValueSmoothingTypes::Linear).
template <typename T>
class SmoothedValue {
public:
    void reset(double sampleRate, double rampLengthInSeconds) {
        stepsToTarget_ = std::max(0, (int)std::floor(rampLengthInSeconds * sampleRate));
        setCurrentAndTargetValue(target_);
    }
    void setCurrentAndTargetValue(T v) {
        current_ = target_ = v;
        countdown_ = 0;
    }
    void setTargetValue(T v) {
        if (v == target_) return;
        if (stepsToTarget_ <= 0) {
            setCurrentAndTargetValue(v);
            return;
        }
        target_ = v;
        countdown_ = stepsToTarget_;
        step_ = (target_ - current_) / (T)countdown_;
    }
    T getNextValue() {
        if (countdown_ <= 0) return target_;
        --countdown_;
        if (countdown_ == 0) current_ = target_;
        else current_ += step_;
        return current_;
    }

private:
    T current_ = 0, target_ = 0, step_ = 0;
    int countdown_ = 0, stepsToTarget_ = 0;
};

// Single-producer/single-consumer ring index manager. Snaptune uses it from
// one thread at a time here (NeoLooper runs the engine offline), so plain
// ints suffice. Like JUCE, one slot stays empty (capacity = size - 1).
class AbstractFifo {
public:
    explicit AbstractFifo(int capacity) : size_(capacity) {}
    int getNumReady() const noexcept {
        return end_ >= start_ ? end_ - start_ : size_ - (start_ - end_);
    }
    int getFreeSpace() const noexcept { return size_ - 1 - getNumReady(); }

    struct Scope {
        int startIndex1 = 0, blockSize1 = 0, startIndex2 = 0, blockSize2 = 0;
    };
    Scope write(int n) {
        Scope s;
        n = std::min(n, getFreeSpace());
        if (n <= 0) return s;
        s.startIndex1 = end_;
        s.blockSize1 = std::min(size_ - end_, n);
        s.blockSize2 = n - s.blockSize1;
        end_ = (end_ + n) % size_;
        return s;
    }
    Scope read(int n) {
        Scope s;
        n = std::min(n, getNumReady());
        if (n <= 0) return s;
        s.startIndex1 = start_;
        s.blockSize1 = std::min(size_ - start_, n);
        s.blockSize2 = n - s.blockSize1;
        start_ = (start_ + n) % size_;
        return s;
    }

private:
    int size_;
    int start_ = 0, end_ = 0;
};

namespace dsp {

// Real-only FFT with JUCE's buffer layout: the buffer holds 2*size floats;
// forward reads size reals and writes size/2+1 interleaved complex bins
// (re, im); inverse reads those bins (Hermitian symmetry assumed) and
// writes size reals, scaled by 1/size like JUCE's fallback engine.
// (Snaptune measures the inverse scale itself, so only consistency matters.)
// The size-N real transform runs as one size-N/2 complex FFT (even/odd
// samples packed as re/im) plus a split step, with precomputed tables.
class FFT {
public:
    explicit FFT(int order) : size_(1 << order), half_(1 << (order - 1)) {
        const double pi = MathConstants<double>::pi;
        buf_.resize((size_t)half_);
        tw_.resize((size_t)half_ / 2 + 1);
        for (int k = 0; k <= half_ / 2; ++k)
            tw_[(size_t)k] = std::polar(1.0, -2.0 * pi * k / half_);
        split_.resize((size_t)half_ + 1);
        for (int k = 0; k <= half_; ++k)
            split_[(size_t)k] = std::polar(1.0, -2.0 * pi * k / size_);
        rev_.resize((size_t)half_);
        for (int i = 0, j = 0; i < half_; ++i) {
            rev_[(size_t)i] = j;
            int bit = half_ >> 1;
            for (; bit && (j & bit); bit >>= 1) j ^= bit;
            j ^= bit;
        }
    }
    int getSize() const noexcept { return size_; }

    void performRealOnlyForwardTransform(float* d, bool /*onlyNonNegative*/ = false) {
        for (int i = 0; i < half_; ++i) buf_[(size_t)rev_[(size_t)i]] = {(double)d[2 * i], (double)d[2 * i + 1]};
        transform(false);
        // X[k] = E[k] + W^k O[k], with E/O recovered from Z = FFT(even + i*odd).
        for (int k = 0; k <= half_; ++k) {
            const auto zk = buf_[(size_t)(k % half_)];
            const auto zn = buf_[(size_t)((half_ - k) % half_)];
            // e = (zk + conj(zn)) / 2, o = -i (zk - conj(zn)) / 2
            const double er = 0.5 * (zk.real() + zn.real()), ei = 0.5 * (zk.imag() - zn.imag());
            const double orr = 0.5 * (zk.imag() + zn.imag()), oi = -0.5 * (zk.real() - zn.real());
            const double wr = split_[(size_t)k].real(), wi = split_[(size_t)k].imag();
            d[2 * k] = (float)(er + wr * orr - wi * oi);
            d[2 * k + 1] = (float)(ei + wr * oi + wi * orr);
        }
    }
    void performRealOnlyInverseTransform(float* d) {
        // Inverse of the split: Z[k] = E[k] + i*O[k] from X[k] and X[N/2-k].
        std::vector<std::complex<double>>& z = tmp_;
        z.resize((size_t)half_);
        for (int k = 0; k < half_; ++k) {
            const double xr = d[2 * k], xi = d[2 * k + 1];
            const double nr = d[2 * (half_ - k)], ni = -d[2 * (half_ - k) + 1]; // conj(X[N/2-k])
            const double er = 0.5 * (xr + nr), ei = 0.5 * (xi + ni);
            const double dr = 0.5 * (xr - nr), di = 0.5 * (xi - ni);
            const double wr = split_[(size_t)k].real(), wi = -split_[(size_t)k].imag(); // conj(W^k)
            const double orr = dr * wr - di * wi, oi = dr * wi + di * wr;
            z[(size_t)k] = {er - oi, ei + orr}; // e + i*o
        }
        for (int i = 0; i < half_; ++i) buf_[(size_t)rev_[(size_t)i]] = z[(size_t)i];
        transform(true);
        // The unnormalized half-size inverse yields (N/2) * (even + i*odd):
        // dividing by N/2 gives back the input, like JUCE's 1/size scaling.
        const double scale = 1.0 / half_;
        for (int i = 0; i < half_; ++i) {
            d[2 * i] = (float)(buf_[(size_t)i].real() * scale);
            d[2 * i + 1] = (float)(buf_[(size_t)i].imag() * scale);
        }
    }

private:
    // Iterative radix-2 Cooley-Tukey on buf_ (already in bit-reversed
    // order). Products written out by hand: std::complex's operator* checks
    // for NaN/inf (C99 Annex G) unless -ffast-math, several times slower.
    void transform(bool inverse) {
        const int n = half_;
        double* b = reinterpret_cast<double*>(buf_.data()); // re, im interleaved
        const double* t = reinterpret_cast<const double*>(tw_.data());
        const double sgn = inverse ? -1.0 : 1.0;
        for (int len = 2; len <= n; len <<= 1) {
            const int stride = n / len, h = len / 2;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < h; ++k) {
                    const double wr = t[2 * k * stride], wi = sgn * t[2 * k * stride + 1];
                    double* u = b + 2 * (i + k);
                    double* v = b + 2 * (i + k + h);
                    const double vr = v[0] * wr - v[1] * wi, vi = v[0] * wi + v[1] * wr;
                    v[0] = u[0] - vr;
                    v[1] = u[1] - vi;
                    u[0] += vr;
                    u[1] += vi;
                }
        }
    }

    int size_, half_;
    std::vector<std::complex<double>> buf_, tw_, split_, tmp_;
    std::vector<int> rev_;
};

} // namespace dsp
} // namespace juce
