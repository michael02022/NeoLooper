// DSP core of the Filter module (non-loop processing, see src/loopmods/ for
// that), with no ImGui dependency so it can be tested from core_test.
// Replaces the previous graphic Equalizer (Envelope + FFT filtering): here
// filtering happens in the TIME DOMAIN with classic biquads (formulas from
// Robert Bristow-Johnson's Audio EQ Cookbook), a cascaded high-pass
// (adjustable slope, N stages of 12 dB/octave each, with Q/resonance on the
// first stage) and a bell band (peaking EQ with +/- gain: boost or cut,
// width via Q -- in practice, to add brightness to the highs, which was hard
// to get with a low-pass; replaces the previous 4 notches, which in practice
// went unused) -- much simpler and without the visualization/calibration
// issues of the previous STFT-based approach, since applying the filter
// needs no window or FFT at all, just the biquad's difference equation,
// sample by sample.
//
// The VISUALIZATION (analyzeStaticSpectrum) is a ONE-SHOT analysis (not
// live) of the chosen region, with the same dB calibration as the
// oscilloscope's live spectrum (see EditorUI::updateLiveSpectrum) -- an
// explicit user request, who found that view clearer than the previous
// attempt's per-frame point cloud. Being a single averaged curve (not loose
// per-frame points), there's no "where does it connect" heuristic that can
// go wrong.
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace filterdsp {

// Biquad in transposed Direct Form II (normalized coefficients, a0=1). The
// state (z1/z2) is part of the instance -- each channel needs its own copy
// so history doesn't mix between channels.
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    float process(float x) {
        const double y = b0 * (double)x + z1;
        z1 = b1 * (double)x - a1 * y + z2;
        z2 = b2 * (double)x - a2 * y;
        return (float)y;
    }
};

// Butterworth Q (maximally flat response) for each stage of the cascaded
// high-pass.
constexpr double kButterworthQ = 0.70710678118654752440;

// freq is clamped below Nyquist (0.49*sr) to avoid instability.
Biquad makeHighpass(double freq, double sr, double q);
// gainDb positive = boost (the band's main use case: brightness in the
// highs), negative = cut.
Biquad makePeaking(double freq, double sr, double q, double gainDb);

// 1..kMaxStages = number of cascaded stages in the high-pass (each one 12
// dB/oct of slope, with no interaction between stages -- cascades of stable
// sections stay stable no matter how many) -> 12/24/.../96 dB/oct.
// kMaxStages=8 (96 dB/oct) gives a "wall"-like slope: one octave past the
// cutoff it's already at -96dB, effectively inaudible. The filter's on/off
// is a separate flag (hpOn): turning it off does NOT lose the chosen
// slope/freq/Q.
constexpr int kMaxStages = 8;

struct FilterSpec {
    // The user's Q applies ONLY to the cascade's first stage (a single
    // resonance peak at the cutoff, synth-filter style); the extra stages
    // use Butterworth Q (0.707) and only add slope.
    // 0.707 = no resonance.
    bool hpOn = false;
    int hpStages = kMaxStages; // default slope: 96 dB/oct (wall)
    float hpFreq = 80.f;
    float hpQ = 1.f; // default resonance (0.707 = no resonance/Butterworth)

    // Bell band (peaking EQ): +/- gain around bandFreq, width via Q.
    // Replaces the previous low-pass and notches.
    bool bandOn = false;
    float bandFreq = 8000.f;
    float bandGainDb = 0.f;
    float bandQ = 1.f;
};

// Filters src[s0,s1) for each channel (same length as the input, the filter
// doesn't change duration). Uses up to 100ms of real audio before s0 as
// context to settle the biquad's state before it starts writing the result,
// avoiding a click at the splice with the rest of the document (same
// principle as the spectral modules' context, much cheaper here: no FFT at
// all, just a few tens of thousands of samples of recursive computation).
// Empty if s1<=s0 or there are no channels.
std::vector<std::vector<float>> apply(const AudioData& src, int64_t s0, int64_t s1,
                                      const FilterSpec& spec);

// Theoretical LINEAR gain of the COMBINED filter (high-pass + band active)
// at a given frequency -- for drawing the response curve over the real
// spectrum and seeing where the cuts/boosts land.
double responseAt(const FilterSpec& spec, double freq, double sr);

// Averaged spectrum (a single dB value per bin, NOT a per-frame point
// cloud) of src[s0,s1), over several overlapping STFT frames, subsampled if
// needed (bounds the work regardless of how long the region is). Same
// calibration as EditorUI::updateLiveSpectrum: ref = nFft/4 (0 dB lands
// around "full scale" with a Hann window, not "the loudest bin in this
// view").
struct SpectrumCurve {
    std::vector<float> db; // size nFft/2+1
    int nFft = 0;
    double sr = 44100.0;
};
void analyzeStaticSpectrum(const AudioData& src, int64_t s0, int64_t s1, int nFft,
                           int64_t maxFrames, SpectrumCurve& out);

} // namespace filterdsp
