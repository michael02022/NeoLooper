// Tests of the Ensemble and Random Phase loop modules (altloop::build),
// Freeze's optional settings (Extras) and
// the HarmonicFreeze analysis behind its Average.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "../src/engine.hpp"
#include "../src/fft.hpp"
#include "../src/loopmods/alt_loopers_dsp.hpp"
#include "../src/loopmods/freeze_loop_dsp.hpp"
#include "../src/loopmods/rotation_loop_dsp.hpp"

static int fails = 0;
#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            printf("FALLO: %s\n", msg);           \
            ++fails;                              \
        } else {                                  \
            printf("ok: %s\n", msg);              \
        }                                         \
    } while (0)

// Stereo harmonic tone (6 harmonics, 1/h, slow decay); R = gainR * L.
static std::shared_ptr<AudioData> tone(double sr, double seconds, double f0, float gainR = 1.f) {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = sr;
    d->ch.resize(2);
    const int n = (int)(sr * seconds);
    for (auto& c : d->ch) c.resize(n);
    for (int i = 0; i < n; ++i) {
        const double t = i / sr;
        double v = 0.0;
        for (int h = 1; h <= 6; ++h)
            v += (1.0 / h) * std::sin(2.0 * M_PI * f0 * h * t + 0.3 * h) * std::exp(-t * 0.3);
        d->ch[0][i] = (float)(v * 0.4);
        d->ch[1][i] = (float)(v * 0.4) * gainR;
    }
    return d;
}

// buildFreeze's optional settings, positional for brevity.
static freezedsp::Extras ex(float averageMs, bool autoPhase, float loopGainDb = 0.f,
                            bool extendXfade = false, bool fractionalCycles = false) {
    freezedsp::Extras x;
    x.averageMs = averageMs;
    x.autoPhase = autoPhase;
    x.loopGainDb = loopGainDb;
    x.extendXfade = extendXfade;
    x.fractionalCycles = fractionalCycles;
    return x;
}

static double maxAdjacent(const std::vector<float>& v) {
    double m = 0.0;
    for (size_t i = 1; i < v.size(); ++i) m = std::max(m, (double)std::fabs(v[i] - v[i - 1]));
    return m;
}

// Wrap (end -> start) and splice (attack blend -> loop) no bigger than the
// loop's own largest sample-to-sample step.
template <class R> // altloop::Result or freezedsp::Result
static bool seamless(const R& r) {
    for (const auto& c : r.ch) {
        const auto& lr = c.loopRegion;
        const double lim = 1.05 * maxAdjacent(lr);
        if (std::fabs(lr.back() - lr.front()) > lim) return false;
        if (!c.attackTail.empty() && std::fabs(c.attackTail.back() - lr.front()) > lim) return false;
    }
    return true;
}

static int64_t peakBin(const std::vector<float>& v) {
    const auto X = dsp::rfft(std::vector<double>(v.begin(), v.end()));
    int64_t best = 1;
    for (size_t k = 1; k < X.size(); ++k)
        if (std::abs(X[k]) > std::abs(X[(size_t)best])) best = (int64_t)k;
    return best;
}

static double rms(const std::vector<float>& v, size_t from = 0, size_t n = 0) {
    if (n == 0) n = v.size() - from;
    double e = 0.0;
    for (size_t i = from; i < from + n; ++i) e += (double)v[i] * v[i];
    return std::sqrt(e / (double)n);
}

