#pragma once

#include "pitch_method.hpp"

// Loris (third_party/loris-2.0): transpose via additive resynthesis
// (sinusoidal partial tracking) instead of a phase vocoder -- much better
// preservation of timbre/transients than the other Pitch methods, but only
// for monophonic, quasi-harmonic material (a single melodic note); chords,
// percussion or noisy samples are better served by Pitch (built-in) or
// Pitch (Signalsmith). Always re-synthesizes (even at 0 semitones/0
// cents), so unlike them there's no bit-identical passthrough. See
// loris_dsp.hpp for the DSP core.
//
// Needs an expected fundamental frequency to seed the analysis. By default
// ("Auto" checkbox on) it's derived from the document's Root Note + Tune
// (same formula as harmonicdsp::noteFreq / the oscilloscope's trigger
// frequency), read fresh every frame -- not cached at open() time, same
// principle as PitchMethod's s0_/s1_ being recomputed fresh at Apply.
// Unchecking Auto unlocks a manual Hz field, for samples with no/wrong
// Root Note, or just to experiment.
class PitchLoris : public PitchMethod {
public:
    const char* name() const override { return "Pitch (Loris)"; }

protected:
    bool drawParams(const Document& doc) override;
    double lengthFactor() const override { return 1.0; } // always duration-preserving
    std::vector<std::vector<float>> render(const AudioData& src, int64_t s0, int64_t s1) override;

private:
    static double autoRefHz(const Document& doc);

    bool autoRef_ = true;       // Auto (Root Note + Tune); off = manual manualRefHz_ below
    float manualRefHz_ = 220.f; // only used when autoRef_ == false
    double refFreqHz_ = 220.0;  // resolved every frame in drawParams(), used by render()
};
