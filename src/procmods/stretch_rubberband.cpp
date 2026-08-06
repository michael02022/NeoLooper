#include "stretch_rubberband.hpp"

#include <cmath>

#include "imgui.h"

#include "rubberband_stretch_dsp.hpp"

bool StretchRubberBand::drawParams() {
    bool changed = false;
    ImGui::TextWrapped(
        "Rubber Band Library (breakfastquay.com/rubberband): the "
        "time-stretcher behind Ardour, Audacity, Mixxx and others.");

    int pct = (int)std::lround(percent_);
    if (ImGui::SliderInt("Stretch", &pct, 5, 2000, "%d%%",
                         ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
        percent_ = (float)pct;
        changed = true;
    }
    ImGui::SetItemTooltip(
        "100%% = unchanged. Below 100%% = faster/shorter, above 100%% = "
        "slower/longer. Middle-click to type the exact value.");

    ImGui::Separator();
    if (ImGui::Checkbox("Finer engine (R3)", &finerEngine_)) changed = true;
    ImGui::SetItemTooltip(
        "R3 \"Finer\" (checked): higher quality, the library's recommended "
        "choice for offline use like this. Unchecked uses R2 \"Faster\", "
        "tuned instead for real-time performance.");
    return changed;
}

std::vector<std::vector<float>> StretchRubberBand::render(const AudioData& src, int64_t s0,
                                                           int64_t s1) {
    return rubberbandstretchdsp::apply(
        src, s0, s1, percent_,
        finerEngine_ ? rubberbandstretchdsp::Engine::Finer : rubberbandstretchdsp::Engine::Faster);
}
