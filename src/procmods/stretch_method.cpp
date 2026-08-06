#include "stretch_method.hpp"

#include <algorithm>
#include <cmath>

#include "imgui.h"

void StretchMethod::open(AudioDataPtr src, int64_t s0, int64_t s1) {
    src_ = std::move(src);
    s0_ = s0;
    s1_ = s1;
    open_ = true;
    prevDirty_ = true; // the first draw() publishes the initial preview
    lastEditT_ = 0.0;
    previewDoc_.reset();
}

void StretchMethod::close(Engine& eng) {
    clearPreview(eng);
    open_ = false;
    src_.reset();
}

void StretchMethod::clearPreview(Engine& eng) {
    previewDoc_.reset();
    eng.publishDocPreview(nullptr);
}

void StretchMethod::rebuildPreview(Engine& eng) {
    prevDirty_ = false;
    if (!src_) return;
    auto piece = render(*src_, s0_, s1_);
    if (piece.empty()) {
        clearPreview(eng);
        return;
    }
    // Splices [s0_,s1_) with 'piece' (a DIFFERENT length except exactly at
    // 1x) -- same scheme as the old StretchTool/PitchTool::rebuildPreview.
    auto pd = std::make_shared<AudioData>();
    pd->sampleRate = src_->sampleRate;
    pd->format = src_->format;
    const int nch = src_->channels();
    pd->ch.resize(nch);
    for (int c = 0; c < nch; ++c) {
        const auto& s = src_->ch[(size_t)c];
        const auto& p = piece[std::min((size_t)c, piece.size() - 1)];
        auto& v = pd->ch[(size_t)c];
        v.reserve(s.size() - (size_t)(s1_ - s0_) + p.size());
        v.insert(v.end(), s.begin(), s.begin() + s0_);
        v.insert(v.end(), p.begin(), p.end());
        v.insert(v.end(), s.begin() + s1_, s.end());
    }
    pd->buildPeaks();
    previewDoc_ = pd;
    eng.publishDocPreview(std::move(pd));
}

void StretchMethod::draw(Document& doc, Engine& eng) {
    if (!open_) return;
    if (!src_ || s1_ <= s0_) {
        close(eng);
        return;
    }
    // Any widget that touches the method's own params sets this; at the
    // end of the frame it's decided (with a debounce) whether to
    // recompute the preview -- same pattern as FilterTool/PitchTool/the
    // old StretchTool::draw.
    bool specChanged = false;

    bool keep = true;
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(name(), &keep, ImGuiWindowFlags_NoCollapse)) {
        const int64_t L = s1_ - s0_;
        ImGui::Text("Region: %lld samples (%.1f ms)%s", (long long)L,
                    1000.0 * (double)L / src_->sampleRate,
                    (s0_ == 0 && s1_ == src_->frames()) ? " -- whole sample" : " -- selection");

        specChanged = drawParams();

        if (ImGui::Checkbox("Preview", &previewOn_)) {
            if (previewOn_) {
                prevDirty_ = true;
                lastEditT_ = 0.0; // recompute right away, skip the debounce
            } else {
                clearPreview(eng);
            }
        }
        ImGui::SetItemTooltip(
            "The waveform and playback use the stretched version of the "
            "document (without applying anything): loop markers, "
            "selection and envelopes keep working as usual. Recomputed "
            "when a control is released or paused, not on every pixel of "
            "a drag.");

        const int64_t newLen = (int64_t)std::llround((double)L * lengthFactor());
        ImGui::TextDisabled("Region becomes %lld samples (%.1f ms)", (long long)newLen,
                            1000.0 * (double)newLen / src_->sampleRate);

        ImGui::Separator();
        if (ImGui::Button("Apply", ImVec2(140, 0))) {
            doc.applyStretch(
                [this](const AudioData& s, int64_t a, int64_t b) { return render(s, a, b); }, name());
            keep = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) keep = false;

        // Same debounce as FilterTool/PitchTool: recomputing on every
        // pixel of a drag would be too expensive on long selections.
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
