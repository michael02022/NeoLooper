#pragma once

#include "loop_module.hpp"
#include "rotation_loop_dsp.hpp"

// Rotation: a "unison"/supersaw-style thickener built from the MARKED loop
// [ls_, le_) -- requires loop start/end to be defined (unlike Freeze) and,
// unlike Harmonic, doesn't synthesize anything itself: it reorders the
// loop's own audio. Port of proto/rotation_loop.py (reworked, see
// rotation_loop_dsp.hpp); the DSP core lives there so it can be tested
// without ImGui.
//
// The method, in order:
//   1. Optional, off by default ("Make seamless first"): Classic Crossfade
//      (Inside) (its same two sliders) over the marked loop FIRST: blends
//      its own tail into its own head, the head then plays as attack and
//      the loop that remains wraps seamlessly -- so every rotated layer,
//      and their sum, is seamless too (no blend is needed afterward). Off:
//      the marked loop is rotated as-is and must already be seamless.
//   2. "Quality (layers)" (2-100): number of circularly rotated copies of
//      that loop summed together. Each copy is spectrally identical (same
//      waveform, just a different entry point), but since they're out of
//      phase with each other the mix sounds denser; the sum is scaled to
//      exactly the loop's own energy per channel, keeping its DC.
//   3. "Seed" (-1..max): which rotation offsets are used. -1 = random on
//      each render; any other value = reproducible.
//   4. "Crossfade"/"Curve": blends the real ATTACK (audio before ls_) into
//      the start of the thickened loop, rotated to where it best correlates
//      with that attack (one rotation for all channels, or one each with
//      "Align channels separately") -- which is why Document::replaceRange
//      in apply() starts at ls_-xfadeN, not at ls_.
//
// preview_ concatenates, in this order, the entry crossfade (step 4) + the
// head excluded by step 1 + the final loop; previewLoopOffset() tells the
// engine that only that last part (from xfadeN+ccAvail onward) repeats
// forever -- same mechanism as Harmonic/Freeze.
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
    // Optional first step, off by default: Classic Crossfade (Inside) over
    // the marked loop so it's seamless before rotating (ccPct_ only counts
    // while useCcInside_ is on; off = the loop is rotated as marked).
    bool useCcInside_ = false;
    float ccPct_ = 25.f;
    float ccShape_ = 1.f;     // same
    float effectiveCcPct() const { return useCcInside_ ? ccPct_ : 0.f; }
    // Stereo: align each channel with its own attack instead of one shared
    // rotation (rotationdsp::buildRotationLoop's alignPerChannel).
    bool alignPerChannel_ = false;

    // Result cached from the last render(): apply() reuses it AS-IS (doesn't
    // call buildRotationLoop again) so what gets printed into the document
    // is exactly what was heard in the preview -- with Seed=-1 every call
    // to buildRotationLoop draws different layer offsets, so recomputing in
    // apply() would apply audio different from what was heard (same bug
    // Granular had, see that .hpp's comment; here it had gone uncorrected).
    bool lastOk_ = true;
    float lastPeak_ = 0.f;
    int64_t lastXfadeN_ = 0;
    int64_t lastCcAvail_ = 0;
    rotationdsp::Result lastResult_;
};
