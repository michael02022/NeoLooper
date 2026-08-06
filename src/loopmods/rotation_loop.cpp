#include "rotation_loop.hpp"

#include "imgui.h"

#include "rotation_loop_dsp.hpp"

namespace {
// attackTail ++ ccHead ++ loopBlended, in that order: this is the content
// both the preview (with previewLoopOffset()==xfadeN+ccAvail) and Apply
// insert.
std::vector<std::vector<float>> assemble(const rotationdsp::Result& res, int nch) {
    std::vector<std::vector<float>> out(nch);
    for (int c = 0; c < nch; ++c) {
        auto& v = out[c];
        const auto& rc = res.ch[c];
        v.reserve(rc.attackTail.size() + rc.ccHead.size() + rc.loopBlended.size());
        v.insert(v.end(), rc.attackTail.begin(), rc.attackTail.end());
        v.insert(v.end(), rc.ccHead.begin(), rc.ccHead.end());
        v.insert(v.end(), rc.loopBlended.begin(), rc.loopBlended.end());
    }
    return out;
}
} // namespace

bool RotationLoop::drawParams() {
    bool changed = false;
    changed |= ImGui::SliderInt("Quality (layers)", &nLayers_, 2, 100, "%d",
                                ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "Number of rotated copies of the loop summed together; more layers "
        "= denser timbre (unison/supersaw style), with the level "
        "auto-compensated (equal-power) so volume doesn't change.");
    changed |= ImGui::SliderInt("Seed", &seed_, -1, kMaxSeed, "%d",
                                ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "-1 = random rotation offsets on every render; any other value = "
        "reproducible. Middle-click to type the exact value.");

    ImGui::Separator();
    changed |= ImGui::SliderFloat("Crossfade", &xfadeMs_, 0.f, 300.f, "%.1f ms",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Middle-click to type the exact value");
    changed |= ImGui::SliderFloat("Curve", &xfadeCurve_, 0.f, 1.f,
                                  "equal gain < %.2f > equal power",
                                  ImGuiSliderFlags_AlwaysClamp);

    ImGui::Separator();
    ImGui::TextDisabled(
        "Last step: Classic Crossfade (Inside), reused over the "
        "already-thickened loop -- blends its own tail into its own head.");
    changed |= ImGui::SliderFloat("Crossfade (inside)", &ccPct_, 0.f, 100.f, "%.2f%%",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Middle-click to type the exact value");
    changed |= ImGui::SliderFloat("Curve (inside)", &ccShape_, 0.f, 1.f,
                                  "equal gain < %.2f > equal power",
                                  ImGuiSliderFlags_AlwaysClamp);

    int64_t xfadeNReq = 0, xfadeN = 0, nLoopSamples = 0, ccAvail = 0;
    rotationdsp::sizes(ls_, le_, src_->sampleRate, xfadeMs_, ccPct_, xfadeNReq, xfadeN,
                       nLoopSamples, ccAvail);
    const double sr = src_->sampleRate;
    ImGui::Text("Thickened loop: %lld samples (%.1f ms)", (long long)nLoopSamples,
                1000.0 * (double)nLoopSamples / sr);
    ImGui::Text("Entry crossfade: %lld samples (%.1f ms)", (long long)xfadeN,
                1000.0 * (double)xfadeN / sr);
    if (xfadeN < xfadeNReq)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "Limited: not enough audio before the loop start, "
                           "or the crossfade exceeds the loop's duration");
    ImGui::Text("Internal blend (Classic Crossfade Inside): %lld samples (%.1f ms)",
                (long long)ccAvail, 1000.0 * (double)ccAvail / sr);
    ImGui::Text("Final loop: %lld samples (%.1f ms)", (long long)(nLoopSamples - ccAvail),
                1000.0 * (double)(nLoopSamples - ccAvail) / sr);

    if (lastSafetyNormalized_)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "The rotation offsets aligned several layers in phase: "
                           "attenuated to avoid exceeding the original peak");
    ImGui::TextDisabled(
        "Reorders the marked loop into rotated layers (without synthesizing "
        "anything new); requires it to already be seamless. The last step "
        "reuses Classic Crossfade (Inside) over the result.");

    return changed;
}

std::shared_ptr<AudioData> RotationLoop::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    const int nch = src_->channels();

    lastOk_ = rotationdsp::buildRotationLoop(*src_, ls_, le_, nLayers_, seed_, xfadeMs_,
                                             xfadeCurve_, ccPct_, ccShape_, lastResult_);
    if (!lastOk_) {
        d->ch.resize(nch);
        return d;
    }
    lastXfadeN_ = lastResult_.xfadeN;
    lastCcAvail_ = lastResult_.ccAvail;
    lastSafetyNormalized_ = lastResult_.safetyNormalized;
    // previewLoopOffset()==xfadeN+ccAvail tells the engine that only the
    // final loop (from that point on) repeats; everything before it (entry
    // crossfade + head excluded by Classic Crossfade Inside) plays once when
    // entering preview, same as in Harmonic/Freeze.
    d->ch = assemble(lastResult_, nch);
    return d;
}

void RotationLoop::apply(Document& doc) {
    // Reuses lastResult_ (the last previewed render()) AS-IS -- doesn't call
    // buildRotationLoop again, which with Seed=-1 would draw layer offsets
    // different from the ones that were heard. See the comment in the .hpp.
    if (!lastOk_) return;
    auto out = assemble(lastResult_, src_->channels());
    // The loop end doesn't move (same as Classic Crossfade Inside); the loop
    // start advances past both the entry crossfade AND the head that
    // Classic Crossfade Inside excluded from the loop.
    doc.replaceRange(ls_ - lastResult_.xfadeN, le_, std::move(out), ls_ + lastResult_.ccAvail, le_,
                     "Rotation");
}
