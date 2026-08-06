#pragma once

#include "loop_module.hpp"

// PingPong Harmonic: port of proto/mirror_loop.py. Requires a loop
// start/end already marked (unlike Freeze) and builds, from that material,
// a there-and-back "lap": the region's STFT frames are walked in the order
// [0..M-1, M-2..1] -- forward then backward -- so that ONLY the
// envelope/timbre's position in time plays in reverse; the synthesis phase
// always advances forward (identity phase-locking on the spectral peaks,
// see pingpong_harmonic_dsp.hpp), so the fine pitch/texture never sounds
// reversed -- it's not a plain audio "reverse". Each lap closes at an exact
// phase (same quantization as Harmonic, generalized over the whole lap), so
// the loop marked in the document (after Apply) is genuinely periodic.
//
// The marked loop is [ls_, ls_+oneLapSamples): ONE lap, and it's the only
// thing rendered into the document (laps=1 fixed toward the DSP; the laps
// would be identical to each other by construction, so rendering more would
// only lengthen the document without changing how the loop sounds).
//
// The entry crossfade blends the real ATTACK (audio before ls_) into the
// start of the first synthesized lap -- that's why Document::replaceRange
// in apply() starts at ls_-xfadeN, not at ls_ (same mechanism as Harmonic).
class PingPongHarmonicLoop : public LoopModule {
public:
    const char* name() const override { return "PingPong Harmonic"; }

protected:
    bool drawParams() override;
    std::shared_ptr<AudioData> render() override;
    void apply(Document& doc) override;
    int64_t previewRangeStart() const override { return ls_ - lastXfadeN_; }
    int64_t previewLoopOffset() const override { return lastXfadeN_; }

private:
    static constexpr int kFftSizes[7] = {128, 256, 512, 1024, 2048, 4096, 8192};
    int fftSizeIdx_ = 4;      // 2048, same as the script's default
    float xfadeMs_ = 30.f;
    float xfadeCurve_ = 0.5f; // 0 = linear, 1 = equal power

    // State cached from the last render(), used by previewRangeStart()/
    // previewLoopOffset() (called right after, same frame) and to show info
    // in drawParams() (at most one GUI frame out of date).
    bool lastOk_ = true;
    double lastSeamError_ = 0.0;
    int64_t lastXfadeN_ = 0;
    int64_t lastOneLapSamples_ = 0;
};
