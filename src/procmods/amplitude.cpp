#include "amplitude.hpp"

#include <algorithm>
#include <cmath>

#include "imgui.h"

#include "../engine.hpp"

namespace {
float linToDbFloor(float lin, float floorDb = -100.f) {
    if (lin <= 0.f) return floorDb;
    return std::max(floorDb, 20.f * std::log10(lin));
}
} // namespace

void AmplitudeTool::open(AudioDataPtr src, int64_t s0, int64_t s1) {
    src_ = std::move(src);
    s0_ = s0;
    s1_ = s1;
    open_ = true;
    prevDirty_ = true; // the first draw() publishes the initial preview
    lastEditT_ = 0.0;
    previewDoc_.reset();
}

void AmplitudeTool::close(Engine& eng) {
    clearPreview(eng);
    open_ = false;
    src_.reset();
}

void AmplitudeTool::clearPreview(Engine& eng) {
    previewDoc_.reset();
    eng.publishDocPreview(nullptr);
}

void AmplitudeTool::rebuildPreview(Engine& eng) {
    prevDirty_ = false;
    if (!src_) return;
    auto gained = amplitudedsp::apply(*src_, s0_, s1_, spec_);
    if (gained.empty()) {
        clearPreview(eng);
        return;
    }
    // Full document with [s0_, s1_) replaced by the gained version (same
    // length: gain doesn't change duration) -- same scheme as
    // FilterTool::rebuildPreview.
    auto pd = std::make_shared<AudioData>();
    pd->sampleRate = src_->sampleRate;
    pd->format = src_->format;
    pd->ch = src_->ch;
    for (size_t c = 0; c < gained.size() && c < pd->ch.size(); ++c)
        std::copy(gained[c].begin(), gained[c].end(), pd->ch[c].begin() + (size_t)s0_);
    pd->buildPeaks();
    previewDoc_ = pd;
    eng.publishDocPreview(std::move(pd));
}

void AmplitudeTool::draw(Document& doc, Engine& eng) {
    if (!open_) return;
    if (!src_ || s1_ <= s0_) {
        close(eng);
        return;
    }
    // Any widget that touches spec_ sets this; at the end of the frame it's
    // decided (with a debounce) whether to recompute the preview -- same
    // pattern as FilterTool::draw.
    bool specChanged = false;

    bool keep = true;
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Amplitude", &keep, ImGuiWindowFlags_NoCollapse)) {
        const int64_t L = s1_ - s0_;
        ImGui::Text("Region: %lld samples (%.1f ms)%s", (long long)L,
                    1000.0 * (double)L / src_->sampleRate,
                    (s0_ == 0 && s1_ == src_->frames()) ? " -- whole sample" : " -- selection");
        ImGui::TextWrapped(
            "Plain gain, baked into the audio itself (unlike a live volume "
            "envelope).");

        specChanged |= ImGui::SliderFloat("Gain", &spec_.gainDb, -60.f, 24.f, "%+.1f dB",
                                          ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Middle-click to type the exact value.");

        specChanged |= ImGui::Checkbox("Clip", &spec_.clip);
        ImGui::SetItemTooltip(
            "On (default): hard-clamps the result to [-1, 1] -- the classic "
            "digital 'clip' from pushing a signal too hot.\n"
            "Off: samples are left as-is beyond +-1.0 ('hot'); a later "
            "bit-depth conversion or export scales the whole file back down "
            "instead of clipping (same behavior as the 16/32-bit toggle).");

        const float peakIn = amplitudedsp::peakAbs(*src_, s0_, s1_);
        const float peakOut = peakIn * (float)amplitudedsp::gainLinear(spec_);
        ImGui::TextDisabled(
            "Peak: %.1f dBFS -> %.1f dBFS%s", linToDbFloor(peakIn),
            linToDbFloor(spec_.clip ? std::min(peakOut, 1.f) : peakOut),
            (!spec_.clip && peakOut > 1.f) ? " (over 0 dBFS)" : "");

        if (ImGui::Checkbox("Preview", &previewOn_)) {
            if (previewOn_) {
                prevDirty_ = true;
                lastEditT_ = 0.0; // recompute right away, skip the debounce
            } else {
                clearPreview(eng);
            }
        }
        ImGui::SetItemTooltip(
            "The waveform and playback use the document's gained version "
            "(without applying anything): loop markers, selection and "
            "envelopes keep working as usual. Recomputed when a control is "
            "released or paused, not on every pixel of a drag.");

        ImGui::Separator();
        if (ImGui::Button("Apply", ImVec2(120, 0))) {
            doc.applyAmplitude(spec_);
            keep = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(120, 0))) {
            spec_ = amplitudedsp::AmplitudeSpec{};
            specChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) keep = false;

        // Same debounce as FilterTool: recomputing on every pixel of a drag
        // would be too expensive on long selections.
        if (specChanged) {
            prevDirty_ = true;
            lastEditT_ = ImGui::GetTime();
        }
        if (previewOn_ && prevDirty_ &&
            (ImGui::GetTime() - lastEditT_ > 0.12 || !ImGui::IsAnyItemActive()))
            rebuildPreview(eng);
    }
    ImGui::End();
    if (!keep) close(eng);
}
