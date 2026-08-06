// Amplitude / pan / stereo-separation envelopes (Edison-style). Points with
// linear interpolation; drawn over the waveform and only alter the sample
// when the user hits "Apply".
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

struct EnvPoint {
    double frame = 0.0; // position in samples (double: sub-sample precision)
    float value = 0.f;
};

struct Envelope {
    std::vector<EnvPoint> pts; // sorted by frame
    float defaultValue = 0.f;
    float minValue = 0.f, maxValue = 1.f;

    bool empty() const { return pts.empty(); }

    void clear() { pts.clear(); }

    // true if applying it would change anything
    bool isActive() const {
        for (auto& p : pts)
            if (p.value != defaultValue) return true;
        return false;
    }

    float valueAt(double frame) const {
        if (pts.empty()) return defaultValue;
        if (frame <= pts.front().frame) return pts.front().value;
        if (frame >= pts.back().frame) return pts.back().value;
        // binary search for the segment
        size_t lo = 0, hi = pts.size() - 1;
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (pts[mid].frame <= frame) lo = mid; else hi = mid;
        }
        double x0 = pts[lo].frame, x1 = pts[hi].frame;
        if (x1 <= x0) return pts[hi].value;
        double t = (frame - x0) / (x1 - x0);
        return (float)(pts[lo].value + (pts[hi].value - pts[lo].value) * t);
    }

    // Inserts keeping order; returns the index of the new point.
    int addPoint(double frame, float value) {
        EnvPoint p{frame, std::min(maxValue, std::max(minValue, value))};
        auto it = std::upper_bound(pts.begin(), pts.end(), p,
            [](const EnvPoint& a, const EnvPoint& b) { return a.frame < b.frame; });
        int idx = (int)(it - pts.begin());
        pts.insert(it, p);
        return idx;
    }

    void movePoint(int idx, double frame, float value, double maxFrame) {
        if (idx < 0 || idx >= (int)pts.size()) return;
        double lo = (idx > 0) ? pts[idx - 1].frame : 0.0;
        double hi = (idx + 1 < (int)pts.size()) ? pts[idx + 1].frame : maxFrame;
        pts[idx].frame = std::min(hi, std::max(lo, frame));
        pts[idx].value = std::min(maxValue, std::max(minValue, value));
    }

    void removePoint(int idx) {
        if (idx >= 0 && idx < (int)pts.size()) pts.erase(pts.begin() + idx);
    }
};

// The editor's four envelopes.
struct Envelopes {
    Envelope amp;   // gain 0..2 (200%), default 1
    Envelope pan;   // -1 (L) .. +1 (R), default 0
    Envelope sep;   // stereo separation: -1 = wider, +1 = mono, default 0
    // Tuning in cents, +-1200 (one octave), default 0. Applied as varispeed
    // -- same mechanism as Tune/pitch-bend (Engine::process): local timing
    // shifts slightly along with pitch, which is why it's suited to aligning
    // momentary detuning without the "phasiness" of a duration-preserving
    // pitch shift.
    Envelope pitch;

    Envelopes() {
        amp.defaultValue = 1.f;   amp.minValue = 0.f;     amp.maxValue = 2.f;
        pan.defaultValue = 0.f;   pan.minValue = -1.f;    pan.maxValue = 1.f;
        sep.defaultValue = 0.f;   sep.minValue = -1.f;    sep.maxValue = 1.f;
        pitch.defaultValue = 0.f; pitch.minValue = -1200.f; pitch.maxValue = 1200.f;
    }

    bool anyActive() const {
        return amp.isActive() || pan.isActive() || sep.isActive() || pitch.isActive();
    }

    void clearAll() {
        amp.clear();
        pan.clear();
        sep.clear();
        pitch.clear();
    }
};
