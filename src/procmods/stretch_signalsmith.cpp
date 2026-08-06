#include "stretch_signalsmith.hpp"

#include <cmath>

#include "imgui.h"

#include "signalsmith_stretch_dsp.hpp"

bool StretchSignalsmith::drawParams() {
    bool changed = false;
    ImGui::TextWrapped(
        "Signalsmith Stretch (signalsmith-audio.co.uk/code/stretch): "
        "spectral phase-vocoder with automatic transient/tonal handling. "
        "Works best on regions of at least ~100-200 ms; very short "
        "selections may fail to produce a result.");

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
    if (ImGui::Checkbox("Cheaper preset (lower latency/quality)", &cheaper_)) changed = true;
    return changed;
}

std::vector<std::vector<float>> StretchSignalsmith::render(const AudioData& src, int64_t s0,
                                                            int64_t s1) {
    return signalsmithstretchdsp::apply(src, s0, s1, percent_, cheaper_);
}
