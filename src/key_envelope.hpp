// Piano preview envelope: an optional Hold + Decay (sustain always 0)
// applied to notes triggered from the on-screen piano, to hear how the
// sample will behave under a programmed amplitude envelope in a sampler and
// calibrate its own amplitude curve against it. Never touches the document.
//
// Time runs in real seconds from the key press (independent of the pitch
// the key plays at): 'holdSec' at full level, then the decay template over
// 'decaySec'. Templates (sustain 0, starting from full level):
//
//   * Linear      amplitude-linear ramp 1 -> 0 over the decay time.
//   * SF2         FluidSynth's volume envelope decay toward sustain 1000 cB
//                 (full attenuation): linear in dB, 0 -> -96 dB
//                 (FLUID_PEAK_ATTENUATION) over the decay time, then silence.
//                 (FluidSynth's release from full level is the same ramp, so
//                 there's no separate SF2 release template.)
//   * SFZ decay   sfizz's decay toward ampeg_sustain=0: exponential,
//                 exp(-9 t / T) (T at least 6 ms), until it falls under
//                 sustain + 0.001 (-60 dB, at ~0.77 T); from there a 50 ms
//                 linear fade to 0 (sfizz's free-running release with
//                 ampeg_release=0).
//   * SFZ release sfizz's release from full level: the same exponential
//                 until -80 dB (1e-4, at ~1.02 T), then its 50 ms linear
//                 fade-out.
#pragma once

#include <algorithm>
#include <cmath>

namespace keyenv {

enum class Curve { Linear = 0, Sf2, SfzDecay, SfzRelease };
constexpr int kCurveCount = 4;

inline const char* curveName(Curve c) {
    switch (c) {
    case Curve::Linear: return "Linear";
    case Curve::Sf2: return "SF2";
    case Curve::SfzDecay: return "SFZ (decay)";
    case Curve::SfzRelease: return "SFZ (release)";
    }
    return "?";
}

namespace detail {
constexpr double kSf2PeakDb = 96.0;           // FLUID_PEAK_ATTENUATION (960 cB)
constexpr double kSf2MinDecay = 0.001;        // -12000 timecents
constexpr double kSfzExp = 9.0;               // sfizz secondsToExpRate()
constexpr double kSfzMinTime = 6e-3;          // sfizz Default::offTime
constexpr double kSfzDecayCut = 0.001;        // sustain 0 + config::virtuallyZero
constexpr double kSfzReleaseCut = 1e-4;       // config::egReleaseThreshold
constexpr double kSfzFade = 50e-3;            // config::egTransitionTime
} // namespace detail

// Seconds the decay segment lasts until true silence (after the hold).
inline double decayLength(Curve c, double decaySec) {
    using namespace detail;
    switch (c) {
    case Curve::Linear: return std::max(decaySec, 0.0);
    case Curve::Sf2: return std::max(decaySec, kSf2MinDecay);
    case Curve::SfzDecay:
    case Curve::SfzRelease: {
        double T = std::max(decaySec, kSfzMinTime);
        double cut = c == Curve::SfzDecay ? kSfzDecayCut : kSfzReleaseCut;
        return T * std::log(1.0 / cut) / kSfzExp + kSfzFade;
    }
    }
    return 0.0;
}

// Linear gain t seconds after the key press. 'done' = the envelope reached
// silence for good (the preview can stop).
inline float gain(Curve c, double t, double holdSec, double decaySec, bool& done) {
    using namespace detail;
    done = false;
    if (t < holdSec) return 1.f;
    const double u = t - std::max(holdSec, 0.0);
    if (u >= decayLength(c, decaySec)) {
        done = true;
        return 0.f;
    }
    switch (c) {
    case Curve::Linear:
        return (float)(1.0 - u / decaySec); // decayLength > 0 here, so decaySec > 0
    case Curve::Sf2: {
        double T = decayLength(c, decaySec);
        return (float)std::pow(10.0, -kSf2PeakDb * (u / T) / 20.0);
    }
    case Curve::SfzDecay:
    case Curve::SfzRelease: {
        double T = std::max(decaySec, kSfzMinTime);
        double cut = c == Curve::SfzDecay ? kSfzDecayCut : kSfzReleaseCut;
        double tCut = T * std::log(1.0 / cut) / kSfzExp;
        if (u < tCut) return (float)std::exp(-kSfzExp * u / T);
        return (float)(cut * (1.0 - (u - tCut) / kSfzFade));
    }
    }
    return 0.f;
}

} // namespace keyenv
