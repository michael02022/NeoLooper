#pragma once

#include "loop_module.hpp"

// Classic Crossfade (Inside): a variant of Classic Crossfade that, instead
// of taking material from OUTSIDE the loop (the audio immediately before
// ls_, which Classic Crossfade only READS, never modifies), takes material
// from INSIDE the loop itself -- the head [ls_, ls_+avail) serves as a read
// reference (just as untouched as the material before ls_ in the normal
// Classic Crossfade, only this time located INSIDE the original loop) to
// blend the loop's TAIL [le_-avail, le_), exactly like normal Classic
// Crossfade blends its tail with the audio before ls_. The difference is
// that, since the reference head is still part of the original loop, it has
// to be removed from the region that repeats so it isn't heard twice: the
// loop start shifts to ls_+avail (the loop end does NOT move) and that head
// plays only once, as an attack, before entering the loop -- already
// shortened by 'avail' samples. Since the head and tail come from the same
// original loop and can't overlap each other, the blend is capped at at
// most half the loop's duration. Useful when there's no -- or you don't want
// to use -- audio before the loop start: unlike the normal variant (which
// ALWAYS needs audio before ls_), this one never depends on anything outside
// [ls_, le_).
class ClassicCrossfadeInside : public LoopModule {
public:
    const char* name() const override { return "Classic Crossfade (Inside)"; }

protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;
    // The previewed/applied loop starts at ls_+fadeLen(), not at ls_ (the
    // reference head is excluded from the loop, see above).
    int64_t previewRangeStart() const override { return ls_ + fadeLen(); }

private:
    float pct_ = 25.f;   // % of the MAXIMUM possible blend (half the loop)
    float shape_ = 1.f;  // 0 = equal gain, 1 = equal power
    int64_t fadeLen() const;
    // Blended tail (fadeLen() samples): replaces [le_-avail, le_) in the
    // document. Doesn't include the head (that stays intact, only READ).
    void blendTail(std::vector<std::vector<float>>& out) const;
};
