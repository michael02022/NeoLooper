#include "loop_module.hpp"

#include "imgui.h"

void LoopModule::draw(Document& doc, Engine& eng) {
    if (!open_) return;
    if (!src_ || le_ <= ls_) {
        close(eng);
        return;
    }
    bool keep = true;
    docRootNote_ = doc.rootNote();
    docTuneCents_ = doc.tuneCents();
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(name(), &keep, ImGuiWindowFlags_NoCollapse)) {
        if (showLoopRegionHeader()) {
            const int64_t L = le_ - ls_;
            ImGui::Text("Loop region: %lld samples (%.1f ms)", (long long)L,
                        1000.0 * (double)L / src_->sampleRate);
            ImGui::TextDisabled(
                "The loop plays back looped while you adjust it; nothing is "
                "applied to the sample until you press Apply.");
            ImGui::Separator();
        }

        bool changed = drawParams();
        if (dirty_ || changed || !preview_) {
            preview_ = render();
            rebuildPreviewDoc();
            eng.publishPreview(preview_, previewLoopOffset());
            dirty_ = false;
            if (!eng.isPlaying()) eng.play();
        }

        ImGui::Separator();
        if (ImGui::Button("Apply", ImVec2(120, 0))) {
            apply(doc);
            keep = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) keep = false;
    }
    ImGui::End();
    if (!keep) close(eng);
}

void LoopModule::rebuildPreviewDoc() {
    if (!preview_ || !src_) {
        previewDoc_.reset();
        return;
    }
    // Full document with [previewRangeStart(), previewRangeEnd()) replaced
    // by the rendered content (normally previewRangeStart()==ls_ and
    // previewRangeEnd()==le_, except for modules that don't operate on a
    // marked loop, see those hooks).
    auto pd = std::make_shared<AudioData>();
    pd->sampleRate = src_->sampleRate;
    pd->format = src_->format;
    pd->ch.resize(src_->channels());
    const int64_t rs = previewRangeStart();
    const int64_t re = previewRangeEnd();
    const int64_t prevLen = preview_->frames();
    for (int c = 0; c < src_->channels(); ++c) {
        const auto& s = src_->ch[c];
        const auto& p = preview_->ch[std::min((size_t)c,
                                              preview_->ch.size() - 1)];
        auto& v = pd->ch[c];
        v.reserve(s.size() - (size_t)(re - rs) + (size_t)prevLen);
        v.insert(v.end(), s.begin(), s.begin() + rs);
        v.insert(v.end(), p.begin(), p.end());
        v.insert(v.end(), s.begin() + re, s.end());
    }
    pd->buildPeaks();
    previewDoc_ = std::move(pd);
}
