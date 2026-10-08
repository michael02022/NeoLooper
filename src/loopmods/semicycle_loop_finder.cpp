#include "semicycle_loop_finder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace semicycledsp {

namespace {

constexpr float kSilenceDb = -100.f; // exact zero, and anything outside the sample
constexpr int kSigSlopes = 16;       // slopes per signature: n = i-7 .. i+8
constexpr int64_t kRefineFrames = 1000;

// Quantized to 16 bits first, so float input behaves like the 16-bit
// sources this was designed around (nothing between 0 and -90.3 dB).
float signedDb(float x) {
    long q = std::lround((double)x * 32768.0);
    q = std::clamp(q, -32768L, 32767L);
    if (q == 0) return kSilenceDb;
    const double u = (double)q / 32768.0;
    return (float)(q > 0 ? 20.0 * std::log10(u) : -20.0 * std::log10(-u));
}

struct Prepared {
    int64_t n = 0;
    std::vector<float> vl, vr;     // signed dB per channel (vr == vl for mono)
    std::vector<int32_t> sig;      // slope signature per frame
    std::vector<int64_t> semiStart; // first frame of each half-cycle
    std::vector<int64_t> semiOf;   // half-cycle containing frame i, i in [0,N]
};

Prepared prepare(const AudioData& audio) {
    Prepared p;
    p.n = audio.frames();
    const int64_t n = p.n;
    const auto& L = audio.ch[0];
    const auto& R = audio.ch.size() > 1 ? audio.ch[1] : audio.ch[0];

    p.vl.resize((size_t)n);
    p.vr.resize((size_t)n);
    std::vector<float> mid((size_t)n);
    for (int64_t i = 0; i < n; ++i) {
        p.vl[(size_t)i] = signedDb(L[(size_t)i]);
        p.vr[(size_t)i] = &R == &L ? p.vl[(size_t)i] : signedDb(R[(size_t)i]);
        mid[(size_t)i] = 0.5f * (p.vl[(size_t)i] + p.vr[(size_t)i]);
    }
    auto midAt = [&](int64_t i) {
        return (i < 0 || i >= n) ? kSilenceDb : mid[(size_t)i];
    };

    // slope[n] = sign(mid[n] - mid[n-1]) for n in [-7, N+8], stored at
    // offset +7. The signature of frame i packs slopes i+8 (weight 3^0)
    // down to i-7 (weight 3^15) as a balanced base-3 number, which fits
    // an int32 (|sum| <= (3^16-1)/2).
    const int64_t off = kSigSlopes / 2 - 1; // 7
    std::vector<int8_t> slope((size_t)(n + kSigSlopes));
    for (int64_t k = -off; k <= n + off + 1; ++k) {
        const float d = midAt(k) - midAt(k - 1);
        slope[(size_t)(k + off)] = (int8_t)((d > 0.f) - (d < 0.f));
    }
    p.sig.resize((size_t)n);
    for (int64_t i = 0; i < n; ++i) {
        int32_t s = 0, w = 1;
        for (int q = 0; q < kSigSlopes; ++q, w *= 3)
            s += w * slope[(size_t)(i + 8 - q + off)];
        p.sig[(size_t)i] = s;
    }

    // Half-cycles: a new one opens at every STRICT sign change, alternating
    // between looking for >0 and for <0 -- so a mid that touches exactly 0
    // and goes back doesn't count as a crossing.
    p.semiOf.resize((size_t)n + 1);
    p.semiStart.push_back(0);
    bool wantPositive = !(n > 0 && mid[0] > 0.f);
    int64_t k = 0;
    for (int64_t i = 0; i < n; ++i) {
        const float m = mid[(size_t)i];
        if (wantPositive ? (m > 0.f) : (m < 0.f)) {
            if (i > 0) {
                p.semiStart.push_back(i);
                ++k;
            }
            wantPositive = !wantPositive;
        }
        p.semiOf[(size_t)i] = k;
    }
    p.semiOf[(size_t)n] = k;
    return p;
}

class Search {
public:
    Search(const Prepared& p, int64_t window) : p_(p), w_(window) {}

    // Scores (s,e) and keeps it if it ties or beats the best so far (ties
    // go to the later evaluation). Returns false once a perfect (0) match
    // has been found, i.e. the whole search should stop.
    bool consider(int64_t s, int64_t e) {
        ++res_.evaluations;
        if (s < 0 || e >= p_.n || s >= e) return true;
        const int32_t se = p_.sig[(size_t)e];
        if (se == 0 || p_.sig[(size_t)s] != se) return true;
        const double err = error(s, e);
        if (err <= best_) {
            best_ = err;
            res_.ok = true;
            res_.start = s;
            res_.end = e;
            res_.error = err;
        }
        return best_ > 0.0;
    }

