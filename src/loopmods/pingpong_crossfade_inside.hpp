#pragma once

#include "loop_module.hpp"

// Ping Pong Crossfade (Inside): same as Ping Pong Crossfade, but the
// "traditional" crossfade at the ends (the one that makes the wrap between
// the end of the doubled loop and the loop start continuous) uses material
// from INSIDE the loop itself as its reference instead of the audio before
// ls_ -- the same change Classic Crossfade Inside makes over Classic
// Crossfade, applied only to that crossfade; the normal<->reversed junction
// crossfade (pctJunction_) is untouched.
//
// The reference is the loop's head [ls_, ls_+x2) (normal audio, not
// reversed or blended) -- just as untouched as the material before ls_ in
// the normal variant, only located inside the original loop. Since that
// head is still real audio that can't be altered, it has to be removed from
// the region that repeats (otherwise it would sound twice): the loop start
// advances to ls_+x2 (that head then plays once, as an attack) and the end
// of the doubled loop (ls_+total, the equivalent of "le_" for this
// construction) doesn't move. x2 is capped at the duration of the
// unreversed normal part (L-x1), which also always fits within half of
// outLength(), so the reference head never overlaps the tail being blended.
class PingPongCrossfadeInside : public LoopModule {
public:
    const char* name() const override { return "Ping Pong Crossfade (Inside)"; }

protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;
    int64_t previewRangeStart() const override { return ls_ + endLen(); }

private:
    float pctJunction_ = 10.f; // % of the MAXIMUM possible blend at the junction
    float shapeJunction_ = 1.f;
    float pctEnd_ = 25.f;      // % of the MAXIMUM possible blend at the ends
    float shapeEnd_ = 1.f;
    int64_t junctionLen() const;
    int64_t endLen() const;
    int64_t outLength() const; // total length of v, head included
    // Builds the full v, with the head [0, endLen()) still present and
    // untouched -- apply() needs it to keep it in the document as an
    // attack; render() only previews v[endLen(), outLength()).
    void renderFull(std::vector<std::vector<float>>& out) const;
};