int main() {
    const double sr = 44100.0;
    const int note = 57; // A3, 220 Hz
    const double P = altloop::notePeriod(sr, note, 0.f);
    auto sig = tone(sr, 3.0, 220.0, 0.5f);
    const int64_t ls = (int64_t)sr, le = ls + (int64_t)(sr * 0.5);
    const double regionRms = rms(sig->ch[0], (size_t)ls, (size_t)(le - ls));

    altloop::Params base;
    base.rootNote = note;
    base.seed = 42;

    // --- HarmonicFreeze analysis (Freeze's Average) ---
    {
        altloop::Params p = base;
        p.method = altloop::Method::HarmonicFreeze;
        altloop::Result r;
        const bool ok = altloop::build(*sig, ls, le, p, r);
        CHECK(ok, "freeze: build ok");
        if (ok) {
            const int64_t k = (int64_t)std::floor((double)(le - ls) / P);
            CHECK(r.cycles == k && r.loopCycles == k, "freeze: every whole cycle of the region");
            CHECK(r.nLoopSamples == (int64_t)std::llround(k * P), "freeze: loop = round(k*P) samples");
            CHECK(peakBin(r.ch[0].loopRegion) == k, "freeze: fundamental exactly on bin k");
            CHECK(seamless(r), "freeze: seamless wrap and attack splice");
            CHECK(r.xfadeN == (int64_t)std::llround(sr * 0.03), "freeze: 30 ms entry crossfade");
            double err = 0.0;
            for (size_t i = 0; i < r.ch[0].loopRegion.size(); ++i)
                err = std::max(err, (double)std::fabs(r.ch[1].loopRegion[i] -
                                                       0.5f * r.ch[0].loopRegion[i]));
            CHECK(err < 1e-4, "freeze: stereo balance kept (R = 0.5 L)");
            const double lr = rms(r.ch[0].loopRegion);
            CHECK(std::fabs(lr / regionRms - 1.0) < 0.1, "freeze: level matched to the region");
        }

        p.loopCycles = 1;
        const bool ok1 = altloop::build(*sig, ls, le, p, r);
        CHECK(ok1 && r.nLoopSamples == (int64_t)std::llround(P) && r.loopCycles == 1,
              "freeze: single-cycle loop of round(P) samples");
        if (ok1) {
            CHECK(seamless(r), "freeze: single cycle seamless");
            CHECK(r.xfadeN == (int64_t)std::llround(sr * 0.03) && r.xfadeN > r.nLoopSamples,
                  "freeze: full 30 ms crossfade over a single-cycle loop");
            // End of the fade (gIn ~ 1): the generated cycles are the loop itself.
            const auto& c0 = r.ch[0];
            const int64_t n = r.nLoopSamples, x = r.xfadeN;
            double err = 0.0;
            for (int64_t j = x - 20; j < x - 1; ++j)
                err = std::max(err, (double)std::fabs(c0.attackTail[(size_t)j] -
                                                       c0.loopRegion[(size_t)(((j - x) % n + n) % n)]));
            CHECK(err < 0.02, "freeze: crossfade tail made of the loop's own cycles");
        }

        p.loopCycles = 7;
        const bool ok7 = altloop::build(*sig, ls, le, p, r);
        CHECK(ok7 && r.nLoopSamples == (int64_t)std::llround(7 * P) &&
                  peakBin(r.ch[0].loopRegion) == 7,
              "freeze: 7-cycle loop, fundamental on bin 7");
    }

    // --- Loop gain (Alt modules) ---
    {
        altloop::Params p = base;
        p.method = altloop::Method::HarmonicFreeze;
        altloop::Result a, b;
        const bool okA = altloop::build(*sig, ls, le, p, a);
        p.loopGainDb = -6.f;
        const bool okB = altloop::build(*sig, ls, le, p, b);
        double err = 0.0;
        if (okA && okB)
            for (size_t i = 0; i < a.ch[0].loopRegion.size(); ++i)
                err = std::max(err, std::fabs(b.ch[0].loopRegion[i] -
                                              std::pow(10.0, -6.0 / 20.0) * a.ch[0].loopRegion[i]));
        CHECK(okA && okB && err < 1e-5 && a.shift == b.shift,
              "alt: Loop gain -6 dB scales the loop, same phase");
    }

    // --- Ensemble ---
    {
        altloop::Params p = base;
        p.method = altloop::Method::Ensemble;
        p.lowWidth = p.highWidth = 0.f;
        altloop::Result r;
        const bool ok = altloop::build(*sig, ls, le, p, r);
        CHECK(ok, "ensemble: build ok");
        if (ok) {
            CHECK(seamless(r), "ensemble: seamless wrap and attack splice");
            const auto X = dsp::rfft(std::vector<double>(r.ch[0].loopRegion.begin(),
                                                         r.ch[0].loopRegion.end()));
            double eAll = 0.0, eHarm = 0.0;
            for (size_t b = 1; b < X.size(); ++b) {
                eAll += std::norm(X[b]);
                if ((int64_t)b % r.cycles == 0) eHarm += std::norm(X[b]);
            }
            CHECK(eHarm / eAll > 0.99, "ensemble: width 0 -> pure harmonics");
            const double lr = rms(r.ch[0].loopRegion);
            CHECK(std::fabs(lr / regionRms - 1.0) < 0.1, "ensemble: level matched to the region");
        }
        p.lowWidth = 50.f;
        p.highWidth = 200.f;
        p.floorPct = 10.f;
        altloop::Result a, b, c;
        const bool okA = altloop::build(*sig, ls, le, p, a);
        const bool okB = altloop::build(*sig, ls, le, p, b);
        p.seed = 7;
        const bool okC = altloop::build(*sig, ls, le, p, c);
        CHECK(okA && okB && a.ch[0].loopRegion == b.ch[0].loopRegion,
              "ensemble: same seed -> same loop");
        CHECK(okC && a.ch[0].loopRegion != c.ch[0].loopRegion, "ensemble: other seed -> other loop");
        if (okA) CHECK(seamless(a), "ensemble: wide harmonics + floor still seamless");

        altloop::Result shortR;
        CHECK(!altloop::build(*sig, ls, ls + (int64_t)(5 * P), p, shortR) && !shortR.error.empty(),
              "ensemble: fewer than 10 cycles -> error");
    }

    // --- Ensemble: shapes, inharmonicity, real noise floor ---
    {
        // Long region (~330 cycles): one bin = 220/330 Hz, fine enough for
        // detune and inharmonicity to land where expected.
        const int64_t leL = ls + (int64_t)(sr * 1.5);
        altloop::Params p = base;
        p.method = altloop::Method::Ensemble;
        p.lowWidth = p.highWidth = 0.f;
        auto spectrum = [](const altloop::Result& r) {
            const auto X = dsp::rfft(std::vector<double>(r.ch[0].loopRegion.begin(),
                                                         r.ch[0].loopRegion.end()));
            std::vector<double> pw(X.size());
            for (size_t b = 0; b < X.size(); ++b) pw[b] = std::norm(X[b]);
            return pw;
        };
        auto bandEnergy = [](const std::vector<double>& pw, int64_t c, int64_t half) {
            double e = 0.0;
            for (int64_t b = c - half; b <= c + half; ++b) e += pw[(size_t)b];
            return e;
        };
        auto harmonicFraction = [](const std::vector<double>& pw, int64_t k) {
            double all = 0.0, harm = 0.0;
            for (size_t b = 1; b < pw.size(); ++b) {
                all += pw[b];
                if ((int64_t)b % k == 0) harm += pw[b];
            }
            return harm / all;
        };

        altloop::Result rf;
        p.shape = altloop::HarmonicShape::Flat;
        const bool okF0 = altloop::build(*sig, ls, leL, p, rf);
        CHECK(okF0 && harmonicFraction(spectrum(rf), rf.cycles) > 0.99,
              "ensemble: Flat with width 0 -> pure harmonics");
        // 100 per mille: +-0.17 of the spacing, the bands of neighboring
        // harmonics don't touch.
        p.lowWidth = p.highWidth = 100.f;
        const bool okF = altloop::build(*sig, ls, leL, p, rf);
        if (okF) {
            // a = 0.3*k*sqrt(3) bins: even inside, nothing past the edge.
            const auto pw = spectrum(rf);
            const int64_t k = rf.cycles, c = 2 * k;
            const int64_t a = (int64_t)std::floor(0.1 * (double)k * std::sqrt(3.0));
            double lo = 1e300, hi = 0.0;
            for (int64_t j = -a + 2; j <= a - 2; ++j) {
                lo = std::min(lo, pw[(size_t)(c + j)]);
                hi = std::max(hi, pw[(size_t)(c + j)]);
            }
            const double inside = bandEnergy(pw, c, a + 1), wider = bandEnergy(pw, c, a + 20);
            CHECK(okF && hi / lo < 1.0 + 1e-6 && wider / inside < 1.0 + 1e-6,
                  "ensemble: Flat band is even and stops at +-width*sqrt(3)");
            CHECK(seamless(rf), "ensemble: Flat seamless");
        }

        p.shape = altloop::HarmonicShape::Voices;
        p.voices = 5;
        p.lowDetuneCents = p.highDetuneCents = 20.f;
        altloop::Result rv;
        const bool okV = altloop::build(*sig, ls, leL, p, rv);
        if (okV) {
            // Harmonic 3: voices 10 cents apart -> ~5.8 bins apart, outer at +-11.5.
            const auto pw = spectrum(rv);
            const int64_t k = rv.cycles, c = 3 * k;
            const double total = bandEnergy(pw, c, 14), center = bandEnergy(pw, c, 1);
            const double outer = c * (std::pow(2.0, 20.0 / 1200.0) - 1.0);
            const double edge = bandEnergy(pw, c + (int64_t)std::llround(outer), 1);
            CHECK(center / total > 0.15 && center / total < 0.25 && edge / total > 0.15,
                  "ensemble: Voices split each harmonic into N detuned voices");
            CHECK(seamless(rv), "ensemble: Voices seamless");
        }
        CHECK(okV, "ensemble: Voices build");

        // Voices detune follows the two bands: +-5 cents up to 300 Hz,
        // +-40 cents from 1 kHz.
        p.lowFreq = 300.f;
        p.lowDetuneCents = 5.f;
        p.highFreq = 1000.f;
        p.highDetuneCents = 40.f;
        altloop::Result rb;
        const bool okB = altloop::build(*sig, ls, leL, p, rb);
        if (okB) {
            const auto pw = spectrum(rb);
            const int64_t k = rb.cycles;
            // h=1 (220 Hz): outer voices ~0.95 bins away -> all within +-2.
            const double tight = bandEnergy(pw, k, 2) / bandEnergy(pw, k, 20);
            // h=6 (1320 Hz): outer voices at 6k*(2^(40/1200)-1) ~ 46 bins.
            const int64_t c6 = 6 * k;
            const int64_t off6 = (int64_t)std::llround(c6 * (std::pow(2.0, 40.0 / 1200.0) - 1.0));
            const double outer6 = bandEnergy(pw, c6 + off6, 1) / bandEnergy(pw, c6, off6 + 3);
            CHECK(tight > 0.95 && outer6 > 0.15,
                  "ensemble: Voices detune follows the Low/High bands");
        }
        CHECK(okB, "ensemble: band-detune Voices build");
        p.lowFreq = 100.f;
        p.highFreq = 8000.f;

        p.shape = altloop::HarmonicShape::Gaussian;
        p.lowWidth = p.highWidth = 0.f;
        p.inharmonicity = 0.001f;
        altloop::Result ri;
        const bool okI = altloop::build(*sig, ls, leL, p, ri);
        if (okI) {
            const auto pw = spectrum(ri);
            const int64_t k = ri.cycles;
            const int64_t moved = (int64_t)std::llround(6.0 * k * std::sqrt((1.0 + 0.001 * 36.0) / 1.001));
            CHECK(peakBin(ri.ch[0].loopRegion) == k, "ensemble: inharmonicity keeps the fundamental");
            CHECK(pw[(size_t)moved] > 1e4 * pw[(size_t)(6 * k)] && moved > 6 * k,
                  "ensemble: inharmonicity stretches harmonic 6 upward");
            CHECK(seamless(ri), "ensemble: inharmonic loop seamless");
        }
        CHECK(okI, "ensemble: inharmonicity build");

        // Real noise color: tone + low-passed noise. With Noise floor 100 %
        // the residual keeps the sample's low/high balance and level.
        auto noisy = std::make_shared<AudioData>(*sig);
        uint64_t st = 777;
        double lp = 0.0;
        for (size_t i = 0; i < noisy->ch[0].size(); ++i) {
            st = st * 6364136223846793005ULL + 1442695040888963407ULL;
            const double w = ((double)(st >> 11) / 9007199254740992.0 - 0.5);
            lp = 0.97 * lp + 0.03 * w;
            noisy->ch[0][i] += (float)(lp * 0.3);
            noisy->ch[1][i] = noisy->ch[0][i];
        }
        // Energy between the harmonics (> 0.3 f0 from any) in [f1, f2) Hz.
        auto residual = [&](const std::vector<float>& v, double f1, double f2) {
            const auto X = dsp::rfft(std::vector<double>(v.begin(), v.end()));
            const double hz = sr / (double)v.size();
            double e = 0.0;
            for (size_t b = 1; b < X.size(); ++b) {
                const double f = b * hz, d = std::fabs(f / 220.0 - std::round(f / 220.0));
                if (f >= f1 && f < f2 && d > 0.3) e += std::norm(X[b]);
            }
            return e / (double)v.size();
        };
        auto totalE = [](const std::vector<float>& v) {
            double e = 0.0;
            for (float x : v) e += (double)x * x;
            return e;
        };
        std::vector<float> region(noisy->ch[0].begin() + ls, noisy->ch[0].begin() + leL);
        p.inharmonicity = 0.f;
        p.floorPct = 100.f;
        altloop::Result rn, rq;
        const bool okN = altloop::build(*noisy, ls, leL, p, rn);
        p.floorPct = 0.f;
        const bool okQ = altloop::build(*noisy, ls, leL, p, rq);
        if (okN && okQ) {
            const auto& out = rn.ch[0].loopRegion;
            const double tiltSrc = residual(region, 200, 1500) / residual(region, 3000, 8000);
            const double tiltOut = residual(out, 200, 1500) / residual(out, 3000, 8000);
            const double lvlSrc = residual(region, 200, 8000) / totalE(region) * region.size();
            const double lvlOut = residual(out, 200, 8000) / totalE(out) * out.size();
            printf("   noise color low/high: source %.1f, loop %.1f; noise share: source %.4f, loop %.4f\n",
                   tiltSrc, tiltOut, lvlSrc, lvlOut);
            CHECK(std::fabs(10.0 * std::log10(tiltOut / tiltSrc)) < 3.0,
                  "ensemble: Noise floor keeps the sample's noise color (within 3 dB)");
            CHECK(std::fabs(10.0 * std::log10(lvlOut / lvlSrc)) < 3.0,
                  "ensemble: Noise floor 100% keeps the noise level (within 3 dB)");
            const double quiet = residual(rq.ch[0].loopRegion, 200, 8000) /
                                 totalE(rq.ch[0].loopRegion) * rq.ch[0].loopRegion.size();
            CHECK(quiet < 0.01 * lvlOut, "ensemble: Noise floor 0% -> no noise between harmonics");
            CHECK(seamless(rn), "ensemble: noisy loop seamless");
        }
        CHECK(okN && okQ, "ensemble: noise floor builds");
    }

    // --- Random Phase ---
    {
        altloop::Params p = base;
        p.method = altloop::Method::RandomPhase;
        altloop::Result r;
        const bool ok = altloop::build(*sig, ls, le, p, r);
        CHECK(ok && r.nLoopSamples == (int64_t)std::llround(r.cycles * P),
              "random phase: integer cycles -> round(k*P) samples");
        if (ok) {
            CHECK(seamless(r), "random phase: seamless wrap and attack splice");
            CHECK(peakBin(r.ch[0].loopRegion) == r.cycles, "random phase: fundamental on bin k");
        }

        p.integerCycles = false;
        const int64_t le2 = ls + 20000;
        const bool okF = altloop::build(*sig, ls, le2, p, r);
        CHECK(okF && r.nLoopSamples == 20000 && r.cycles == 0,
              "random phase: free mode keeps the region length");
        if (okF) {
            CHECK(std::llabs(peakBin(r.ch[0].loopRegion) - std::llround(20000 / P)) <= 1,
                  "random phase: free mode keeps the pitch");
            const double lr = rms(r.ch[0].loopRegion);
            const double rr = rms(sig->ch[0], (size_t)ls, 20000);
            CHECK(std::fabs(lr / rr - 1.0) < 0.1, "random phase: level matched to the region");
        }
    }

    // --- Random Phase: Amount, Stereo decorrelation, Smoothing ---
    {
        altloop::Params p = base;
        p.method = altloop::Method::RandomPhase;
        p.integerCycles = false;
        p.xfadeMs = 0.f;
        const int64_t le2 = ls + 20000;

        p.randomPct = 0.f;
        altloop::Result r0;
        const bool ok0 = altloop::build(*sig, ls, le2, p, r0);
        double err0 = 0.0;
        if (ok0)
            for (int64_t i = 0; i < 20000; ++i)
                err0 = std::max(err0, (double)std::fabs(r0.ch[0].loopRegion[(size_t)i] -
                                                        sig->ch[0][(size_t)(ls + i)]));
        CHECK(ok0 && err0 < 1e-4, "random phase: Amount 0% = the region as-is");

        auto corrLR = [](const altloop::Result& r) {
            const auto &a = r.ch[0].loopRegion, &b = r.ch[1].loopRegion;
            double ab = 0.0, aa = 0.0, bb = 0.0;
            for (size_t i = 0; i < a.size(); ++i) {
                ab += (double)a[i] * b[i];
                aa += (double)a[i] * a[i];
                bb += (double)b[i] * b[i];
            }
            return ab / std::sqrt(aa * bb);
        };
        // Broadband source (identical L/R noise): with thousands of bins the
        // L/R correlation converges to E[cos(dphi)] = sinc^2(amount), i.e.
        // ~0 fully decorrelated and (sin(0.3pi)/(0.3pi))^2 = 0.737 at 30 %.
        auto wide = std::make_shared<AudioData>();
        wide->sampleRate = sr;
        wide->ch.assign(2, std::vector<float>((size_t)(3 * sr)));
        uint64_t ws = 999;
        for (size_t i = 0; i < wide->ch[0].size(); ++i) {
            ws = ws * 6364136223846793005ULL + 1442695040888963407ULL;
            wide->ch[0][i] = wide->ch[1][i] =
                (float)(((double)(ws >> 11) / 9007199254740992.0 - 0.5) * 0.5);
        }
        p.randomPct = 100.f;
        altloop::Result shared, decor, half;
        const bool okS = altloop::build(*wide, ls, le2, p, shared);
        p.decorrelate = true;
        const bool okD = altloop::build(*wide, ls, le2, p, decor);
        p.randomPct = 30.f;
        const bool okH = altloop::build(*wide, ls, le2, p, half);
        const double expect30 = std::pow(std::sin(0.3 * M_PI) / (0.3 * M_PI), 2.0);
        if (okS && okD && okH)
            printf("   corr L/R: shared %.3f, decorrelated %.3f, 30%% %.3f (expected %.3f)\n",
                   corrLR(shared), corrLR(decor), corrLR(half), expect30);
        CHECK(okS && corrLR(shared) > 0.999, "random phase: shared phases keep L/R coherent");
        CHECK(okD && std::fabs(corrLR(decor)) < 0.05, "random phase: Stereo decorrelation decorrelates L/R");
        CHECK(okH && std::fabs(corrLR(half) - expect30) < 0.05,
              "random phase: decorrelation scales with Amount");
        if (okD) CHECK(seamless(decor), "random phase: decorrelated loop seamless");
        altloop::Result tonal;
        p.randomPct = 100.f;
        CHECK(altloop::build(*sig, ls, le2, p, tonal) && tonal.ch.size() == 2,
              "random phase: decorrelation on a tonal stereo sample");

        // Smoothing: a pure tone exactly on bin 500 spreads into the Hann
        // kernel's shape (half = round(50 Hz * 20000 / sr / 2) = 11 bins).
        auto pure = std::make_shared<AudioData>();
        pure->sampleRate = sr;
        pure->ch.assign(1, std::vector<float>((size_t)(3 * sr)));
        for (size_t i = 0; i < pure->ch[0].size(); ++i)
            pure->ch[0][i] = (float)(0.5 * std::sin(2.0 * M_PI * 500.0 * (double)i / 20000.0));
        altloop::Params ps = base;
        ps.method = altloop::Method::RandomPhase;
        ps.integerCycles = false;
        ps.windowPct = 0.f;
        ps.xfadeMs = 0.f;
        ps.smoothHz = 50.f;
        altloop::Result rs;
        const bool okSm = altloop::build(*pure, ls, le2, ps, rs);
        if (okSm) {
            const auto X = dsp::rfft(std::vector<double>(rs.ch[0].loopRegion.begin(),
                                                         rs.ch[0].loopRegion.end()));
            double kerr = 0.0;
            for (int j = 0; j <= 13; ++j) {
                const double want = j <= 11 ? std::sqrt(0.5 + 0.5 * std::cos(M_PI * j / 12.0)) : 0.0;
                kerr = std::max(kerr, std::fabs(std::abs(X[(size_t)(500 + j)]) / std::abs(X[500]) - want));
                kerr = std::max(kerr, std::fabs(std::abs(X[(size_t)(500 - j)]) / std::abs(X[500]) - want));
            }
            CHECK(kerr < 1e-3, "random phase: Smoothing follows the Hann kernel exactly");
        } else {
            CHECK(false, "random phase: Smoothing build");
        }

        // On noise, smoothing flattens the spiky long-FFT magnitudes.
        auto noise = std::make_shared<AudioData>();
        noise->sampleRate = sr;
        noise->ch.assign(1, std::vector<float>((size_t)(3 * sr)));
        uint64_t st = 12345;
        for (auto& v : noise->ch[0]) {
            st = st * 6364136223846793005ULL + 1442695040888963407ULL;
            v = (float)(((double)(st >> 11) / 9007199254740992.0 - 0.5) * 0.5);
        }
        auto spikiness = [](const altloop::Result& r) {
            const auto X = dsp::rfft(std::vector<double>(r.ch[0].loopRegion.begin(),
                                                         r.ch[0].loopRegion.end()));
            double m = 0.0, m2 = 0.0;
            int n = 0;
            for (size_t b = 200; b < 5000; ++b, ++n) {
                const double pw = std::norm(X[b]);
                m += pw;
                m2 += pw * pw;
            }
            m /= n;
            return std::sqrt(m2 / n - m * m) / m; // coefficient of variation
        };
        ps.windowPct = 100.f;
        ps.smoothHz = 0.f;
        altloop::Result rn0, rn1;
        const bool okN0 = altloop::build(*noise, ls, le2, ps, rn0);
        ps.smoothHz = 200.f;
        const bool okN1 = altloop::build(*noise, ls, le2, ps, rn1);
        if (okN0 && okN1)
            printf("   noise spikiness: off %.3f, 200 Hz %.3f\n", spikiness(rn0), spikiness(rn1));
        CHECK(okN0 && okN1 && spikiness(rn1) < 0.3 * spikiness(rn0),
              "random phase: Smoothing flattens a noise spectrum");
        if (okN1) CHECK(seamless(rn1), "random phase: smoothed loop seamless");

        // A tone survives a narrow smoothing.
        altloop::Params pt = base;
        pt.method = altloop::Method::RandomPhase;
        pt.smoothHz = 20.f;
        altloop::Result rt;
        CHECK(altloop::build(*sig, ls, le, pt, rt) && peakBin(rt.ch[0].loopRegion) == rt.cycles,
              "random phase: 20 Hz smoothing keeps the fundamental");
    }

    // --- Freeze: Average + Auto phase ---
    {
        // 2nd harmonic growing over time: a2(t) = 0.1 + 0.4 t. Pitch exactly
        // sr/200 so the exact-cycle period is a whole 200 samples.
        const double f0 = sr / 200.0;
        const float tune = (float)(1200.0 * std::log2(f0 / 220.0));
        auto ev = std::make_shared<AudioData>();
        ev->sampleRate = sr;
        ev->ch.resize(2);
        const int n = (int)(sr * 3.0);
        for (auto& c : ev->ch) c.resize(n);
        for (int i = 0; i < n; ++i) {
            const double t = i / sr;
            const double v = std::sin(2.0 * M_PI * f0 * t) +
                             (0.1 + 0.4 * t) * std::sin(2.0 * M_PI * 2.0 * f0 * t + 0.7);
            ev->ch[0][i] = ev->ch[1][i] = (float)(0.3 * v);
        }
        const double pos = 1.0 / 3.0; // ~1 s
        auto h2ratio = [](const std::vector<float>& cyc) {
            const auto X = dsp::rfft(std::vector<double>(cyc.begin(), cyc.begin() + 200));
            return std::abs(X[2]) / std::abs(X[1]);
        };

        freezedsp::Result plain, single, avg;
        freezedsp::buildFreeze(*ev, pos, 500.f, 0.f, 30.f, 0.5f, tune, 2048, true, note, plain);
        const bool okS = freezedsp::buildFreeze(*ev, pos, 500.f, 0.f, 30.f, 0.5f, tune, 2048,
                                                true, note, single, ex(0.f, false));
        CHECK(okS && single.ch[0].loopRegion == plain.ch[0].loopRegion &&
                  single.averagedCycles == 0,
              "freeze: Average off + Auto phase off = original behavior");
        const bool okA = freezedsp::buildFreeze(*ev, pos, 500.f, 0.f, 30.f, 0.5f, tune, 2048,
                                                true, note, avg, ex(400.f, true));
        CHECK(okA && avg.cyclePeriod == 200 && avg.averagedCycles == (int64_t)(sr * 0.4 / 200),
              "freeze: Average 400 ms -> every whole cycle of the section");
        if (okS && okA) {
            const auto& lr = avg.ch[0].loopRegion;
            bool identical = true;
            for (size_t i = 0; i + 200 < lr.size(); ++i) identical &= lr[i] == lr[i + 200];
            CHECK(identical, "freeze: averaged cycles still bit-identical");
            // Section 1.0..1.4 s -> mean a2 = 0.58; a single frame at 1 s -> 0.5.
            const double rA = h2ratio(lr), rS = h2ratio(single.ch[0].loopRegion);
            CHECK(std::fabs(rA - 0.58) < 0.02, "freeze: Average gives the section's mean timbre");
            CHECK(std::fabs(rS - 0.50) < 0.03, "freeze: single frame gives the timbre at the point");
        }

        // Auto phase: the crossfaded attack follows the original audio.
        auto attackErr = [&](const freezedsp::Result& r) {
            const auto& a = r.ch[0].attackTail;
            double e = 0.0;
            for (size_t j = 0; j < a.size(); ++j) {
                const double d = a[j] - ev->ch[0][(size_t)(r.freezeStart - r.xfadeN) + j];
                e += d * d;
            }
            return std::sqrt(e / (double)a.size());
        };
        // Auto phase must be as good as the best hand-picked offset.
        freezedsp::Result autoP, manual;
        const bool okP = freezedsp::buildFreeze(*ev, pos, 500.f, 0.f, 30.f, 0.5f, tune, 2048,
                                                true, note, autoP, ex(400.f, true));
        double bestManual = 1e9, worstManual = 0.0;
        for (int off = 0; off < 200; off += 2) {
            if (!freezedsp::buildFreeze(*ev, pos, 500.f, off / 200.f, 30.f, 0.5f, tune, 2048,
                                        true, note, manual, ex(400.f, false)))
                continue;
            bestManual = std::min(bestManual, attackErr(manual));
            worstManual = std::max(worstManual, attackErr(manual));
        }
        CHECK(okP && attackErr(autoP) <= bestManual * 1.05 && attackErr(autoP) < 0.5 * worstManual,
              "freeze: Auto phase matches the best manual offset for the entry crossfade");
        // Loop gain: only the frozen chunk is scaled; the start of the attack
        // blend (gIn = 0) stays the real audio.
        auto scaledBy = [](const freezedsp::Result& a, const freezedsp::Result& b, double g) {
            double err = 0.0;
            for (size_t i = 0; i < a.ch[0].loopRegion.size(); ++i)
                err = std::max(err, std::fabs(b.ch[0].loopRegion[i] - g * a.ch[0].loopRegion[i]));
            return err;
        };
        freezedsp::Result g0, g6, gm12;
        const bool ok0 = freezedsp::buildFreeze(*ev, pos, 500.f, 0.f, 30.f, 0.5f, tune, 2048,
                                                true, note, g0, ex(400.f, true, 0.f));
        const bool ok6 = freezedsp::buildFreeze(*ev, pos, 500.f, 0.f, 30.f, 0.5f, tune, 2048,
                                                true, note, g6, ex(400.f, true, 6.f));
        const bool ok12 = freezedsp::buildFreeze(*ev, pos, 500.f, 0.f, 30.f, 0.5f, 0.f, 2048,
                                                 false, note, gm12, ex(0.f, true, -12.f));
        CHECK(ok0 && ok6 && scaledBy(g0, g6, std::pow(10.0, 6.0 / 20.0)) < 1e-5 &&
                  g6.shift == g0.shift,
              "freeze: Loop gain +6 dB scales the frozen chunk, same phase");
        CHECK(ok6 && std::fabs(g6.ch[0].attackTail[0] -
                               ev->ch[0][(size_t)(g6.freezeStart - g6.xfadeN)]) < 1e-6,
              "freeze: Loop gain leaves the real attack untouched");
        CHECK(ok6 && std::fabs(g6.loopPeak / g0.loopPeak - std::pow(10.0, 6.0 / 20.0)) < 1e-4,
              "freeze: loopPeak follows Loop gain");
        CHECK(ok12, "freeze: Loop gain in naive mode");

        // extendXfade: minimal (single-cycle) chunk, 30 ms crossfade.
        const int64_t want = (int64_t)std::llround(sr * 0.03);
        auto tailIsLoop = [](const freezedsp::Result& r) {
            const auto& c0 = r.ch[0];
            const int64_t n = r.nLoopSamples, x = r.xfadeN;
            double err = 0.0;
            for (int64_t j = x - 20; j < x - 1; ++j)
                err = std::max(err, (double)std::fabs(c0.attackTail[(size_t)j] -
                                                       c0.loopRegion[(size_t)(((j - x) % n + n) % n)]));
            return err;
        };
        freezedsp::Result clip1, ext1, extN;
        const bool okC1 = freezedsp::buildFreeze(*ev, pos, 0.f, 0.f, 30.f, 0.5f, tune, 2048, true,
                                                 note, clip1, ex(400.f, true, 0.f, false));
        CHECK(okC1 && clip1.xfadeN <= clip1.nLoopSamples,
              "freeze: without extendXfade the crossfade is clipped to the chunk (original)");
        const bool okE1 = freezedsp::buildFreeze(*ev, pos, 0.f, 0.f, 30.f, 0.5f, tune, 2048, true,
                                                 note, ext1, ex(400.f, true, 0.f, true));
        CHECK(okE1 && ext1.nLoopSamples == 200 && ext1.xfadeN == want &&
                  (int64_t)ext1.ch[0].attackTail.size() == want &&
                  ext1.ch[0].loopRegion.size() == 200,
              "freeze: extendXfade -> full 30 ms fade, chunk still a single cycle");
        if (okE1) CHECK(tailIsLoop(ext1) < 0.02, "freeze: the fade is made of repeated cycles");
        const bool okEN = freezedsp::buildFreeze(*ev, pos, 0.f, 0.f, 30.f, 0.5f, 0.f, 2048, false,
                                                 note, extN, ex(0.f, true, 0.f, true));
        CHECK(okEN && extN.xfadeN == want && extN.xfadeN > extN.nLoopSamples &&
                  tailIsLoop(extN) < 0.02,
              "freeze: extendXfade in naive mode repeats the short chunk under the fade");
        int64_t sReq = 0, sXf = 0, sFs = 0, sN = 0;
        freezedsp::sizes(ev->frames(), sr, pos, 0.f, 30.f, 2048, sFs, sReq, sXf, sN, true);
        CHECK(sXf == want, "freeze: sizes() with extendXfade only limited by the audio before");

        freezedsp::Result naive;
        const bool okN = freezedsp::buildFreeze(*ev, pos, 500.f, 0.f, 30.f, 0.5f, 0.f, 2048,
                                                false, note, naive, ex(0.f, true));
        CHECK(okN, "freeze: Auto phase also works in naive mode");
        freezedsp::Result tooLong;
        CHECK(!freezedsp::buildFreeze(*ev, 0.99, 500.f, 0.f, 30.f, 0.5f, tune, 2048, true, note,
                                      tooLong, ex(400.f, true)),
              "freeze: Average section past the end -> fails");
    }

    // --- Freeze: fractional k-cycle loops (Extras::fractionalCycles) ---
    {
        // A3 = 220 Hz -> P = 200.45: one whole-sample cycle (200) is +3.9 ct sharp.
        const double pos = (double)ls / (double)(sig->frames() - 1);
        auto cents = [&](const freezedsp::Result& r) {
            return 1200.0 * std::log2(sr * (double)r.loopCycles / (double)r.cyclePeriod / 220.0);
        };
        freezedsp::Result one, many, avgMany;
        const bool ok1 = freezedsp::buildFreeze(*sig, pos, 0.f, 0.f, 30.f, 0.5f, 0.f, 2048, true,
                                                note, one, ex(0.f, true, 0.f, true, true));
        CHECK(ok1 && one.loopCycles == 1 && one.cyclePeriod == 200 &&
                  std::fabs(cents(one) - 3.93) < 0.05,
              "freeze frac: Duration 0 -> one whole-sample cycle, as before (+3.9 ct)");
        const bool okM = freezedsp::buildFreeze(*sig, pos, 100.f, 0.f, 30.f, 0.5f, 0.f, 2048, true,
                                                note, many, ex(0.f, true, 0.f, true, true));
        const int64_t k = (int64_t)std::llround(sr * 0.1 / P);
        CHECK(okM && many.loopCycles == k && many.cyclePeriod == (int64_t)std::llround(k * P) &&
                  many.nLoopSamples == many.cyclePeriod,
              "freeze frac: Duration 100 ms -> loop of k cycles in round(k*P) samples");
        if (ok1 && okM) {
            printf("   frac: 1 cycle %+.3f ct, %lld cycles %+.4f ct\n", cents(one),
                   (long long)many.loopCycles, cents(many));
            CHECK(std::fabs(cents(many)) < 0.1, "freeze frac: tuning error under 0.1 cent");
            CHECK(peakBin(many.ch[0].loopRegion) == k, "freeze frac: fundamental on bin k");
            CHECK(seamless(many), "freeze frac: seamless wrap and attack splice");
            // Same waveform: harmonic balance of the k-cycle loop = the single cycle's.
            auto ratio = [](const std::vector<float>& v, int64_t f) {
                const auto X = dsp::rfft(std::vector<double>(v.begin(), v.end()));
                return std::abs(X[(size_t)(2 * f)]) / std::abs(X[(size_t)f]);
            };
            const double r1 = ratio(one.ch[0].loopRegion, 1), rk = ratio(many.ch[0].loopRegion, k);
            CHECK(std::fabs(rk / r1 - 1.0) < 0.01, "freeze frac: same timbre as the single cycle");
        }
        const bool okA = freezedsp::buildFreeze(*sig, pos, 100.f, 0.f, 30.f, 0.5f, 0.f, 2048, true,
                                                note, avgMany, ex(300.f, true, 0.f, true, true));
        CHECK(okA && avgMany.loopCycles == k && std::fabs(cents(avgMany)) < 0.1 && seamless(avgMany),
              "freeze frac: also with Average");
        // Extras::loopCycles sets the count directly (the window's slider).
        for (int64_t want : {1, 3, 7, 150}) {
            freezedsp::Extras xc = ex(0.f, true, 0.f, true, true);
            xc.loopCycles = want;
            freezedsp::Result rc;
            const bool okc = freezedsp::buildFreeze(*sig, pos, 0.f, 0.f, 30.f, 0.5f, 0.f, 2048,
                                                    true, note, rc, xc);
            char msg[96];
            snprintf(msg, sizeof msg, "freeze frac: loopCycles=%lld -> %lld cycles in round(k*P)",
                     (long long)want, (long long)want);
            CHECK(okc && rc.loopCycles == want &&
                      rc.cyclePeriod == (want == 1 ? 200 : (int64_t)std::llround(want * P)) &&
                      peakBin(rc.ch[0].loopRegion) == want && seamless(rc),
                  msg);
        }
    }

    // --- Ensemble: shape of the crossfade between bands (altloop::bandShape) ---
    {
        using altloop::BandCurve;
        bool ends = true, monotonic = true, bipolarSym = true;
        for (auto mode : {BandCurve::Curve, BandCurve::Bipolar})
            for (double bend : {-1.0, -0.5, 0.0, 0.3, 1.0}) {
                ends &= std::fabs(altloop::bandShape(0.0, mode, bend)) < 1e-9 &&
                        std::fabs(altloop::bandShape(1.0, mode, bend) - 1.0) < 1e-9;
                double prev = -1.0;
                for (int i = 0; i <= 200; ++i) {
                    const double t = i / 200.0, g = altloop::bandShape(t, mode, bend);
                    monotonic &= g >= prev - 1e-12;
                    prev = g;
                    if (mode == BandCurve::Bipolar)
                        bipolarSym &= std::fabs(g + altloop::bandShape(1.0 - t, mode, bend) - 1.0) < 1e-9;
                }
            }
        CHECK(ends && monotonic, "band curve: every shape goes 0 -> 1, monotonic");
        CHECK(bipolarSym, "band curve: Bipolar is point-symmetric");
        bool linear = true;
        for (int i = 0; i <= 10; ++i) {
            linear &= std::fabs(altloop::bandShape(i / 10.0, BandCurve::Curve, 0.0) - i / 10.0) < 1e-9;
            linear &= std::fabs(altloop::bandShape(i / 10.0, BandCurve::Bipolar, 0.0) - i / 10.0) < 1e-9;
        }
        CHECK(linear, "band curve: Bend 0 = linear in both modes");
        CHECK(altloop::bandShape(0.5, BandCurve::Curve, 0.5) > 0.6 &&
                  altloop::bandShape(0.5, BandCurve::Curve, -0.5) < 0.4,
              "band curve: Curve right = convex, left = concave");
        auto midSlope = [](double bend) {
            return (altloop::bandShape(0.55, BandCurve::Bipolar, bend) -
                    altloop::bandShape(0.45, BandCurve::Bipolar, bend)) / 0.1;
        };
        CHECK(midSlope(0.6) > 1.5 && midSlope(-0.6) < 0.5,
              "band curve: Bipolar right = S (steep middle), left = plateau in the middle");

        // bandValue: the bands themselves don't move, only the way across.
        const double f1 = 100.0, f2 = 10000.0, mid = 1000.0; // mid = halfway in octaves
        bool bandsFixed = true;
        for (auto mode : {BandCurve::Curve, BandCurve::Bipolar})
            for (double bend : {-0.8, 0.0, 0.8}) {
                bandsFixed &= altloop::bandValue(50.0, f1, 10.0, f2, 50.0, mode, bend) == 10.0;
                bandsFixed &= altloop::bandValue(f1, f1, 10.0, f2, 50.0, mode, bend) == 10.0;
                bandsFixed &= altloop::bandValue(f2, f1, 10.0, f2, 50.0, mode, bend) == 50.0;
                bandsFixed &= altloop::bandValue(15000.0, f1, 10.0, f2, 50.0, mode, bend) == 50.0;
            }
        CHECK(bandsFixed, "band curve: the low and high bands keep their values");
        CHECK(std::fabs(altloop::bandValue(mid, f1, 10.0, f2, 50.0) - 30.0) < 1e-9 &&
                  altloop::bandValue(mid, f1, 10.0, f2, 50.0, BandCurve::Curve, 0.7) > 40.0 &&
                  altloop::bandValue(mid, f1, 10.0, f2, 50.0, BandCurve::Curve, -0.7) < 20.0,
              "band curve: Bend moves the crossfade between the bands");

        // In Ensemble: the band curve reaches the harmonics' widths/detune.
        altloop::Params p = base;
        p.method = altloop::Method::Ensemble;
        p.lowFreq = 200.f;
        p.lowWidth = 0.f;
        p.highFreq = 2000.f;
        p.highWidth = 200.f;
        altloop::Result lin, late;
        const int64_t leL = ls + (int64_t)(sr * 1.5);
        const bool okL = altloop::build(*sig, ls, leL, p, lin);
        p.bandBend = -1.f; // concave: harmonics between the bands stay narrow
        const bool okC = altloop::build(*sig, ls, leL, p, late);
        if (okL && okC) {
            // Harmonic 3 (660 Hz, inside the crossfade): energy on its exact
            // bin is higher when the crossfade is concave (narrower there).
            auto onBin = [](const altloop::Result& r, int64_t b) {
                const auto X = dsp::rfft(std::vector<double>(r.ch[0].loopRegion.begin(),
                                                             r.ch[0].loopRegion.end()));
                double e = 0.0;
                for (int64_t j = -40; j <= 40; ++j) e += std::norm(X[(size_t)(b + j)]);
                return std::norm(X[(size_t)b]) / e;
            };
            CHECK(onBin(late, 3 * late.cycles) > 2.0 * onBin(lin, 3 * lin.cycles),
                  "band curve: Ensemble applies it (concave keeps mid harmonics narrower)");
            CHECK(seamless(late), "band curve: loop stays seamless");
        }
        CHECK(okL && okC, "band curve: Ensemble builds");
    }

    // --- Ensemble (1 band): equal low/high values = one value everywhere ---
    {
        for (auto shape : {altloop::HarmonicShape::Gaussian, altloop::HarmonicShape::Voices}) {
            altloop::Params a = base;
            a.method = altloop::Method::Ensemble;
            a.shape = shape;
            a.lowWidth = a.highWidth = 40.f;
            a.lowDetuneCents = a.highDetuneCents = 12.f;
            altloop::Params b = a;
            b.lowFreq = 500.f;
            b.highFreq = 900.f;
            b.bandCurve = altloop::BandCurve::Bipolar;
            b.bandBend = -1.f;
            altloop::Result ra, rb;
            const bool ok = altloop::build(*sig, ls, le, a, ra) && altloop::build(*sig, ls, le, b, rb);
            CHECK(ok && ra.ch[0].loopRegion == rb.ch[0].loopRegion,
                  shape == altloop::HarmonicShape::Voices
                      ? "ensemble 1 band: one detune -> band settings don't matter"
                      : "ensemble 1 band: one width -> band settings don't matter");
        }
    }

    // --- Monitoring volume (Engine::setMonitorGain) ---
    {
        // Constant DC preview: after the ramps settle the output is exactly
        // level * monitoring gain, clamped at 1 while boosting.
        Engine engine;
        engine.activate(44100.0);
        auto ad = std::make_shared<AudioData>();
        ad->sampleRate = 44100.0;
        ad->ch.assign(2, std::vector<float>(4000, 0.05f));
        ad->buildPeaks();
        engine.publishDocument(ad);
        engine.publishPreview(ad);
        engine.play();
        auto settle = [&](float gain) {
            engine.setMonitorGain(gain);
            float outL[64], outR[64];
            for (int b = 0; b < 200; ++b) { // ~0.3 s: both ramps settled
                for (int i = 0; i < 64; ++i) outL[i] = outR[i] = 0.f;
                engine.process(nullptr, nullptr, outL, outR, 64);
            }
            const float scope = engine.scopeSample(engine.scopeCounter() - 1);
            return std::make_pair(outL[63], scope);
        };
        const auto unity = settle(1.f);
        const auto x10 = settle(10.f);
        const auto x40 = settle(40.f);
        const auto back = settle(1.f);
        printf("   monitoring: unity %.4f, x10 %.4f (scope %.4f), x40 %.4f, back %.4f\n",
               unity.first, x10.first, x10.second, x40.first, back.first);
        CHECK(std::fabs(unity.first - 0.05f) < 1e-4, "monitoring: unity leaves the output as-is");
        CHECK(std::fabs(x10.first - 0.5f) < 1e-3 && std::fabs(x10.second - 0.5f) < 1e-3,
              "monitoring: +20 dB boosts the output and the oscilloscope");
        CHECK(std::fabs(x40.first - 1.f) < 1e-6, "monitoring: boosted output clamped at 0 dBFS");
        CHECK(std::fabs(back.first - 0.05f) < 1e-4 && ad->ch[0][0] == 0.05f,
              "monitoring: back to unity, the audio itself never changed");
    }

    // --- Rotation: seamless, exact level, DC, alignment ---
    {
        // Mono tone of 200-sample cycles; region of 100.25 cycles: its wrap jumps.
        auto tone200 = [&](double dc, int nch, int rDelay) {
            auto d = std::make_shared<AudioData>();
            d->sampleRate = sr;
            d->ch.resize((size_t)nch);
            const int n = (int)(sr * 2.0);
            for (auto& c : d->ch) c.resize((size_t)n);
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < nch; ++c) {
                    const int j = i - (c == 1 ? rDelay : 0);
                    double v = dc;
                    for (int h = 1; h <= 6; ++h) v += std::sin(2.0 * M_PI * h * j / 200.0 + h) / h;
                    d->ch[(size_t)c][(size_t)i] = (float)(0.3 * v * (c == 1 ? 0.6 : 1.0));
                }
            return d;
        };
        const int64_t rs = (int64_t)sr, re = rs + 20050;
        auto mono = tone200(0.0, 1, 0);
        double smooth = 0.0;
        for (size_t i = 1; i < mono->ch[0].size(); ++i)
            smooth = std::max(smooth, (double)std::fabs(mono->ch[0][i] - mono->ch[0][i - 1]));
        auto jumps = [&](const std::vector<float>& v) {
            int n = 0;
            for (size_t i = 0; i < v.size(); ++i)
                n += std::fabs(v[(i + 1) % v.size()] - v[i]) > 1.5 * smooth;
            return n;
        };
        int worstJumps = 0;
        for (int N : {2, 4, 16, 64}) {
            rotationdsp::Result r;
            rotationdsp::buildRotationLoop(*mono, rs, re, N, 5, 30.f, 0.5f, 25.f, 1.f, r);
            worstJumps = std::max(worstJumps, jumps(r.ch[0].loopBlended));
        }
        CHECK(worstJumps == 0, "rotation: a loop that clicks comes out seamless (no jump, any layer count)");
        rotationdsp::Result r0;
        rotationdsp::buildRotationLoop(*mono, rs, re, 4, 5, 30.f, 0.5f, 0.f, 1.f, r0);
        CHECK(jumps(r0.ch[0].loopBlended) > 1,
              "rotation: without Crossfade (inside) the click is spread into one per layer");

        // Exact level per channel: identical for every seed and layer count.
        auto stereo = tone200(0.0, 2, 37);
        auto rmsAC = [](const std::vector<float>& v) {
            double m = 0.0, e = 0.0;
            for (float x : v) m += x;
            m /= (double)v.size();
            for (float x : v) e += (x - m) * (x - m);
            return std::sqrt(e / (double)v.size());
        };
        double lo[2] = {1e9, 1e9}, hi[2] = {0.0, 0.0};
        for (int N : {2, 4, 8, 32})
            for (int seed = 0; seed < 10; ++seed) {
                rotationdsp::Result r;
                rotationdsp::buildRotationLoop(*stereo, rs, re, N, seed, 30.f, 0.5f, 25.f, 1.f, r);
                for (int c = 0; c < 2; ++c) {
                    const double v = rmsAC(r.ch[(size_t)c].loopBlended);
                    lo[c] = std::min(lo[c], v);
                    hi[c] = std::max(hi[c], v);
                }
            }
        const double region0 = rmsAC(std::vector<float>(stereo->ch[0].begin() + rs, stereo->ch[0].begin() + re));
        printf("   rotation level: L %.4f..%.4f, R %.4f..%.4f (region L %.4f)\n", lo[0], hi[0], lo[1],
               hi[1], region0);
        CHECK(20.0 * std::log10(hi[0] / lo[0]) < 0.01 && 20.0 * std::log10(hi[1] / lo[1]) < 0.01,
              "rotation: level independent of seed and layer count (< 0.01 dB)");
        CHECK(std::fabs(20.0 * std::log10(lo[0] / region0)) < 0.5,
              "rotation: level matches the marked loop");

        // DC kept as in the loop, not multiplied.
        auto withDc = tone200(0.2, 1, 0);
        const auto& wx = withDc->ch[0];
        double dcIn = 0.0;
        for (int64_t i = rs; i < re; ++i) dcIn += wx[(size_t)i];
        dcIn /= (double)(re - rs);
        bool dcOk = true;
        for (int N : {4, 32}) {
            rotationdsp::Result r;
            rotationdsp::buildRotationLoop(*withDc, rs, re, N, 1, 30.f, 0.5f, 25.f, 1.f, r);
            double m = 0.0;
            for (float v : r.ch[0].loopBlended) m += v;
            m /= (double)r.ch[0].loopBlended.size();
            dcOk &= std::fabs(m - dcIn) < 0.01;
        }
        CHECK(dcOk, "rotation: the loop's DC is kept (not multiplied by the layers)");

        // Alignment: L/R are the same noise, R 50 samples later. One shared
        // rotation keeps that 50-sample offset; per channel it may change.
        auto nz = std::make_shared<AudioData>();
        nz->sampleRate = sr;
        nz->ch.assign(2, std::vector<float>((size_t)(2 * sr)));
        uint64_t st = 4242;
        std::vector<float> w((size_t)(2 * sr) + 50);
        for (auto& v : w) {
            st = st * 6364136223846793005ULL + 1442695040888963407ULL;
            v = (float)(((double)(st >> 11) / 9007199254740992.0 - 0.5) * 0.5);
        }
        for (size_t i = 0; i < nz->ch[0].size(); ++i) {
            nz->ch[0][i] = w[i + 50];
            nz->ch[1][i] = w[i];
        }
        auto lagLR = [](const rotationdsp::Result& r) {
            const auto &L = r.ch[0].loopBlended, &R = r.ch[1].loopBlended;
            const int64_t n = (int64_t)L.size();
            int best = 0;
            double bv = -1e300;
            for (int s2 = 0; s2 < 200; ++s2) {
                double c = 0.0;
                for (int64_t i = 0; i < n; ++i) c += (double)L[(size_t)i] * R[(size_t)((i + s2) % n)];
                if (c > bv) { bv = c; best = s2; }
            }
            return best;
        };
        rotationdsp::Result sh, pc;
        rotationdsp::buildRotationLoop(*nz, rs, re, 8, 3, 30.f, 0.5f, 25.f, 1.f, sh, false);
        rotationdsp::buildRotationLoop(*nz, rs, re, 8, 3, 30.f, 0.5f, 25.f, 1.f, pc, true);
        printf("   rotation L/R lag: shared %d, per channel %d (source 50)\n", lagLR(sh), lagLR(pc));
        CHECK(lagLR(sh) == 50, "rotation: shared alignment keeps the L/R relationship");
        CHECK((sh.ch[0].loopBlended != pc.ch[0].loopBlended ||
               sh.ch[1].loopBlended != pc.ch[1].loopBlended) && lagLR(pc) != 50,
              "rotation: 'Align channels separately' aligns each channel on its own");
    }

    // --- Edge cases ---
    {
        altloop::Params p = base;
        p.method = altloop::Method::HarmonicFreeze;
        altloop::Result r;
        const bool ok = altloop::build(*sig, 100, 100 + (int64_t)(sr * 0.2), p, r);
        CHECK(ok && r.xfadeN == 100 && r.xfadeN < r.xfadeNRequested,
              "crossfade limited by the audio before the loop");
        CHECK(!altloop::build(*sig, ls, ls + (int64_t)P, p, r) && !r.error.empty(),
              "loop shorter than two cycles -> error");
        auto mono = std::make_shared<AudioData>();
        mono->sampleRate = sr;
        mono->ch.push_back(sig->ch[0]);
        CHECK(altloop::build(*mono, ls, le, p, r) && r.ch.size() == 1, "mono input");
        auto silent = std::make_shared<AudioData>(*mono);
        std::fill(silent->ch[0].begin(), silent->ch[0].end(), 0.f);
        CHECK(!altloop::build(*silent, ls, le, p, r), "silent region -> error");
    }

    printf(fails ? "\n%d TESTS FALLARON\n" : "\nTODOS LOS TESTS OK\n", fails);
    return fails ? 1 : 0;
}
