#include "pitch_builtin.hpp"

#include "imgui.h"

bool PitchBuiltin::drawParams(const Document&) {
    bool changed = false;
    changed |= ImGui::Checkbox("Preserve duration (stretch)", &preserveDuration_);
    ImGui::SetItemTooltip(
        "Off (default): traditional pitch shift -- plain resample, "
        "duration changes together with pitch (higher = shorter), like "
        "speeding up tape/vinyl.\n"
        "On: phase-vocoder time-stretch -- duration and sample rate "
        "stay exactly as they are, only the tonal content changes. "
        "More CPU; large shifts can sound a bit 'phasey' on "
        "transients.");
    if (preserveDuration_) {
        static const char* kSizeLabels[] = {"128", "256", "512", "1024", "2048", "4096", "8192"};
        ImGui::SetNextItemWidth(120);
        if (ImGui::Combo("FFT size", &fftSizeIdx_, kSizeLabels, 7)) changed = true;
        ImGui::SetItemTooltip(
            "Larger = better frequency resolution (cleaner sustained "
            "tones), smaller = less smearing on transients.");
    }
    return changed;
}

std::vector<std::vector<float>> PitchBuiltin::render(const AudioData& src, int64_t s0, int64_t s1) {
    return pitchdsp::apply(src, s0, s1, spec_, kFftSizes[fftSizeIdx_], preserveDuration_);
}
