#pragma once

#include "loop_module.hpp"
#include "rotation_loop_dsp.hpp"

// Rotation: a "unison"/supersaw-style thickener built from a loop that's
// ALREADY MARKED and seamless -- requires loop start/end to be defined
// (unlike Freeze) and, unlike Harmonic, doesn't synthesize anything itself:
// it assumes [ls_, le_) is already a perfect loop (e.g. written by this
// program itself, or by Harmonic) and just reorders it. Port of
// proto/rotation_loop.py; the DSP core lives in rotation_loop_dsp.hpp so it
// can be tested without ImGui.
//
// The method, in order:
//   1. "Quality (layers)" (2-100): number of circularly rotated copies of
//      the marked loop summed together. Each copy is spectrally identical
//      (same waveform, just a different entry point), but since they're out
//      of phase with each other the mix sounds denser without losing volume
//      (auto-compensated level, equal-power).
//   2. "Seed" (-1..max): which rotation offsets are used. -1 = random on
//      each render; any other value = reproducible.
//   3. "Crossfade"/"Curve": blends the real ATTACK (audio before ls_) into
//      the start of the already-thickened loop -- the same rotation
//      technique aligned via circular correlation that Harmonic uses (see
//      spectral_loop_core.hpp), which is why Document::replaceRange in
//      apply() starts at ls_-xfadeN, not at ls_.
//   4. Last step: reuses, as-is, Classic Crossfade (Inside)'s processing
//      (and its same two sliders) over the already-thickened loop spliced
//      with the attack -- blends its own tail into its own head, and that
//      head then plays as attack (it's no longer part of the repeating
//      loop; the loop end doesn't move, see crossfade_inside_core.hpp).
//
// preview_ concatenates, in this order, the entry crossfade (step 3) + the
// head excluded by step 4 + the final loop with its tail already blended;
// previewLoopOffset() tells the engine that only that last part (from
// xfadeN+ccAvail onward) repeats forever -- same mechanism as
// Harmonic/Freeze.
class RotationLoop : public LoopModule {
public:
    const char* name() const override { return "Rotation"; }

protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;
    int64_t previewRangeStart() const override { return ls_ - lastXfadeN_; }
    int64_t previewLoopOffset() const override { return lastXfadeN_ + lastCcAvail_; }

private:
    // Ceiling for the seed slider. ImGui::SliderInt maps the drag with
    // 32-bit floating point arithmetic: with a range as large as INT32_MAX
    // it loses precision near the negative end and the slider gets stuck,
    // unable to pull away from negative values. 9999 is plenty for
    // practically unlimited seeds for everyday use
    // (rotationdsp::buildRotationLoop accepts int64_t all the same, without
    // this limit, in case a larger value ever needs to be typed by hand).
    static constexpr int kMaxSeed = 9999;

    int nLayers_ = 8;     // 2-100
    int seed_ = -1;       // -1 = random on each render; 0..kMaxSeed = reproducible
    float xfadeMs_ = 30.f;
    float xfadeCurve_ = 0.5f; // 0 = linear, 1 = equal power
    float ccPct_ = 25.f;      // Classic Crossfade (Inside), reused as the final step
    float ccShape_ = 1.f;     // same

    // Result cached from the last render(): apply() reuses it AS-IS (doesn't
    // call buildRotationLoop again) so what gets printed into the document
    // is exactly what was heard in the preview -- with Seed=-1 every call
    // to buildRotationLoop draws different layer offsets, so recomputing in
    // apply() would apply audio different from what was heard (same bug
    // Granular had, see that .hpp's comment; here it had gone uncorrected).
    bool lastOk_ = true;
    bool lastSafetyNormalized_ = false;
    int64_t lastXfadeN_ = 0;
    int64_t lastCcAvail_ = 0;
    rotationdsp::Result lastResult_;
};
