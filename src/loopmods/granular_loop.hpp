#pragma once

#include "granular_loop_dsp.hpp"
#include "loop_module.hpp"

// Granular: a long sustain loop, built in the time domain by chaining
// several "grains" of random length within [ls_, le_) joined by
// phase-aligned crossfades -- port of proto/granaular sample looping.py
// (Peter Eastman's method); the DSP core lives in granular_loop_dsp.hpp so
// it can be tested without ImGui.
//
// Unlike Harmonic/Freeze (a single pure period, exactly periodic -- can
// sound "electric" on long notes) or Rotation (reorders an ALREADY seamless
// loop into layers), Granular doesn't require [ls_, le_) to already be
// periodic: it reconstructs a loop body LONGER than the marked region, with
// real internal variation (each grain comes from a different point in the
// sustain itself), at the cost of the per-grain phase search being an
// approximation (single-degree-of-freedom WSOLA), not a mathematical
// guarantee like the phase vocoder. Uses audio from INSIDE [ls_, le_) only
// -- needs nothing before ls_ (same contract as Classic Crossfade Inside).
//
// The loop marked in the document (after Apply) is
// [ls_+headLen, ls_+headLen+nLoopSamples): the "head" (the first
// headLen=~50% of the marked region, as-is, unmodified) plays once as a
// transition, and the "body" (the rest, the already-blended grain chain) is
// what repeats. The body's own wrap closure (body.back()->body.front()) is
// guaranteed EXACT by construction (two adjacent samples of the original
// audio) -- see the detailed comment in granular_loop_dsp.hpp.
//
// preview_ concatenates head++body, same order as Apply; previewLoopOffset()
// tells the engine that only the body (from headLen onward) repeats forever
// -- same mechanism as Harmonic/Freeze/Rotation.
class GranularLoop : public LoopModule {
public:
    const char* name() const override { return "Granular"; }

protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;
    int64_t previewLoopOffset() const override { return lastHeadLen_; }

private:
    // Same reason as kMaxSeed in Rotation: ImGui::SliderInt loses precision
    // with huge ranges in 32-bit floating point.
    static constexpr int kMaxSeed = 9999;

    int nGrains_ = 10;          // 2-60, same as the script's num_segments=10
    float grainMinPct_ = 20.f;  // % of (le_-ls_), same as the script's width//5
    float grainMaxPct_ = 33.3f; // % of (le_-ls_), same as the script's width//3
    float headPct_ = 50.f;      // % of (le_-ls_) kept as "head" (plays once)
    int seed_ = -1;             // -1 = random on each render; 0..kMaxSeed = reproducible
    // No crossfade-curve slider: the blend is ALWAYS the reference script's
    // raised cosine (see granular_loop_dsp.cpp).

    // Result cached from the last render(): apply() reuses it AS-IS (doesn't
    // call buildGranularLoop again) so what gets printed into the document
    // is exactly what was heard in the preview -- with Seed=-1 every call
    // to buildGranularLoop draws different grains, so recomputing in
    // apply() would apply audio different from what was heard (a real bug,
    // reported with Seed=-1: "Apply" produced a click the preview didn't
    // have).
    bool lastOk_ = true;
    bool lastClamped_ = false;
    int64_t lastHeadLen_ = 0;
    int64_t lastNLoopSamples_ = 0;
    int64_t lastPeriod_ = 0;
    granulardsp::Result lastResult_;
};
