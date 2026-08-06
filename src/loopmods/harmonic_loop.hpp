#pragma once

#include "loop_module.hpp"

// Harmonic: "wavetable-style" spectral loop (port of proto/freeze_loop.py,
// subcommand "loop" / build_seamless_loop; the DSP core lives in
// harmonic_loop_dsp.hpp so it can be tested without ImGui). Quantizes each
// STFT bin's instantaneous frequency (phase vocoder) to an integer number of
// cycles within the loop's duration, forcing every bin back to its exact
// starting phase after one period -> the result is genuinely periodic (like
// one wavetable cycle), and since any circular rotation of a period is an
// equally valid entry point, a search (via circular correlation) finds the
// rotation that best aligns with the real waveform right before the loop
// start, to blend the real attack into the start of the synthesized loop
// instead of a hard cut.
//
// The loop marked in the document (after Apply) is [ls_, ls_+nLoopSamples):
// a pure period, perfectly cyclic by construction at its end->start wrap.
// The entry crossfade (--xfade-ms/--xfade-curve) blends the real ATTACK
// (audio before ls_, in [ls_-xfadeN, ls_)) into the start of that period --
// that's why Document::replaceRange in apply() starts at ls_-xfadeN, not at
// ls_ (the only loop module that touches audio before the loop start).
//
// preview_ includes THAT span (blended attack + loop, in that order) so the
// crossfade is heard and seen live. The engine does NOT loop preview_
// wholesale: previewLoopOffset() tells it that only [xfadeN, frames()) --
// the pure period -- repeats forever, and the blended attack before that
// point plays once when entering preview (same mechanism as the attack
// preceding a marked loop in normal playback, see Engine::publishPreview).
// So the preview is exactly what gets applied to the document, with no
// extra smoothing or artifacts specific to the editing loop.
//
// MIDI note mode (noteMode_, "MIDI Note" checkbox): instead of quantizing
// nLoopSamples to the hop grid (multiples of nFft/4, unrelated to any
// pitch), it quantizes to an integer number of cycles of the document's root
// note (same lockPeriod mechanism as Freeze's exact-cycle mode, see
// harmonic_loop_dsp.hpp). Unlike Freeze, Harmonic does NOT fold those cycles
// down to a single bit-identical one: it keeps the k full cycles that fit in
// [ls_,le_), each with its natural overlap-add variation -- it's still the
// usual long, "alive" loop, just now tuned to an exact note (useful for
// multi-note samples, where the loop shouldn't beat against the real pitch
// of the key that triggers it).
class HarmonicLoop : public LoopModule {
public:
    const char* name() const override { return "Harmonic"; }

protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;
    int64_t previewRangeStart() const override { return ls_ - lastXfadeN_; }
    int64_t previewLoopOffset() const override { return lastXfadeN_; }

private:
    static constexpr int kFftSizes[9] = {128,  256,  512,   1024, 2048,
                                         4096, 8192, 16384, 32768};
    int fftSizeIdx_ = 4;      // 2048, same as the script's default
    float xfadeMs_ = 30.f;
    float xfadeCurve_ = 0.5f; // 0 = linear, 1 = equal power
    // MIDI note mode: anchors the phase quantization to the harmonic series
    // of the document's root note (same mechanism as Freeze's exact-cycle
    // mode, specloop::synthesize's lockPeriod) instead of the hop grid.
    // Unlike Freeze, it does NOT fold to a single cycle: it keeps the k full
    // cycles that fit in [ls_,le_) -- still a long, "alive" loop, just with
    // its period tuned to an exact note.
    bool noteMode_ = false;
    // -99..99, fine adjustment of the target pitch (root note + cents), same
    // role as in Freeze. REcalibrated to -docTuneCents_ every time the
    // module opens (same reason: the document's tuning is a CORRECTION, the
    // real content is mistuned the other way).
    float tuneCents_ = 0.f;
    int lastRootNote_ = 60; // detects root note changes while the window is open

    // State cached from the last render(), used by previewRangeStart()/
    // previewLoopOffset() (called right after, same frame) and to show info
    // in drawParams() (which draws before that same frame's render() runs;
    // at most one GUI frame out of date).
    bool lastOk_ = true;
    double lastSeamError_ = 0.0;
    int64_t lastXfadeN_ = 0;
    int64_t lastNotePeriod_ = 0; // 0 if noteMode_==false
};
