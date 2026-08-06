#include "pitch_signalsmith.hpp"

#include <algorithm>

#include "imgui.h"

#include "pitch_signalsmith_dsp.hpp"

bool PitchSignalsmith::drawParams(const Document&) {
    bool changed = false;
    ImGui::TextWrapped(
        "Signalsmith Stretch (signalsmith-audio.co.uk/code/stretch): "
        "spectral phase-vocoder pitch shift, duration always unchanged.");

    const float nyquist = (float)(src_->sampleRate * 0.5);
    tonalityLimitHz_ = std::min(tonalityLimitHz_, nyquist);
    if (ImGui::SliderFloat("Tonality limit", &tonalityLimitHz_, 0.f, nyquist, "%.0f Hz",
                           ImGuiSliderFlags_AlwaysClamp)) {
        changed = true;
    }
    ImGui::SetItemTooltip(
        "0 Hz (default) = the whole spectrum gets a plain pitch shift. "
        "Above 0: content past this frequency instead keeps its relative "
        "spacing (a more 'noise-preserving' shift) -- helps large shifts "
        "sound less smeared on broadband/noisy material. Middle-click to "
        "type the exact value.");

    if (ImGui::Checkbox("Preserve formants", &preserveFormants_)) changed = true;
    ImGui::SetItemTooltip(
        "Keeps the spectral envelope (vowel/timbre character) anchored to "
        "its original frequencies instead of moving with the pitch -- "
        "avoids the 'chipmunk'/'Darth Vader' effect on voice-like "
        "material.");
    return changed;
}

std::vector<std::vector<float>> PitchSignalsmith::render(const AudioData& src, int64_t s0,
                                                          int64_t s1) {
    return pitchsignalsmithdsp::apply(src, s0, s1, spec_, tonalityLimitHz_, preserveFormants_);
}
