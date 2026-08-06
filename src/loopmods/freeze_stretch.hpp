#pragma once

#include "loop_module.hpp"
#include "freeze_stretch_dsp.hpp"

// FreezeStretch: an "ambiance" effect discovered by pushing Signalsmith
// Stretch far past its own "clean stretch" range (see
// freeze_stretch_dsp.hpp for how). Two controls for the freeze itself:
// where in the document the grain is taken from, and how long the
// generated ambiance should last (finite, not an infinite loop) -- plus
// Classic Crossfade (Inside)'s own two controls, reused as the final
// step so the result is loop-ready by itself.
//
// The original attack (everything before the chosen position) is
// discarded: Apply replaces the WHOLE document with the generated,
// already-blended chunk and marks its loop directly, same convention as
// ClassicCrossfadeInside/RotationLoop.
class FreezeStretch : public LoopModule {
public:
    const char* name() const override { return "FreezeStretch"; }
    bool requiresLoopRegion() const override { return false; }

protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;
    bool showLoopRegionHeader() const override { return false; }
    // Only [lastLoopStart_, end) repeats during preview -- the crossfade's
    // own reference head plays once first, same idiom as Freeze/Rotation.
    int64_t previewLoopOffset() const override { return lastLoopStart_; }

private:
    float positionPct_ = 50.f; // 0..100, mapped to positionFrac 0..1
    float durationSec_ = 5.f;
    float ccPct_ = 25.f;  // Classic Crossfade (Inside), final step
    float ccShape_ = 1.f; // 0 = equal gain, 1 = equal power

    bool lastOk_ = true;
    int64_t lastLoopStart_ = 0;
    int64_t lastLoopEnd_ = 0;
};
