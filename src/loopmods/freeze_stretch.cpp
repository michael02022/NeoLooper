#include "freeze_stretch.hpp"

#include <cmath>

#include "crossfade_inside_core.hpp"
#include "imgui.h"

bool FreezeStretch::drawParams() {
    bool changed = false;
    ImGui::TextWrapped(
        "Ambiance effect: freezes a short grain of audio at Position "
        "(via Signalsmith Stretch pinned at its most extreme setting) "
        "and generates an evolving texture from it, discarding the "
        "original attack. Classic Crossfade (Inside) blends the result's "
        "own tail into its own head as a final step, so it's ready to "
        "loop on its own.");

    changed |= ImGui::SliderFloat("Position", &positionPct_, 0.f, 100.f, "%.1f%%",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "Where in the document the ambiance grain is taken from: 0%% = "
        "start, 100%% = end.");
    changed |= ImGui::SliderFloat("Duration", &durationSec_, 0.5f, 60.f, "%.1f s",
                                  ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip(
        "How much of the (otherwise unbounded) frozen effect to render "
        "and keep -- this only cuts the render short, it doesn't change "
        "how frozen it sounds. Middle-click to type the exact value.");

    ImGui::Separator();
    ImGui::TextDisabled(
        "Last step: Classic Crossfade (Inside), blending the generated "
        "chunk's own tail into its own head.");
    changed |= ImGui::SliderFloat("Crossfade", &ccPct_, 0.f, 100.f, "%.2f%%",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Middle-click to type the exact value");
    changed |= ImGui::SliderFloat("Curve", &ccShape_, 0.f, 1.f,
                                  "equal gain < %.2f > equal power",
                                  ImGuiSliderFlags_AlwaysClamp);

    const int64_t durationSamples = (int64_t)std::llround((double)durationSec_ * src_->sampleRate);
    const int64_t avail = ccinside::fadeLen(durationSamples, ccPct_);
    ImGui::Text("Blend: %lld samples (%.1f ms)", (long long)avail,
                1000.0 * (double)avail / src_->sampleRate);
    ImGui::Text("Resulting loop: %lld samples (%.1f ms)", (long long)(durationSamples - avail),
                1000.0 * (double)(durationSamples - avail) / src_->sampleRate);

    if (!lastOk_)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "Not enough audio left at this position for a grain -- "
                           "move Position earlier.");
    return changed;
}

std::shared_ptr<AudioData> FreezeStretch::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    const int nch = src_->channels();
    d->ch.resize(nch);

    freezestretchdsp::Params p;
    p.positionFrac = positionPct_ / 100.0;
    p.durationSec = durationSec_;
    p.ccPct = ccPct_;
    p.ccShape = ccShape_;
    freezestretchdsp::Result res = freezestretchdsp::generate(*src_, p);
    lastOk_ = res.ok;
    if (!lastOk_) return d;
    lastLoopStart_ = res.loopStart;
    lastLoopEnd_ = res.loopEnd;
    d->ch = std::move(res.ambiance);
    return d;
}

void FreezeStretch::apply(Document& doc) {
    if (!lastOk_ || !preview_) return;
    // Replaces the WHOLE document (the original attack is discarded, not
    // prepended) and marks the loop directly, same convention as
    // ClassicCrossfadeInside/RotationLoop's own apply().
    doc.replaceRange(0, src_->frames(), std::move(preview_->ch), lastLoopStart_, lastLoopEnd_,
                     "FreezeStretch");
}
