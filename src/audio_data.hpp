// NeoLooper - in-RAM PCM audio data.
//
// The whole document lives in memory as planar float32. The "format" (16 or
// 32 bit) is Edison-style editing semantics: when the format is Int16 every
// destructive operation quantizes the result to the 16-bit grid, and if an
// edit produces amplitudes that don't fit in 16 bits the document is
// automatically promoted to 32 bits to avoid clipping.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

enum class SampleFormat { Int16, Float32 };

struct AudioData {
    // Planar audio: ch[0] = left (or mono), ch[1] = right.
    std::vector<std::vector<float>> ch;
    double sampleRate = 44100.0;
    SampleFormat format = SampleFormat::Float32;

    // Peak pyramid for drawing the waveform smoothly: min/max per kPeakStep
    // samples, per channel. Built once when publishing.
    static constexpr int kPeakStep = 64;
    struct Peaks {
        std::vector<float> mn, mx;
    };
    std::vector<Peaks> peaks;

    int channels() const { return (int)ch.size(); }
    int64_t frames() const { return ch.empty() ? 0 : (int64_t)ch[0].size(); }

    float at(int c, int64_t i) const {
        if (c >= channels() || i < 0 || i >= frames()) return 0.f;
        return ch[c][(size_t)i];
    }

    void buildPeaks() {
        peaks.clear();
        peaks.resize(ch.size());
        for (size_t c = 0; c < ch.size(); ++c) {
            const auto& v = ch[c];
            size_t n = (v.size() + kPeakStep - 1) / kPeakStep;
            peaks[c].mn.assign(n, 0.f);
            peaks[c].mx.assign(n, 0.f);
            for (size_t b = 0; b < n; ++b) {
                size_t i0 = b * kPeakStep;
                size_t i1 = std::min(v.size(), i0 + (size_t)kPeakStep);
                float mn = v[i0], mx = v[i0];
                for (size_t i = i0 + 1; i < i1; ++i) {
                    mn = std::min(mn, v[i]);
                    mx = std::max(mx, v[i]);
                }
                peaks[c].mn[b] = mn;
                peaks[c].mx[b] = mx;
            }
        }
    }

    float peakAbs() const {
        float p = 0.f;
        for (auto& v : ch)
            for (float s : v) p = std::max(p, std::fabs(s));
        return p;
    }
};

using AudioDataPtr = std::shared_ptr<const AudioData>;

// Maximum representable value in 16 bits (32767/32768).
inline constexpr float kInt16Max = 32767.0f / 32768.0f;

// Maximum representable value in 24 bits (8388607/8388608), same idea as
// kInt16Max -- used when exporting/scaling down to 24-bit PCM.
inline constexpr float kInt24Max = 8388607.0f / 8388608.0f;

inline float quantize16(float x) {
    float q = std::nearbyint(x * 32768.0f);
    q = std::min(32767.0f, std::max(-32768.0f, q));
    return q / 32768.0f;
}

inline const char* formatName(SampleFormat f) {
    return f == SampleFormat::Int16 ? "16-bit" : "32-bit";
}
