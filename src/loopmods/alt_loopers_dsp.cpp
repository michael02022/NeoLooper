#include "alt_loopers_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "../fft.hpp"
#include "../sinc_interp.hpp"
#include "loop_module.hpp" // xfadeGains

namespace {

// Tukey window over n samples: cosine ramps of pct/100 * n/2 samples on
// each side (pct = 100 -> Hann, 0 -> rectangular).
std::vector<double> tukey(int64_t n, float pct) {
    std::vector<double> w((size_t)n, 1.0);
    const double a = std::clamp((double)pct, 0.0, 100.0) / 100.0;
    if (a <= 0.0) return w;
    const double edge = a * (double)n / 2.0;
    for (int64_t i = 0; i < n; ++i) {
        const double d = std::min((double)i, (double)(n - 1 - i));
        if (d < edge) w[(size_t)i] = 0.5 * (1.0 - std::cos(M_PI * d / edge));
    }
    return w;
}

// Inclusive [lo, hi] FFT bins of harmonic h (1-based) for 'cycles' cycles in
// an n-point frame: band edges halfway between harmonics, the first band
// starting at bin 1 (so sub-fundamental energy goes to h=1), the last one
// stopping below Nyquist.
struct Band {
    int64_t h, lo, hi;
};
std::vector<Band> harmonicBands(int64_t n, int64_t cycles) {
    std::vector<Band> bands;
    const int64_t nyq = n / 2;
    int64_t prevEnd = 0;
    for (int64_t h = 1; h * cycles < nyq; ++h) {
        const int64_t end = std::min(h * cycles + cycles / 2, nyq - 1);
        if (end >= prevEnd + 1) bands.push_back({h, prevEnd + 1, end});
        prevEnd = end;
    }
    return bands;
}

// altloop::bandValue (a width per mille) in bins of an FFT whose harmonic
// spacing is 'spacing' bins, at least 0.01 bins (0 = a single bin).
double widthBins(double freq, const altloop::Params& p, double spacing) {
    return std::max(0.01, spacing *
                              altloop::bandValue(freq, p.lowFreq, p.lowWidth, p.highFreq,
                                                 p.highWidth, p.bandCurve, p.bandBend) /
                              1000.0);
}

// Daniell smoothing of power[lo, hi) with the Hann kernel
// 0.5 + 0.5*cos(pi*j/(half+1)), |j| <= half, renormalized where the kernel is
// cut by lo/hi. O(n) whatever the width: the kernel splits into a box sum
// plus a cosine-modulated one, cos(t*(bb-b)) = Re(e^{i*t*bb} * e^{-i*t*b}),
// both read from prefix sums.
std::vector<double> daniellSmooth(const std::vector<double>& power, int64_t lo, int64_t hi,
                                  int64_t half) {
    const int64_t n = hi - lo;
    const double t = M_PI / (double)(half + 1);
    std::vector<double> boxP((size_t)n + 1, 0.0);
    std::vector<dsp::Complex> modP((size_t)n + 1), modW((size_t)n + 1);
    modP[0] = modW[0] = dsp::Complex(0.0, 0.0);
    for (int64_t i = 0; i < n; ++i) {
        const dsp::Complex e = std::polar(1.0, t * (double)i);
        boxP[(size_t)i + 1] = boxP[(size_t)i] + power[(size_t)(lo + i)];
        modP[(size_t)i + 1] = modP[(size_t)i] + power[(size_t)(lo + i)] * e;
        modW[(size_t)i + 1] = modW[(size_t)i] + e;
    }
    std::vector<double> out(power.size(), 0.0);
    for (int64_t i = 0; i < n; ++i) {
        const int64_t a = std::max<int64_t>(0, i - half), b = std::min<int64_t>(n, i + half + 1);
        const dsp::Complex rot = std::polar(1.0, -t * (double)i);
        const double acc = 0.5 * (boxP[(size_t)b] - boxP[(size_t)a]) +
                           0.5 * ((modP[(size_t)b] - modP[(size_t)a]) * rot).real();
        const double wsum = 0.5 * (double)(b - a) + 0.5 * ((modW[(size_t)b] - modW[(size_t)a]) * rot).real();
        out[(size_t)(lo + i)] = std::max(0.0, acc / wsum);
    }
    return out;
}

std::mt19937_64 makeRng(int64_t seed) {
    return std::mt19937_64(seed < 0 ? ((uint64_t)std::random_device{}() << 32) ^ std::random_device{}()
                                    : (uint64_t)seed);
}

// Rotation of loop (shared by every channel) that best aligns it with ref:
// argmax_s sum_c sum_i loop_c[(i+s)%n] * ref_c[i], the multichannel version
// of specloop::circularBestShift (one rotation for all channels -- per
// channel rotations would introduce inter-channel delays).
// loopHalf is the loop's own half spectrum (n/2+1 bins, it was synthesized
// from it), so only the reference needs a forward transform. A reference
// longer than the loop is folded modulo n: it is compared against the loop
// tiled periodically, which is what tiledBlend() puts under it.
int64_t sharedBestShift(const std::vector<std::vector<dsp::Complex>>& loopHalf, size_t n,
                        const std::vector<std::vector<double>>& ref) {
    if (n == 0 || ref.empty() || ref[0].empty()) return 0;
    std::vector<dsp::Complex> acc(n, dsp::Complex(0.0, 0.0));
    for (size_t c = 0; c < loopHalf.size(); ++c) {
        std::vector<dsp::Complex> b(n, dsp::Complex(0.0, 0.0));
        for (size_t i = 0; i < ref[c].size(); ++i) b[i % n] += ref[c][i];
        dsp::dft(b, false);
        for (size_t i = 0; i < n; ++i) {
            const size_t k = (i <= n / 2) ? i : n - i;
            const dsp::Complex a = (i <= n / 2) ? loopHalf[c][k] : std::conj(loopHalf[c][k]);
            acc[i] += a * std::conj(b[i]);
        }
    }
    dsp::dft(acc, true);
    size_t best = 0;
    for (size_t i = 1; i < n; ++i)
        if (acc[i].real() > acc[best].real()) best = i;
    return (int64_t)best;
}

} // namespace

