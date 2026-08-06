// General-purpose FFT (any length, via Bluestein when not a power of two)
// used by the Harmonic loop module for the phase vocoder and the circular-
// correlation rotation search. Only called from the GUI thread when
// recomputing a module's preview (not RT-safe: it allocates memory).
#pragma once

#include <cmath>
#include <complex>
#include <vector>

namespace dsp {

using Complex = std::complex<double>;

namespace detail {

inline bool isPow2(size_t n) { return n && ((n & (n - 1)) == 0); }

// Iterative in-place radix-2 FFT (Cooley-Tukey), n a power of two.
inline void fftRadix2(std::vector<Complex>& a) {
    const size_t n = a.size();
    if (n <= 1) return;
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;
        Complex wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            Complex w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                Complex u = a[i + k];
                Complex v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

// Direct (forward, unnormalized, like numpy.fft.fft) DFT of arbitrary length
// via the Bluestein algorithm (chirp z-transform), leaning on fftRadix2 for
// the internal convolutions.
inline void bluestein(std::vector<Complex>& a) {
    const size_t n = a.size();
    size_t m = 1;
    while (m < 2 * n - 1) m <<= 1;

    std::vector<Complex> chirp(n);
    for (size_t k = 0; k < n; ++k) {
        double ang = M_PI * (double)((unsigned long long)k * (unsigned long long)k %
                                      (2ULL * (unsigned long long)n)) / (double)n;
        chirp[k] = Complex(std::cos(ang), -std::sin(ang));
    }
    std::vector<Complex> A(m, Complex(0.0, 0.0)), B(m, Complex(0.0, 0.0));
    for (size_t k = 0; k < n; ++k) A[k] = a[k] * chirp[k];
    B[0] = std::conj(chirp[0]);
    for (size_t k = 1; k < n; ++k) {
        B[k] = std::conj(chirp[k]);
        B[m - k] = std::conj(chirp[k]);
    }
    fftRadix2(A);
    fftRadix2(B);
    for (size_t k = 0; k < m; ++k) A[k] *= B[k];
    // ifft of the convolution via the conjugate trick (ifft(x) = conj(fft(conj(x)))/m)
    for (auto& v : A) v = std::conj(v);
    fftRadix2(A);
    const double invM = 1.0 / (double)m;
    for (size_t k = 0; k < n; ++k) a[k] = std::conj(A[k]) * invM * chirp[k];
}

} // namespace detail

// In-place DFT. forward (inverse=false): unnormalized, like numpy.fft.fft.
// inverse=true: normalized by 1/n, like numpy.fft.ifft.
inline void dft(std::vector<Complex>& a, bool inverse) {
    const size_t n = a.size();
    if (n <= 1) return;
    if (inverse)
        for (auto& v : a) v = std::conj(v);
    if (detail::isPow2(n))
        detail::fftRadix2(a);
    else
        detail::bluestein(a);
    if (inverse) {
        const double invN = 1.0 / (double)n;
        for (auto& v : a) v = std::conj(v) * invN;
    }
}

// numpy-style rfft/irfft: real input of n samples -> n/2+1 complex bins
// (rfft), and the inverse path assuming Hermitian symmetry (irfft). Used by
// the phase vocoder; always called with n = fft size (a power of two), but
// dft() supports any length (also used for the circular correlation of the
// rotation search, which operates over the loop duration, generally not a
// power of two).
inline std::vector<Complex> rfft(const std::vector<double>& x) {
    std::vector<Complex> a(x.begin(), x.end());
    dft(a, false);
    a.resize(x.size() / 2 + 1);
    return a;
}

inline std::vector<double> irfft(const std::vector<Complex>& half, size_t n) {
    std::vector<Complex> full(n);
    const size_t nBins = n / 2 + 1;
    for (size_t k = 0; k < nBins && k < half.size(); ++k) full[k] = half[k];
    for (size_t k = nBins; k < n; ++k) full[k] = std::conj(full[n - k]);
    dft(full, true);
    std::vector<double> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = full[i].real();
    return out;
}

} // namespace dsp
