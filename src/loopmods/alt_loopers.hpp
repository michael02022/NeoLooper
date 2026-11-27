#pragma once

#include "alt_loopers_dsp.hpp"
#include "loop_module.hpp"

// The Ensemble and Random Phase loop modules. See alt_loopers_dsp.hpp for
// the DSP (which also provides the analysis behind Freeze's Average option,
// see freeze_loop_dsp.hpp).
//
// Both share the same contract as Harmonic: they rebuild the marked
// loop [ls_, le_) into a seamless loop of nLoopSamples (a whole number of
// cycles of the root note) and blend the real
// attack into its start with NeoLooper's entry crossfade (Crossfade/Curve,
// rotation aligned by circular correlation) -- that's why apply() replaces
// from ls_-xfadeN and preview_ includes that span, with previewLoopOffset()
// telling the engine that only the pure loop after it repeats.
class AltLoopModule : public LoopModule {
protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;
    int64_t previewRangeStart() const override { return ls_ - lastXfadeN_; }
    int64_t previewLoopOffset() const override { return lastXfadeN_; }

    virtual altloop::Method method() const = 0;
    // Method-specific controls, drawn between the pitch and crossfade ones.
    virtual bool drawMethodParams() = 0;
    virtual void fillParams(altloop::Params& p) const = 0;
    // Random Phase without integer cycles doesn't depend on the pitch.
    virtual bool usesPitch() const { return true; }
    virtual bool usesSeed() const { return false; }

    // Same ceiling as Rotation's seed slider (see rotation_loop.hpp).
    static constexpr int kMaxSeed = 9999;

    // -99..99, fine adjustment of the root note's pitch, REcalibrated to
    // -docTuneCents_ every time the module opens (same as Harmonic/Freeze:
    // the document's tuning is a CORRECTION, the content is mistuned the
    // other way).
    float tuneCents_ = 0.f;
    int seed_ = -1;
    float xfadeMs_ = 30.f;
    float xfadeCurve_ = 0.5f; // 0 = linear, 1 = equal power
    float loopGainDb_ = 0.f;  // user level of the loop, dB over the region's RMS
    int lastRootNote_ = 60;   // detects root note changes while the window is open

    // Result cached from the last render(): apply() prints it AS-IS (with
    // Seed=-1 a rebuild would draw different random phases from the ones
    // that were heard, same reason as Rotation).
    bool lastOk_ = false;
    int64_t lastXfadeN_ = 0;
    altloop::Result lastResult_;
};

// oneBand = "Ensemble (1 band)": the same options, but a single width (or
// Voices' detune) for the whole spectrum instead of the two crossfaded
// bands -- handed to the DSP as identical low/high values.
class EnsembleLoop : public AltLoopModule {
public:
    explicit EnsembleLoop(bool oneBand = false) : oneBand_(oneBand) {}
    const char* name() const override { return oneBand_ ? "Ensemble (1 band)" : "Ensemble"; }

protected:
    altloop::Method method() const override { return altloop::Method::Ensemble; }
    bool drawMethodParams() override;
    void fillParams(altloop::Params& p) const override;
    bool usesSeed() const override { return true; }

private:
    // Illustration of the Low/High bands and the crossfade between them
    // (altloop::bandValue, the same curve the DSP applies) over an octave
    // axis, with the loop's harmonics marked: the width for Gaussian/Flat,
    // the detune (cents) for Voices.
    void drawBandCurve() const;

    const bool oneBand_;
    float width_ = 20.f;       // oneBand_: width for the whole spectrum (per mille)
    float detune_ = 12.f;      // oneBand_: Voices' detune for the whole spectrum (cents)
    float lowFreq_ = 100.f;
    float lowWidth_ = 0.f;
    float highFreq_ = 8000.f;
    float highWidth_ = 30.f;
    float floorPct_ = 0.f;
    int shape_ = 0;            // altloop::HarmonicShape
    int bandCurve_ = 0;        // altloop::BandCurve
    float bandBend_ = 0.f;     // -1..1, 0 = linear
    int voices_ = 7;
    float lowDetune_ = 10.f;   // cents
    float highDetune_ = 15.f;  // cents
    float inharmonicity_ = 0.f;
};

class RandomPhaseLoop : public AltLoopModule {
public:
    const char* name() const override { return "Random Phase"; }

protected:
    altloop::Method method() const override { return altloop::Method::RandomPhase; }
    bool drawMethodParams() override;
    void fillParams(altloop::Params& p) const override;
    bool usesPitch() const override { return integerCycles_; }
    bool usesSeed() const override { return true; }

private:
    float windowPct_ = 100.f;
    bool integerCycles_ = true;
    float randomPct_ = 100.f;
    bool decorrelate_ = false;
    float smoothHz_ = 0.f;
};