namespace altloop {

// See the .hpp: specloop::rotateAndBlend, but the entry crossfade may be LONGER than the
// loop: the rotated loop is tiled periodically under the attack (as many
// whole cycles as the crossfade needs, generated on the fly) while
// loopRegion stays exactly one loop long -- e.g. a single-cycle Freeze
// loop can still fade in over 30 ms of attack. Same layout as
// rotateAndBlend otherwise (attackTail ends on rotated[(xfadeN-1)%n],
// loopRegion starts on rotated[xfadeN%n]).
void tiledBlend(const std::vector<double>& loop, const std::vector<double>& ref, int64_t shift,
                int64_t xfadeN, float xfadeCurve, specloop::RotatedBlend& out) {
    const int64_t n = (int64_t)loop.size();
    auto lb = [&](int64_t i) { return loop[(size_t)((i + shift) % n)]; };
    out.attackTail.resize((size_t)xfadeN);
    for (int64_t j = 0; j < xfadeN; ++j) {
        const float t = (xfadeN > 1) ? (float)j / (float)(xfadeN - 1) : 0.f;
        float gIn, gOut;
        xfadeGains(t, xfadeCurve, gIn, gOut);
        out.attackTail[(size_t)j] = (float)ref[(size_t)j] * gOut + (float)lb(j) * gIn;
    }
    out.loopRegion.resize((size_t)n);
    for (int64_t i = 0; i < n; ++i) out.loopRegion[(size_t)i] = (float)lb(i + xfadeN);
}

namespace {
// (1 - e^{-a t}) / (1 - e^{-a}): a > 0 convex, a < 0 concave, a = 0 linear.
double expBend(double t, double a) {
    if (std::fabs(a) < 1e-6) return t;
    return (1.0 - std::exp(-a * t)) / (1.0 - std::exp(-a));
}
} // namespace

double bandShape(double t, BandCurve mode, double bend) {
    t = std::clamp(t, 0.0, 1.0);
    const double a = 8.0 * std::clamp(bend, -1.0, 1.0);
    if (mode == BandCurve::Curve) return expBend(t, a);
    // Bipolar: each half is the bend, the second half mirrored -- a concave
    // first half (bend > 0) gives an S, a convex one an inverted S.
    return t < 0.5 ? 0.5 * expBend(2.0 * t, -a) : 1.0 - 0.5 * expBend(2.0 * (1.0 - t), -a);
}

double bandValue(double freq, double f1, double v1, double f2, double v2, BandCurve curve,
                 double bend) {
    if (f2 <= f1 || freq <= f1) return (f2 <= f1 && freq >= f2) ? v2 : v1;
    if (freq >= f2) return v2;
    return v1 + (v2 - v1) * bandShape(std::log(freq / f1) / std::log(f2 / f1), curve, bend);
}

double notePeriod(double sampleRate, int rootNote, float tuneCents) {
    const double f0 =
        440.0 * std::pow(2.0, ((double)rootNote - 69.0 + (double)tuneCents / 100.0) / 12.0);
    return sampleRate / f0;
}

// Steps 1-4 (see the .hpp); also returns the loop's half spectra (pre
// level-match, same phases) so build() can search the rotation without
// transforming the loop again.
static bool rebuildImpl(const AudioData& src, int64_t ls, int64_t le, const Params& p,
                        Result& out, std::vector<std::vector<double>>& loop,
                        std::vector<std::vector<dsp::Complex>>* spectra) {
    out = Result();
    const int nch = src.channels();
    const double sr = src.sampleRate;
    if (nch < 1 || ls < 0 || le > src.frames() || le - ls < 8) {
        out.error = "The loop must contain at least 8 samples";
        return false;
    }
    const int64_t L = le - ls;

    // --- 1. Period ----------------------------------------------------------
    const bool needsPitch = p.method != Method::RandomPhase || p.integerCycles;
    out.period = notePeriod(sr, p.rootNote, p.tuneCents);
    if (needsPitch && (double)L < 2.0 * out.period) {
        out.error = "The loop must contain at least two cycles of the root note";
        return false;
    }

    // --- 2. Analysis region: k whole cycles in nA samples -------------------
    int64_t k = 0, nA = L;
    double step = 1.0;
    if (needsPitch) {
        k = (int64_t)std::floor((double)L / out.period);
        const int64_t minCycles = p.method == Method::Ensemble ? 10 : 2;
        if (k < minCycles) {
            out.error = p.method == Method::Ensemble
                            ? "Ensemble needs at least 10 cycles in the loop"
                            : "The loop must contain at least two cycles";
            return false;
        }
        nA = std::min<int64_t>(L, std::max<int64_t>(8, (int64_t)std::llround((double)k * out.period)));
        step = (double)k * out.period / (double)nA;
    }
    out.cycles = k;

    std::vector<std::vector<double>> raw((size_t)nch, std::vector<double>((size_t)nA));
    if (std::fabs(step - 1.0) < 1e-12) {
        for (int c = 0; c < nch; ++c)
            for (int64_t i = 0; i < nA; ++i) raw[c][(size_t)i] = src.ch[c][(size_t)(ls + i)];
    } else {
        // Band-limited (16-tap sinc) read.
        static const SincInterp16 sinc;
        float w[SincInterp16::kTaps];
        for (int64_t i = 0; i < nA; ++i) {
            const double pos = (double)ls + (double)i * step;
            const int64_t base = (int64_t)std::floor(pos);
            sinc.weights((float)(pos - (double)base), w);
            for (int c = 0; c < nch; ++c) {
                double acc = 0.0;
                for (int t = 0; t < SincInterp16::kTaps; ++t)
                    acc += (double)w[t] * (double)src.at(c, base + t - (SincInterp16::kHalf - 1));
                raw[c][(size_t)i] = acc;
            }
        }
    }

    // --- 3. Spectral rebuild ------------------------------------------------
    const double amount = std::clamp((double)p.randomPct, 0.0, 100.0) / 100.0;
    const auto win = tukey(nA, p.method == Method::Ensemble  ? 100.f
                               : p.method == Method::RandomPhase ? p.windowPct * (float)amount
                                                                 : p.windowPct);
    std::vector<std::vector<dsp::Complex>> X((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        std::vector<double> xw((size_t)nA);
        for (int64_t i = 0; i < nA; ++i) xw[(size_t)i] = raw[c][(size_t)i] * win[(size_t)i];
        X[c] = dsp::rfft(xw);
    }
    const int64_t nBins = nA / 2 + 1;
    auto rng = makeRng(p.seed);
    std::uniform_real_distribution<double> uni(0.0, 2.0 * M_PI);

    int64_t nOut = nA; // output loop length
    std::vector<std::vector<dsp::Complex>> Y((size_t)nch);

    if (p.method == Method::RandomPhase) {
        // A random offset of up to +-amount*pi per bin, the same on every
        // channel (inter-channel phase differences -- the stereo image --
        // survive) unless decorrelate draws one per channel. DC and a real
        // Nyquist bin are left untouched.
        const int64_t last = (nA % 2 == 0) ? nBins - 1 : nBins;
        std::vector<std::vector<double>> phi((size_t)(p.decorrelate ? nch : 1),
                                             std::vector<double>((size_t)nBins, 0.0));
        for (auto& ph : phi)
            for (int64_t b = 1; b < last; ++b) ph[(size_t)b] = amount * (uni(rng) - M_PI);

        // Daniell smoothing (see Params::smoothHz), Hann kernel over +-half
        // bins between DC and Nyquist (both excluded).
        const int64_t half = (int64_t)std::llround((double)p.smoothHz * (double)nA / sr / 2.0);
        for (int c = 0; c < nch; ++c) {
            Y[c] = X[c];
            std::vector<double> mag;
            if (p.smoothHz > 0.f && half >= 1 && last > 2) {
                std::vector<double> power((size_t)nBins);
                for (int64_t b = 0; b < nBins; ++b) power[(size_t)b] = std::norm(X[c][(size_t)b]);
                mag = daniellSmooth(power, 1, last, half);
                for (auto& v : mag) v = std::sqrt(v);
            }
            const auto& ph = phi[p.decorrelate ? (size_t)c : 0];
            for (int64_t b = 1; b < last; ++b) {
                auto& v = Y[c][(size_t)b];
                if (!mag.empty()) v = std::polar(mag[(size_t)b], std::arg(v));
                v *= std::polar(1.0, ph[(size_t)b]);
            }
        }
        out.loopCycles = k;
    } else if (p.method == Method::HarmonicFreeze) {
        // Band energy -> one exact harmonic with the original phase. With
        // m < k output cycles, harmonic h goes to bin h*m of an
        // nOut = round(m*P) frame instead of h*k of nA: a shorter loop with
        // the same waveform (a single cycle at m=1).
        const int64_t m = (p.loopCycles <= 0) ? k : std::min<int64_t>(p.loopCycles, k);
        nOut = (m == k) ? nA : std::max<int64_t>(2, (int64_t)std::llround((double)m * out.period));
        out.loopCycles = m;
        const auto bands = harmonicBands(nA, k);
        for (int c = 0; c < nch; ++c) {
            Y[c].assign((size_t)(nOut / 2 + 1), dsp::Complex(0.0, 0.0));
            for (const auto& bd : bands) {
                const int64_t center = bd.h * k, dst = bd.h * m;
                if (center >= nBins - 1 || 2 * dst >= nOut) continue;
                const dsp::Complex src0 = X[c][(size_t)center];
                if (std::abs(src0) <= 0.0) continue;
                double energy = 0.0;
                for (int64_t b = bd.lo; b <= bd.hi; ++b) energy += std::norm(X[c][(size_t)b]);
                Y[c][(size_t)dst] = std::polar(std::sqrt(energy), std::arg(src0));
            }
        }
    } else {
        // Ensemble (see Method::Ensemble / Params): per channel, estimate the
        // sample's own noise residual, take each harmonic band's energy
        // minus that residual, move the harmonic (inharmonicity), spread it
        // with the chosen shape and add the residual back at floorPct.
        // Random phases are shared by every channel plus each channel's
        // original phase difference at that band's harmonic (keeps the
        // stereo image instead of collapsing it to amplitude panning).
        const auto bands = harmonicBands(nA, k);
        std::vector<double> phi((size_t)nBins);
        for (auto& v : phi) v = uni(rng);
        std::vector<int64_t> bandOf((size_t)nBins, -1);
        for (size_t i = 0; i < bands.size(); ++i)
            for (int64_t b = bands[i].lo; b <= bands[i].hi; ++b) bandOf[(size_t)b] = (int64_t)i;
        const double sqrtLog = std::sqrt(2.0 * std::log(1e5));
        const double B = std::max(0.0, (double)p.inharmonicity);
        const int64_t top = nBins - 2; // highest usable bin (below Nyquist)
        // Residual sampling: bins within +-guard of a harmonic are masked
        // (Hann main lobe and some vibrato), the middle of each gap is kept.
        const int64_t guard = std::max<int64_t>(2, (int64_t)std::llround(0.25 * (double)k));

        auto addPower = [&](std::vector<double>& pw, double bin, double e) {
            // Power e at a fractional bin, split between its two neighbors.
            const int64_t b0 = (int64_t)std::floor(bin);
            const double f = bin - (double)b0;
            if (b0 >= 1 && b0 <= top) pw[(size_t)b0] += e * (1.0 - f);
            if (b0 + 1 >= 1 && b0 + 1 <= top) pw[(size_t)(b0 + 1)] += e * f;
        };

        for (int c = 0; c < nch; ++c) {
            std::vector<double> power((size_t)nBins);
            for (int64_t b = 0; b < nBins; ++b) power[(size_t)b] = std::norm(X[c][(size_t)b]);

            // --- noise residual: mask the harmonics, interpolate the gaps
            // linearly between kept bins, smooth over half a harmonic spacing.
            std::vector<char> masked((size_t)nBins, 0);
            for (const auto& bd : bands)
                for (int64_t b = std::max<int64_t>(1, bd.h * k - guard);
                     b <= std::min<int64_t>(top, bd.h * k + guard); ++b)
                    masked[(size_t)b] = 1;
            std::vector<double> noise((size_t)nBins, 0.0);
            int64_t prev = -1;
            for (int64_t b = 1; b <= top; ++b) {
                if (masked[(size_t)b]) continue;
                noise[(size_t)b] = power[(size_t)b];
                const int64_t from = prev < 0 ? 1 : prev + 1;
                for (int64_t g = from; g < b; ++g)
                    noise[(size_t)g] = prev < 0 ? power[(size_t)b]
                                                : power[(size_t)prev] + (power[(size_t)b] - power[(size_t)prev]) *
                                                                            (double)(g - prev) / (double)(b - prev);
                prev = b;
            }
            if (prev < 0) {
                std::fill(noise.begin(), noise.end(), 0.0); // no gap between harmonics to read it from
            } else {
                for (int64_t g = prev + 1; g <= top; ++g) noise[(size_t)g] = power[(size_t)prev];
                noise = daniellSmooth(noise, 1, top + 1, std::max<int64_t>(1, k / 2));
            }

            std::vector<double> newPower((size_t)nBins, 0.0);
            for (const auto& bd : bands) {
                double energy = 0.0;
                for (int64_t b = bd.lo; b <= bd.hi; ++b)
                    energy += std::max(0.0, power[(size_t)b] - noise[(size_t)b]);
                const double hh = (double)bd.h;
                const double center =
                    hh * (double)k * std::sqrt((1.0 + B * hh * hh) / (1.0 + B));
                if (center > (double)top) continue;
                const int64_t ci = (int64_t)std::llround(center);

                if (p.shape == HarmonicShape::Voices) {
                    const int nv = std::clamp(p.voices, 2, 16);
                    const double detune =
                        bandValue(center * sr / (double)nA, p.lowFreq, p.lowDetuneCents,
                                  p.highFreq, p.highDetuneCents, p.bandCurve, p.bandBend);
                    for (int v = 0; v < nv; ++v) {
                        const double cents = detune * (2.0 * v / (nv - 1) - 1.0);
                        addPower(newPower, center * std::pow(2.0, cents / 1200.0), energy / nv);
                    }
                    continue;
                }
                const double sigma = widthBins(center * sr / (double)nA, p, (double)k);
                std::vector<double> g;
                int64_t jmax;
                if (p.shape == HarmonicShape::Flat) {
                    // Even band [-a, a], a = sigma*sqrt(3) (same variance as
                    // the Gaussian): each bin j gets the part of the band
                    // inside [j-0.5, j+0.5] -- under half a bin it's all in
                    // the center.
                    const double a = sigma * std::sqrt(3.0);
                    jmax = (int64_t)std::ceil(a + 0.5);
                    g.resize((size_t)(2 * jmax + 1));
                    for (int64_t j = -jmax; j <= jmax; ++j)
                        g[(size_t)(j + jmax)] =
                            std::max(0.0, std::min((double)j + 0.5, a) - std::max((double)j - 0.5, -a));
                } else {
                    jmax = std::max<int64_t>(1, (int64_t)std::ceil(1.0 + sigma * sqrtLog));
                    g.resize((size_t)(2 * jmax + 1));
                    for (int64_t j = -jmax; j <= jmax; ++j)
                        g[(size_t)(j + jmax)] = std::exp(-(double)(j * j) / (2.0 * sigma * sigma));
                }
                double gsum = 0.0;
                for (double v : g) gsum += v;
                for (int64_t j = -jmax; j <= jmax; ++j) {
                    const int64_t b = ci + j;
                    if (b >= 1 && b <= top) newPower[(size_t)b] += energy * g[(size_t)(j + jmax)] / gsum;
                }
            }
            if (p.floorPct > 0.f)
                for (int64_t b = 1; b <= top; ++b)
                    newPower[(size_t)b] += noise[(size_t)b] * (double)p.floorPct / 100.0;

            Y[c].assign((size_t)nBins, dsp::Complex(0.0, 0.0));
            for (int64_t b = 1; b <= top; ++b) {
                double ph = phi[(size_t)b];
                const int64_t bi = bandOf[(size_t)b];
                if (c > 0 && bi >= 0) {
                    const int64_t center = std::min<int64_t>(bands[(size_t)bi].h * k, nBins - 1);
                    ph += std::arg(X[c][(size_t)center]) - std::arg(X[0][(size_t)center]);
                }
                Y[c][(size_t)b] = std::polar(std::sqrt(std::max(0.0, newPower[(size_t)b])), ph);
            }
        }
        out.loopCycles = k;
    }

    loop.assign((size_t)nch, {});
    for (int c = 0; c < nch; ++c) loop[c] = dsp::irfft(Y[c], (size_t)nOut);

    // --- 4. Level match to the analysis region ------------------------------
    double eIn = 0.0, eOut = 0.0;
    for (int c = 0; c < nch; ++c) {
        for (double v : raw[c]) eIn += v * v;
        for (double v : loop[c]) eOut += v * v;
    }
    eIn /= (double)nA;
    eOut /= (double)nOut;
    if (eOut <= 1e-30) {
        out.error = "The loop region is silent";
        return false;
    }
    const double gain = std::sqrt(eIn / eOut);
    for (auto& ch : loop)
        for (auto& v : ch) v *= gain;
    out.nLoopSamples = nOut;
    if (spectra) *spectra = std::move(Y);
    return true;
}

bool rebuildLoop(const AudioData& src, int64_t ls, int64_t le, const Params& p, Result& out,
                 std::vector<std::vector<double>>& loop) {
    return rebuildImpl(src, ls, le, p, out, loop, nullptr);
}

int64_t bestShift(const std::vector<std::vector<double>>& loop,
                  const std::vector<std::vector<double>>& ref) {
    std::vector<std::vector<dsp::Complex>> half(loop.size());
    for (size_t c = 0; c < loop.size(); ++c) half[c] = dsp::rfft(loop[c]);
    return sharedBestShift(half, loop.empty() ? 0 : loop[0].size(), ref);
}

bool build(const AudioData& src, int64_t ls, int64_t le, const Params& p, Result& out) {
    std::vector<std::vector<double>> loop;
    std::vector<std::vector<dsp::Complex>> Y;
    if (!rebuildImpl(src, ls, le, p, out, loop, &Y)) return false;
    const int nch = src.channels();
    const double sr = src.sampleRate;
    const int64_t nOut = out.nLoopSamples;

    // User loop gain (build() only: rebuildLoop() stays level-matched, its
    // callers apply their own). Doesn't change the rotation search.
    const double userGain = std::pow(10.0, (double)p.loopGainDb / 20.0);
    for (auto& ch : loop)
        for (auto& v : ch) v *= userGain;

    // --- 5. Splice with the attack (NeoLooper's Harmonic crossfade) ---------
    out.xfadeNRequested = (int64_t)std::llround(sr * (double)p.xfadeMs / 1000.0);
    // Only limited by the audio before ls: the loop is tiled under a
    // crossfade longer than itself (see tiledBlend).
    out.xfadeN = std::max<int64_t>(0, std::min<int64_t>(out.xfadeNRequested, ls));
    std::vector<std::vector<double>> ref((size_t)nch, std::vector<double>((size_t)out.xfadeN));
    for (int c = 0; c < nch; ++c)
        for (int64_t i = 0; i < out.xfadeN; ++i)
            ref[c][(size_t)i] = src.ch[c][(size_t)(ls - out.xfadeN + i)];
    out.shift = out.xfadeN > 0 ? sharedBestShift(Y, (size_t)nOut, ref) : 0;

    out.ch.resize((size_t)nch);
    float peak = 0.f;
    for (int c = 0; c < nch; ++c) {
        tiledBlend(loop[c], ref[c], out.shift, out.xfadeN, p.xfadeCurve, out.ch[c]);
        for (float v : out.ch[c].attackTail) peak = std::max(peak, std::fabs(v));
        for (float v : out.ch[c].loopRegion) {
            peak = std::max(peak, std::fabs(v));
            out.loopPeak = std::max(out.loopPeak, std::fabs(v));
        }
    }
    out.peak = peak;
    return true;
}

} // namespace altloop