    bool done() const { return res_.ok && best_ <= 0.0; }
    const Result& result() const { return res_; }

private:
    // Triangular-weighted mean |difference| of both sides of s vs e,
    // per channel, in signed dB. +inf as soon as it can't beat best_.
    double error(int64_t s, int64_t e) const {
        const auto& vl = p_.vl;
        const auto& vr = p_.vr;
        int64_t w = std::min({w_, s, p_.n - e});
        double total = std::fabs((double)vl[(size_t)s] - vl[(size_t)e]) +
                       std::fabs((double)vr[(size_t)s] - vr[(size_t)e]);
        double count = 1.0;
        double mean = total;
        if (mean > best_) return kInf;
        for (int64_t k = 1; k < w; ++k) {
            const double wt = (double)(w - k) / (double)w;
            const size_t sl = (size_t)(s - k), el = (size_t)(e - k);
            const size_t sr = (size_t)(s + k), er = (size_t)(e + k);
            total += wt * (std::fabs((double)vl[sl] - vl[el]) + std::fabs((double)vr[sl] - vr[el]));
            total += wt * (std::fabs((double)vl[sr] - vl[er]) + std::fabs((double)vr[sr] - vr[er]));
            count += 2.0;
            mean = total / count;
            if (mean > best_) return kInf;
        }
        return mean;
    }

    static constexpr double kInf = std::numeric_limits<double>::infinity();
    const Prepared& p_;
    int64_t w_;
    double best_ = kInf;
    Result res_;
};

int64_t pctToFrames(int64_t n, double pct) {
    return std::clamp((int64_t)std::trunc((double)n * pct / 100.0), (int64_t)0, n);
}

} // namespace

Params shortDefaults() {
    Params p;
    p.mode = Mode::Short;
    p.regionStartPct = 25.0;
    p.regionEndPct = 100.0;
    p.cycles = 4;
    p.compareMs = 5.0;
    return p;
}

Params longDefaults() {
    Params p;
    p.mode = Mode::Long;
    p.regionStartPct = 0.0;
    p.regionEndPct = 100.0;
    p.minLengthPct = 2.0;
    p.maxLengthPct = 100.0;
    p.compareMs = 20.0;
    return p;
}

Result findLoop(const AudioData& audio, const Params& params) {
    const int64_t n = audio.frames();
    if (n < 2 || audio.ch.empty()) return {};

    int64_t a, b;
    if (params.useSelection) {
        a = std::clamp(params.selStart, (int64_t)0, n);
        b = std::clamp(params.selEnd, (int64_t)0, n);
    } else {
        a = pctToFrames(n, params.regionStartPct);
        b = pctToFrames(n, params.regionEndPct);
    }
    if (b <= a) return {};

    const Prepared p = prepare(audio);
    const int64_t ka = p.semiOf[(size_t)a];
    const int64_t kb = p.semiOf[(size_t)b];
    if (kb <= ka) return {};

    const int64_t window =
        std::max<int64_t>(0, (int64_t)std::trunc(audio.sampleRate * params.compareMs / 1000.0));
    Search search(p, window);

    if (params.mode == Mode::Short) {
        const int64_t span = 2 * (int64_t)std::max(1, params.cycles);
        for (int64_t j = kb; j > ka; --j) {
            const int64_t i = std::max<int64_t>(0, j - span);
            if (i >= j) continue;
            if (!search.consider(p.semiStart[(size_t)i], p.semiStart[(size_t)j])) break;
        }
        return search.result();
    }

    // Long mode, phase 1: every half-cycle pair with a length in range.
    int64_t minLen, maxLen;
    if (params.useSelection) {
        minLen = (int64_t)std::trunc(0.4 * (double)(b - a));
        maxLen = b - a;
    } else {
        const double regionPct = params.regionEndPct - params.regionStartPct;
        minLen = (int64_t)std::trunc((double)n * regionPct / 100.0 * params.minLengthPct / 100.0);
        maxLen = (int64_t)std::trunc((double)n * regionPct / 100.0 * params.maxLengthPct / 100.0);
    }
    minLen = std::clamp(minLen, (int64_t)0, n);
    maxLen = std::clamp(maxLen, (int64_t)0, n);

    const int64_t firstStart = p.semiStart[(size_t)ka];
    for (int64_t j = kb; j > ka && !search.done(); --j) {
        const int64_t e = p.semiStart[(size_t)j];
        if (p.sig[(size_t)e] == 0) continue;
        const int64_t from = std::max(e - maxLen, firstStart);
        const int64_t to = std::max(e - minLen, firstStart);
        const int64_t iFrom = p.semiOf[(size_t)from];
        const int64_t iTo = p.semiOf[(size_t)to];
        for (int64_t i = iTo; i > iFrom; --i) {
            if (i >= j) continue;
            if (!search.consider(p.semiStart[(size_t)i], e)) break;
        }
    }

    // Phase 2: keep the best length, slide the end frame by frame.
    if (search.result().ok && !search.done()) {
        const int64_t bestEnd = search.result().end;
        const int64_t len = bestEnd - search.result().start;
        for (int64_t e = bestEnd - kRefineFrames; e < bestEnd + kRefineFrames; ++e)
            if (!search.consider(e - len, e)) break;
    }
    return search.result();
}

} // namespace semicycledsp
