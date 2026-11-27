// DSP core of Process > Exciter (no ImGui dependency, testable from
// core_test): a port of M/S Exciter by Kevin Kopczynski (MIT,
// https://github.com/KevinToodlepoot/MS-Exciter). Chain: input gain ->
// Mid/Side stage (Width, plus "Stereoize": a comb-filtered, delayed copy of
// the mid added to the side, which widens even mono) blended by M/S Mix ->
// 12 dB/oct high-pass (upstream: fixed at 2 kHz; here adjustable) -> Drive
// and Bias -> sigmoid + two Arraya waveshapers at 2x oversampling -> DC
// blocker -> level compensation -> Exciter Mix. Upstream's input/output gain
// and overall Mix aren't ported: the result is level-matched instead (below),
// and NeoLooper itself switches a 16-bit document to 32-bit float when a
// level no longer fits.
//
// Two changes so applying it doesn't change the volume:
//   * Exciter Mix ADDS the harmonics on top of the signal (upstream
//     crossfades the signal out against them, which drops everything below
//     the high-pass by up to 6 dB at the default 50%).
//   * Level matching: the result is scaled to the RMS the region had before
//     (one gain for all channels, so the stereo balance stays).
//
// Runs offline over the region with some context before it (the filters
// settle before the region starts) and, when the region doesn't reach the
// start/end of the sample, crossfades into/out of it over a few ms so its
// edges don't step.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace exciterdsp {

// Frequency controls span kMinHz .. maxHz(sampleRate): the whole spectrum,
// stopping a bit short of Nyquist (the biquads blow up there). Nothing is
// tied to a particular sample rate.
constexpr double kMinHz = 20.0;
inline double maxHz(double sampleRate) { return 0.45 * sampleRate; }
inline double clampHz(double hz, double sampleRate) {
    return std::clamp(hz, kMinHz, std::max(kMinHz, maxHz(sampleRate)));
}

struct MSSpec {
    float msMixPct = 100.f;  // 0..100: M/S stage vs. its input
    float excMixPct = 50.f;  // 0..100: harmonics added on top of the M/S stage
    float width = 1.f;       // 0..4, 1 = unchanged (stereo only)
    float stereoize = 0.f;   // 0..1 (stereo only)
    float hpHz = 2000.f;     // high-pass feeding the waveshapers (upstream: fixed 2 kHz)
    float drive = 1.f;       // 1..20
    float bias = 0.f;        // -1..1, asymmetric shaping (even harmonics)
};

// Returns [s0, s1) per channel, same length (empty if s1 <= s0 or no
// audio). A mono document stays mono: it runs as L = R and keeps the left
// result (Width/Stereoize have no side to act on there).
std::vector<std::vector<float>> applyMS(const AudioData& src, int64_t s0, int64_t s1,
                                        const MSSpec& spec);

constexpr double kContextSec = 0.05;
constexpr double kEdgeFadeSec = 0.005;

} // namespace exciterdsp
