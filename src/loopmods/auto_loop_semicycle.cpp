#include "auto_loop_semicycle.hpp"

#include <cmath>
#include <cstdio>

#include "imgui.h"

namespace {

// Percent sliders in 0.1 steps, same resolution as the spec's region/length
// controls.
bool pctSlider(const char* label, double& v, double lo, double hi) {
    float f = (float)v;
    bool changed = ImGui::SliderFloat(label, &f, (float)lo, (float)hi, "%.1f%%",
                                      ImGuiSliderFlags_AlwaysClamp);
    if (changed) v = std::round((double)f * 10.0) / 10.0;
    return changed;
}

bool msSlider(const char* label, double& v, double sampleRate) {
    float f = (float)v;
    char fmt[48];
    snprintf(fmt, sizeof(fmt), "%%.1f ms (%d samples)", (int)(sampleRate * v / 1000.0));
    bool changed = ImGui::SliderFloat(label, &f, 0.1f, 100.f, fmt,
                                      ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    if (changed) v = f;
    return changed;
}

} // namespace

void AutoLoopSemicycle::drawParams(double sampleRate) {
    ImGui::TextWrapped(
        "Splices only right after zero crossings, where the local slope "
        "pattern matches, and scores each pair by comparing the waveform "
        "around both points in a log (dB) scale -- quiet detail counts as "
        "much as the peaks. Short: a loop of N whole cycles. Long: tries "
        "every length in a range, then refines the best one sample by "
        "sample.");

    int mode = longMode_ ? 1 : 0;
    ImGui::RadioButton("Short loop", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Long loop", &mode, 1);
    longMode_ = mode == 1;

    semicycledsp::Params& p = longMode_ ? long_ : short_;
    ImGui::SeparatorText("Search region (no selection)");
    pctSlider("Start", p.regionStartPct, 0.0, 100.0);
    pctSlider("End", p.regionEndPct, 0.0, 100.0);
    ImGui::SetItemTooltip(
        "Where the loop END may fall, in %% of the sample. With a selection, "
        "the selection is used instead.");

    if (!longMode_) {
        ImGui::SeparatorText("Cycles");
        ImGui::SliderInt("Cycles", &p.cycles, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("The loop spans exactly this many periods (zero-crossing pairs).");
    } else {
        ImGui::SeparatorText("Loop length (no selection)");
        pctSlider("Min length", p.minLengthPct, 0.1, 100.0);
        pctSlider("Max length", p.maxLengthPct, 0.1, 100.0);
        ImGui::SetItemTooltip(
            "In %% of the search region. With a selection they're fixed at "
            "40%% / 100%% of the selection.");
    }

    ImGui::SeparatorText("Accuracy");
    msSlider("Compare window", p.compareMs, sampleRate);
    ImGui::SetItemTooltip(
        "How much audio around each splice point is compared. Longer = "
        "pickier matches, slower search.");

    if (ImGui::Button("Defaults"))
        p = longMode_ ? semicycledsp::longDefaults() : semicycledsp::shortDefaults();
}

std::vector<autoloop::AlgoResult> AutoLoopSemicycle::run(const AudioData& audio, int64_t s0,
                                                          int64_t s1) {
    semicycledsp::Params p = longMode_ ? long_ : short_;
    // The popup passes the whole sample when there's no selection.
    p.useSelection = !(s0 <= 0 && s1 >= audio.frames());
    p.selStart = s0;
    p.selEnd = s1;

    semicycledsp::Result r = semicycledsp::findLoop(audio, p);
    if (!r.ok) return {};

    autoloop::AlgoResult a;
    a.start = r.start;
    a.end = r.end;
    a.quality = r.error;
    char buf[48];
    snprintf(buf, sizeof(buf), "error %.4f dB", r.error);
    a.info = buf;
    return {a};
}
