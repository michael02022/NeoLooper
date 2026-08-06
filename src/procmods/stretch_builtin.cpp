#include "stretch_builtin.hpp"

#include <cmath>

#include "imgui.h"

bool StretchBuiltin::drawParams() {
    bool changed = false;
    ImGui::TextWrapped(
        "Changes duration (slower/longer or faster/shorter) without "
        "changing pitch -- plain phase-vocoder time-stretch.");

    int pct = (int)std::lround(spec_.percent);
    if (ImGui::SliderInt("Stretch", &pct, 5, 2000, "%d%%",
                         ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
        spec_.percent = (float)pct;
        changed = true;
    }
    ImGui::SetItemTooltip(
        "100%% = unchanged. Below 100%% = faster/shorter, above 100%% = "
        "slower/longer. Middle-click to type the exact value.");

    ImGui::Separator();
    static const char* kSizeLabels[] = {"128", "256", "512", "1024", "2048", "4096", "8192"};
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo("FFT size", &fftSizeIdx_, kSizeLabels, 7)) changed = true;
    ImGui::SetItemTooltip(
        "Larger = better frequency resolution (cleaner sustained tones), "
        "smaller = less smearing on transients.");
    return changed;
}

std::vector<std::vector<float>> StretchBuiltin::render(const AudioData& src, int64_t s0, int64_t s1) {
    return stretchdsp::apply(src, s0, s1, spec_, kFftSizes[fftSizeIdx_]);
}
