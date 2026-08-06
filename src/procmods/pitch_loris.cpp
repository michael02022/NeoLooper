#include "pitch_loris.hpp"

#include <cmath>

#include "imgui.h"

#include "loris_dsp.hpp"

double PitchLoris::autoRefHz(const Document& doc) {
    return 440.0 * std::pow(2.0, ((double)doc.rootNote() - 69.0 + (double)doc.tuneCents() / 100.0) / 12.0);
}

bool PitchLoris::drawParams(const Document& doc) {
    bool changed = false;
    ImGui::TextWrapped(
        "Additive resynthesis (sinusoidal partial tracking) instead of a "
        "phase vocoder: much better preservation of timbre and transients, "
        "but only for monophonic, quasi-harmonic material (a single "
        "melodic note) -- chords, percussion or noisy samples are better "
        "served by the other Pitch methods. Always re-synthesizes (even at "
        "0 semitones/0 cents), so unlike them there's no bit-identical "
        "passthrough.");

    changed |= ImGui::Checkbox("Auto (Root Note + Tune)", &autoRef_);
    ImGui::SetItemTooltip(
        "The Loris analysis needs to know the region's expected "
        "fundamental frequency. On (default): computed from the "
        "document's Root Note + Tune. Off: type it manually below -- for "
        "samples with no/wrong Root Note, or just to experiment.");

    float refDisplay = autoRef_ ? (float)autoRefHz(doc) : manualRefHz_;
    ImGui::BeginDisabled(autoRef_);
    if (ImGui::SliderFloat("Reference Frequency", &refDisplay, 20.f, 5000.f, "%.1f Hz",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
        manualRefHz_ = refDisplay;
        changed = true;
    }
    ImGui::SetItemTooltip("Middle-click to type the exact value.");
    ImGui::EndDisabled();
    if (autoRef_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(Root Note %d, Tune %.0f cents)", doc.rootNote(), doc.tuneCents());
    }

    // Recomputed every frame (not cached at open()) so a live change to
    // Root Note/Tune elsewhere in the editor is picked up automatically
    // while this window is open, same "read fresh" principle as s0_/s1_.
    const double resolved = autoRef_ ? autoRefHz(doc) : (double)manualRefHz_;
    if (std::fabs(resolved - refFreqHz_) > 1e-6) changed = true;
    refFreqHz_ = resolved;

    return changed;
}

std::vector<std::vector<float>> PitchLoris::render(const AudioData& src, int64_t s0, int64_t s1) {
    lorisdsp::LorisSpec spec;
    spec.semitones = spec_.semitones;
    spec.cents = spec_.cents;
    spec.refFreqHz = refFreqHz_;
    return lorisdsp::apply(src, s0, s1, spec);
}
