#pragma once

#include "loop_module.hpp"

// Ping Pong Classic: the historically-accurate construction (per the
// user's own research into how vintage hardware samplers/romplers, e.g.
// Roland's, actually built their ping-pong loops).
//
// R = mirror(normal[0,2h)), ONE raw, mirrored copy of a stretch of
// normal's own head -- SPLIT in half (never duplicated/re-mirrored) across
// two SEPARATE crossfades:
// 1. Attack crossfade (length h, attackLen()): ccinside::blendTail with
//    tail=the attack's own real tail (audio right before ls_, what gets
//    modified) and head=R's FIRST half (the part of R farthest from
//    normal[0]). Written entirely BEFORE ls_ -- normal's own head is
//    untouched by this one.
// 2. Loop-start crossfade (also length h): ccinside::blendTail with
//    tail=R's SECOND half (ending exactly on normal[0]) and head=normal's
//    own real head (what gets modified this time). Written entirely AFTER
//    ls_. R's two halves are ADJACENT samples of the same mirrored array,
//    so the seam at ls_ (between crossfade 1's last-used sample and
//    crossfade 2's first-used sample) stays natural, same "adjacent
//    original samples" principle ccinside::blendTail itself relies on at
//    its own single seam. Loop start (see below) sits at the MIDDLE of
//    this second half, not at ls_ itself.
// 3. Junction crossfade (length x1, pctJunction_), but only its FIRST
//    HALF is actually rendered here: normal's tail starts fading into
//    reversed's head -- same mechanism as Ping Pong Crossfade's own
//    junction crossfade, just cut short at its own midpoint (see below).
//
// Everything through step 3 above -- call it "pre" -- is built once, from
// the raw source. Loop end, with the "native ping-pong" checkbox on, sits
// exactly at pre's own end (the junction crossfade's true midpoint, where
// normal and reversed converge). Loop start sits at pre's OWN tail minus
// h/2 from that point... concretely: pre[loopStartOffset, preLen) is
// "porcion" -- the actual loop content. Reversed(porcion) is then
// APPENDED right after pre, mechanically (mirroring already-committed
// samples, not re-blending against raw source) -- so the wrap from the
// mirrored copy's own end back to loop start is bit-exact continuous
// (porcion[0] itself, not an approximation of it), and loop end, with the
// checkbox OFF (default), sits at the very end of this appended mirror --
// "compatible con forward looping" per the user's own words. This
// mechanical mirroring is what makes the OLD self-referential end
// crossfade this class used to have unnecessary -- continuity there is
// now exact by construction, not approximate, so that step was removed.
//
// Because step 1 blends INTO audio before ls_, this module's replaced
// range starts at ls_-h (not ls_) -- see previewRangeStart()/apply(). Loop
// start always stays at ls_+h/2 -- the exact MIDDLE of R's tail (R's
// SECOND half, step 2 above), not ls_ itself (which is only the boundary
// between R's two halves, not the middle of either one). Loop end has two
// possible positions (see nativePingPongEnd_ below), but never moves for a
// given mode -- only the audio around these markers changes -- which is
// what makes Ping Pong Classic a genuinely different construction from
// Ping Pong Crossfade / Ping Pong Crossfade (Inside), not just a renamed
// variant.
//
// nativePingPongEnd_: for samplers with their OWN native ping-pong/
// alternating loop type (true hardware reversal, no baked-in mirror
// needed), the mirrored/appended half doesn't matter for PLAYBACK --
// turning this on moves loop end to the exact MIDDLE of the junction
// crossfade itself (preLen() -- the "X" where normal and reversed
// converge), mirroring loop start's own position at the middle of R's
// blend, rather than to the very end of the mirrored construction. The
// hardware supplies the reverse pass itself; the full construction (pre +
// its mirrored tail) stays in the file regardless, as a reference/preview
// of the complete cycle (e.g. for exporting a WAV that also sounds right
// on non-ping-pong-capable players, using the OTHER, default loop end
// position instead).
class PingPongClassic : public LoopModule {
public:
    const char* name() const override { return "Ping Pong Classic"; }

protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;
    int64_t previewRangeStart() const override { return ls_ - attackLen(); }
    // Loop start sits at ls_+h/2 (document position, the middle of R's
    // tail -- see apply()) -- within preview_ (which starts at ls_-h),
    // that's offset h+h/2.
    int64_t previewLoopOffset() const override { return attackLen() + attackLen() / 2; }

private:
    float pctJunction_ = 10.f; // % of the MAXIMUM possible blend at the junction AND R's half-width
    float shapeJunction_ = 1.f;
    bool nativePingPongEnd_ = false;

    void renderInto(std::vector<std::vector<float>>& out) const;
    int64_t junctionLen() const; // x1
    int64_t attackLen() const;   // h -- half-width of the shared R (R = mirror(normal[0,2h)))
    // Length of "pre" (attack + loop-start crossfades + rest of normal +
    // HALF the junction crossfade) = h + (le_-ls_) - x1 + x1/2. Document
    // position ls_-h+preLen() is exactly le_-x1/2, the junction
    // crossfade's own midpoint -- loop end with nativePingPongEnd_ on.
    int64_t preLen() const;
};
