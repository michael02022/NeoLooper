#pragma once

#include "loop_module.hpp"

// Ping Pong Crossfade: the loop portion is concatenated with its reversed
// copy. A crossfade smooths the join between the normal part and the
// reversed one, and a second "traditional" crossfade (with the audio before
// the loop start) smooths the end of the doubled loop. Each crossfade has
// its own equal gain <-> equal power curve.
class PingPongCrossfade : public LoopModule {
public:
    const char* name() const override { return "Ping Pong Crossfade"; }

protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;

private:
    float pctJunction_ = 10.f; // % of the MAXIMUM possible blend at the junction
    float shapeJunction_ = 1.f;
    float pctEnd_ = 25.f;      // % of the MAXIMUM possible blend at the end
    float shapeEnd_ = 1.f;
    void renderInto(std::vector<std::vector<float>>& out) const;
    // pctJunction_/pctEnd_ are percentages of the longest blend achievable at
    // each junction (see the comment on ClassicCrossfade::fadeLen), not a
    // fixed percentage of the loop's duration -- so 100% on the slider is
    // always the longest possible blend there.
    int64_t junctionLen() const;
    int64_t endLen() const;
    int64_t outLength() const;
};
