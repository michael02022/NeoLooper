#include "harmonic_loop.hpp"

#include <algorithm>

#include "imgui.h"

#include "harmonic_loop_dsp.hpp"

namespace {
// blended attack + pure period, in that order: this is the content used by
// both the preview (with previewLoopOffset()==xfadeN) and Apply.
std::vector<std::vector<float>> assemble(const harmonicdsp::Result& res, int nch) {
    std::vector<std::vector<float>> out(nch);
    for (int c = 0; c < nch; ++c) {
        auto& v = out[c];
        v.reserve((size_t)(res.xfadeN + res.nLoopSamples));
        v.insert(v.end(), res.ch[c].attackTail.begin(), res.ch[c].attackTail.end());
        v.insert(v.end(), res.ch[c].loopRegion.begin(), res.ch[c].loopRegion.end());
    }
    return out;
}
} // namespace

bool HarmonicLoop::drawParams() {
    bool changed = false;
    // Freshly opened (dirty_ is only true on the first drawParams() after
    // open(), see loop_module.hpp): calibrates tuning to the document's,
    // sign inverted -- same criterion as Freeze (see the comment in the
    // .hpp).
    if (dirty_) tuneCents_ = std::clamp(-docTuneCents_, -99.f, 99.f);

    static const char* kSizeLabels[] = {"128",  "256",  "512",   "1024", "2048",
                                        "4096", "8192", "16384", "32768"};
    changed |= ImGui::Combo("FFT size", &fftSizeIdx_, kSizeLabels, 9);

    changed |= ImGui::Checkbox("MIDI Note", &noteMode_);
    ImGui::SetItemTooltip(
        "Anchors the phase quantization to the harmonic series of the "
        "document's root note (same mechanism as Freeze's exact-cycle mode) "
        "instead of the hop grid -- the loop stays long and multi-cycle as "
        "usual, but tuned to an exact note. Useful for multi-note samples: "
        "the loop won't beat against the key's real pitch. Set the root "
        "note to roughly the sample's real pitch.");
    if (noteMode_ && docRootNote_ != lastRootNote_) changed = true;
    lastRootNote_ = docRootNote_;
    if (noteMode_) {
        changed |= ImGui::SliderFloat("Tune", &tuneCents_, -99.f, 99.f, "%.1f cents",
                                      ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip(
            "Fine adjustment of the TARGET pitch (root note + cents) the "
            "harmonic series anchors to. Starts calibrated to the "
            "document's tuning (sign inverted: the real content is mistuned "
            "the opposite way from the correction that parameter applies).");
    }

    changed |= ImGui::SliderFloat("Crossfade", &xfadeMs_, 0.f, 300.f, "%.1f ms",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Middle-click to type the exact value");
    changed |= ImGui::SliderFloat("Curve", &xfadeCurve_, 0.f, 1.f,
                                  "equal gain < %.2f > equal power",
                                  ImGuiSliderFlags_AlwaysClamp);

    int64_t xfadeNReq = 0, xfadeN = 0, nLoopSamples = 0, notePeriod = 0;
    harmonicdsp::sizes(ls_, le_, src_->sampleRate, kFftSizes[fftSizeIdx_], xfadeMs_, noteMode_,
                       docRootNote_, tuneCents_, xfadeNReq, xfadeN, nLoopSamples, notePeriod);
    const double sr = src_->sampleRate;
    if (noteMode_) {
        ImGui::Text("Note %d (%.2f Hz) -> cycle of %lld samples (%.2f Hz actual)", docRootNote_,
                    harmonicdsp::noteFreq(docRootNote_, tuneCents_), (long long)notePeriod,
                    sr / (double)notePeriod);
        ImGui::Text("Synthesized loop: %lld cycles, %lld samples (%.1f ms)",
                    (long long)(nLoopSamples / notePeriod), (long long)nLoopSamples,
                    1000.0 * (double)nLoopSamples / sr);
    } else {
        ImGui::Text("Synthesized loop: %lld samples (%.1f ms)", (long long)nLoopSamples,
                    1000.0 * (double)nLoopSamples / sr);
    }
    ImGui::Text("Entry crossfade: %lld samples (%.1f ms)", (long long)xfadeN,
                1000.0 * (double)xfadeN / sr);
    if (xfadeN < xfadeNReq)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "Limited: not enough audio before the loop start, "
                           "or the crossfade exceeds the loop's duration");

    if (!lastOk_)
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f),
                           "Not enough audio around the loop start for this "
                           "fft-size: try a smaller size");
    else
        ImGui::TextDisabled("Loop continuity: max phase error %.2e", lastSeamError_);
    ImGui::TextDisabled(
        "The crossfade blends the real attack (before the loop) into the "
        "start of the synthesized loop; it's already audible in the preview "
        "(the waveform's orange edge includes that span). Printed into the "
        "document when you press Apply.");

    return changed;
}

std::shared_ptr<AudioData> HarmonicLoop::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    const int nch = src_->channels();

    harmonicdsp::Result res;
    lastOk_ = harmonicdsp::buildLoop(*src_, ls_, le_, kFftSizes[fftSizeIdx_], xfadeMs_,
                                     xfadeCurve_, noteMode_, docRootNote_, tuneCents_, res);
    if (!lastOk_) {
        d->ch.resize(nch);
        return d;
    }
    lastSeamError_ = res.seamError;
    lastXfadeN_ = res.xfadeN;
    lastNotePeriod_ = res.notePeriod;
    // previewLoopOffset()==xfadeN tells the engine that only the pure period
    // (from xfadeN onward) repeats (see Engine::process); the blended attack
    // before that point plays once when entering preview, same as in final
    // playback -- with no smoothing needed in this copy, the engine reuses
    // the same mechanism as a marked loop in normal playback.
    d->ch = assemble(res, nch);
    return d;
}

void HarmonicLoop::apply(Document& doc) {
    harmonicdsp::Result res;
    if (!harmonicdsp::buildLoop(*src_, ls_, le_, kFftSizes[fftSizeIdx_], xfadeMs_, xfadeCurve_,
                               noteMode_, docRootNote_, tuneCents_, res))
        return;
    auto out = assemble(res, src_->channels());
    doc.replaceRange(ls_ - res.xfadeN, le_, std::move(out), ls_, ls_ + res.nLoopSamples,
                     "Harmonic");
}
