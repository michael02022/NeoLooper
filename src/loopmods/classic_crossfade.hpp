#pragma once

#include "loop_module.hpp"

// Classic Crossfade: takes a percentage of the audio immediately BEFORE the
// loop start and blends it over the end of the loop. By wrapping end ->
// start, the blended material lands exactly on the loop start sample, so the
// loop ends up continuous (the classic loop-crossfade technique).
class ClassicCrossfade : public LoopModule {
public:
    const char* name() const override { return "Classic Crossfade"; }

protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;

private:
    float pct_ = 25.f;   // % of the MAXIMUM possible blend (see fadeLen)
    float shape_ = 1.f;  // 0 = equal gain, 1 = equal power
    // Blend duration in samples. pct_ is a percentage of the longest blend
    // achievable (limited by the loop's duration and by the audio available
    // before the loop start), not a fixed percentage of the loop's
    // duration -- so 100% on the slider always corresponds to the longest
    // possible blend, with no dead zone at the end of the range.
    int64_t fadeLen() const;
    void renderInto(std::vector<std::vector<float>>& out) const;
};
