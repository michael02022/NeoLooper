#pragma once

#include "loop_module.hpp"

// Freeze: pauses the sample at a point and sustains a frozen chunk that
// OVERWRITES to the end of the sample -- the rest of the original sample
// isn't kept unprocessed; the frozen chunk becomes the end of the file.
// Port of proto/freeze_loop.py, subcommand "freeze" (spectral_freeze),
// method="naive" (explicit user request: doesn't use Harmonic's coherent
// phase vocoder -- it literally holds the analysis frame, with no phase
// advance, the "buzz" character described in freeze_loop.py). Also
// simplified relative to the script: doesn't require a marked loop/
// selection (the freeze point is its own slider, not [ls,le)), there's no
// automatic rotation search (the frozen chunk's offset/phase is manual) and
// there's no "exit" to blend (only the ENTRY is blended, real attack ->
// frozen chunk). Also includes manual tuning (tuneCents, -99..99) applied
// ONLY to the synthesized chunk (not the real attack): since method="naive"
// has no per-bin frequency to shift (all of them stay at 0 by design), it's
// tuned by resampling the already-synthesized period -- see
// spectral_loop_core.hpp/freeze_loop_dsp.hpp. The user handles by hand any
// other adjustment (fine phase, loop points) they want afterward.
//
// Unlike the rest of the modules, Freeze doesn't operate on [ls_, le_) (it
// never uses them: requiresLoopRegion()==false, EditorUI opens it with
// ls_=0/le_=frames() only to satisfy the LoopModule contract). Its real
// "region" is [freezeStart-xfadeN, frames()) -- from the entry crossfade to
// the end of the document -- computed on every render() from its own
// sliders; that's why previewRangeStart()/previewRangeEnd() are recomputed
// dynamically instead of using ls_/le_.
class FreezeLoop : public LoopModule {
public:
    const char* name() const override { return "Freeze"; }
    bool requiresLoopRegion() const override { return false; }

protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;
    bool showLoopRegionHeader() const override { return false; }
    int64_t previewRangeStart() const override { return lastFreezeStart_ - lastXfadeN_; }
    int64_t previewRangeEnd() const override { return src_->frames(); }
    int64_t previewLoopOffset() const override { return lastXfadeN_; }

private:
    static constexpr int kFftSizes[7] = {128, 256, 512, 1024, 2048, 4096, 8192};
    int fftSizeIdx_ = 4;       // 2048
    float freezePos_ = 0.5f;   // 0 = start of the sample, 1 = end
    float durationMs_ = 1000.f;
    float offsetFrac_ = 0.f;   // 0..1, manual rotation of the frozen chunk
    float xfadeMs_ = 30.f;
    float xfadeCurve_ = 0.5f;  // 0 = linear, 1 = equal power
    // -99..99, manual tuning of the synthesized chunk. REcalibrated to
    // -docTuneCents_ every time the module opens (see the dirty_ check at
    // the start of drawParams(): the document's tuning is a CORRECTION
    // applied over the sample, so the real content is mistuned in the
    // OPPOSITE direction -- flipping the sign makes the quantization target
    // start out already calibrated to the audio's real pitch, without the
    // user having to work it out by hand. It's still a free slider
    // afterward: the user can readjust it.
    float tuneCents_ = 0.f;
    // "Exact cycle" mode (Infinity's Freeze Looper): vocoder anchored to the
    // harmonic series of the document's root note, every cycle
    // bit-identical, and on Apply a loop of a SINGLE cycle gets marked
    // (minimal memory). Default ON (explicit user request): it's the
    // preferred mode for sustain loops, naive ("buzz") stays as a manual
    // alternative.
    bool cycleMode_ = true;
    int lastRootNote_ = 60; // detects root note changes while the window is open

    // State cached from the last render(), used by previewRangeStart()/
    // previewRangeEnd()/previewLoopOffset() (called right after, same
    // frame) and to show info in drawParams() (at most one GUI frame out of
    // date).
    bool lastOk_ = true;
    double lastSeamError_ = 0.0;
    int64_t lastFreezeStart_ = 0;
    int64_t lastXfadeN_ = 0;
    int64_t lastCyclePeriod_ = 0; // 0 in normal mode
    // FINAL chunk duration (post-tuning): sizes() doesn't know about
    // tuneCents (the tuning resample happens later, in buildFreeze), so the
    // info text uses this cached value instead of the cheap estimate.
    int64_t lastNLoopSamples_ = 0;
};
