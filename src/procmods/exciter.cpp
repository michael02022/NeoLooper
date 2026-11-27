#include "exciter.hpp"

#include <algorithm>
#include <cstdio>

#include "imgui.h"

#include "../engine.hpp"

void ExciterTool::open(AudioDataPtr src, int64_t s0, int64_t s1) {
    src_ = std::move(src);
    s0_ = s0;
    s1_ = s1;
    open_ = true;
    prevDirty_ = true; // the first draw() publishes the initial preview
    lastEditT_ = 0.0;
    previewDoc_.reset();
}

void ExciterTool::close(Engine& eng) {
    clearPreview(eng);
    open_ = false;
    src_.reset();
}

void ExciterTool::clearPreview(Engine& eng) {
    previewDoc_.reset();
    eng.publishDocPreview(nullptr);
}

void ExciterTool::rebuildPreview(Engine& eng) {
    prevDirty_ = false;
    if (!src_) return;
    auto piece = exciterdsp::applyMS(*src_, s0_, s1_, ms_);
    if (piece.empty()) {
        clearPreview(eng);
        return;
    }
    // Same scheme as FilterTool::rebuildPreview (same length, in place).
    auto pd = std::make_shared<AudioData>();
    pd->sampleRate = src_->sampleRate;
    pd->format = src_->format;
    pd->ch = src_->ch;
    for (size_t c = 0; c < piece.size() && c < pd->ch.size(); ++c)
        std::copy(piece[c].begin(), piece[c].end(), pd->ch[c].begin() + (size_t)s0_);
    pd->buildPeaks();
    previewDoc_ = pd;
    eng.publishDocPreview(std::move(pd));
}

bool ExciterTool::freqSlider(const char* label, float* hz, float def, const char* tip) {
    // 20 Hz .. 45% of this sample's rate, logarithmic: the whole spectrum
    // whatever the rate. A value past the top (e.g. the 12.2 kHz default on
    // an 8 kHz sample) shows/works as the top.
    const float hi = (float)exciterdsp::maxHz(src_->sampleRate);
    const float lo = (float)exciterdsp::kMinHz;
    float v = (float)exciterdsp::clampHz(*hz, src_->sampleRate);
    ImGui::SetNextItemWidth(-110.f);
    bool changed = ImGui::SliderFloat(label, &v, lo, std::max(lo + 1.f, hi), "%.0f Hz",
                                      ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    if (changed) *hz = v;
    if (ImGui::IsItemClicked(1)) { // right-click: default
        *hz = def;
        changed = true;
    }
    ImGui::SetItemTooltip("%s\n%.0f .. %.0f Hz here. Right-click resets, Ctrl+click types a value.", tip,
                          lo, hi);
    return changed;
}

bool ExciterTool::drawControls(bool stereo) {
    bool changed = false;
    auto slider = [&](const char* label, float* v, float lo, float hi, const char* fmt, const char* tip,
                      float def) {
        ImGui::SetNextItemWidth(-110.f);
        changed |= ImGui::SliderFloat(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemClicked(1)) { // right-click: default
            *v = def;
            changed = true;
        }
        ImGui::SetItemTooltip("%s\nRight-click resets.", tip);
    };
    ImGui::TextWrapped(
        "M/S Exciter: high-passes the signal, drives it through tube-like "
        "waveshapers (2x oversampled) and adds the harmonics back; plus a Mid/Side "
        "stage to widen the stereo image. The result is level-matched to the "
        "original, so it doesn't get louder or quieter.");
    ImGui::SeparatorText("Exciter");
    changed |= freqSlider("High-pass", &ms_.hpHz, 2000.f,
                          "Only what's above this frequency gets excited (12 dB/oct).\n"
                          "The original MS Exciter uses a fixed 2 kHz.");
    slider("Drive", &ms_.drive, 1.f, 20.f, "%.2f", "How hard the highs hit the waveshapers.", 1.f);
    slider("Bias", &ms_.bias, -1.f, 1.f, "%+.2f", "Asymmetric shaping: adds even harmonics.", 0.f);
    slider("Exciter Mix", &ms_.excMixPct, 0.f, 100.f, "%.0f%%",
           "How much of the generated harmonics is added on top of the signal.", 50.f);
    ImGui::SeparatorText("Mid/Side");
    ImGui::BeginDisabled(!stereo);
    slider("Width", &ms_.width, 0.f, 4.f, "%.2fx", "Side level: 0 = mono, 1 = unchanged, >1 wider.", 1.f);
    float st = ms_.stereoize * 100.f;
    slider("Stereoize", &st, 0.f, 100.f, "%.0f%%",
           "Adds a comb-filtered, delayed copy of the mid to the side: width out of mono content.", 0.f);
    ms_.stereoize = st / 100.f;
    slider("M/S Mix", &ms_.msMixPct, 0.f, 100.f, "%.0f%%", "Mid/Side stage vs. its input.", 100.f);
    ImGui::EndDisabled();
    if (!stereo) ImGui::TextDisabled("Mid/Side needs a stereo sample.");
    return changed;
}

void ExciterTool::draw(Document& doc, Engine& eng) {
    if (!open_) return;
    if (!src_ || s1_ <= s0_) {
        close(eng);
        return;
    }
    bool specChanged = false;
    bool keep = true;
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Exciter", &keep, ImGuiWindowFlags_NoCollapse)) {
        const int64_t L = s1_ - s0_;
        ImGui::Text("Region: %lld samples (%.1f ms)%s", (long long)L,
                    1000.0 * (double)L / src_->sampleRate,
                    (s0_ == 0 && s1_ == src_->frames()) ? " -- whole sample" : " -- selection");

        specChanged |= drawControls(src_->channels() > 1);

        ImGui::Spacing();
        if (ImGui::Checkbox("Preview", &previewOn_)) {
            if (previewOn_) {
                prevDirty_ = true;
                lastEditT_ = 0.0;
            } else {
                clearPreview(eng);
            }
        }
        ImGui::SetItemTooltip(
            "The waveform and playback use the processed version (without\n"
            "applying anything). Recomputed when a control is released.");

        ImGui::Separator();
        if (ImGui::Button("Apply", ImVec2(120, 0))) {
            const auto ms = ms_;
            doc.applyProcess(
                [=](const AudioData& d, int64_t a, int64_t b) { return exciterdsp::applyMS(d, a, b, ms); },
                "Exciter");
            keep = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(120, 0))) {
            ms_ = exciterdsp::MSSpec{};
            specChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) keep = false;

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
