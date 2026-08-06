#include "auto_loop_loopauditioneer.hpp"

#include <cstdio>

#include "imgui.h"

#include "loopauditioneer_finder.hpp"

void AutoLoopLoopAuditioneer::drawParams(double /*sampleRate*/) {
    ImGui::TextWrapped(
        "Built for pipe organ sustain samples: finds low-derivative "
        "\"flat\" points inside the stable part of the sample, then keeps "
        "pairs whose immediate surroundings (5 samples) match closely. "
        "Returns several ranked candidates to audition, not just one "
        "guess. github.com/GrandOrgue/LoopAuditioneer");

    ImGui::Checkbox("Auto-detect sustain section", &autoSustain_);
    ImGui::SetItemTooltip(
        "On: automatically finds the stable part of the sample (after the "
        "attack, before any release) within the search range using an RMS "
        "heuristic. Off: the whole search range (selection, or whole "
        "sample) is used as the sustain section as-is.");

    ImGui::SliderFloat("Derivative threshold", &derivativeThresholdPct_, 0.1f, 50.f, "%.1f%%",
                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip(
        "How flat a point must be (relative to the sustain section's "
        "steepest slope) to become a splice candidate. Lower = stricter, "
        "fewer candidates.");
    ImGui::SliderFloat("Min loop length", &minLoopDurationSec_, 0.02f, 10.f, "%.3f s",
                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Min distance between candidates", &minDistanceSec_, 0.01f, 5.f, "%.3f s",
                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Quality factor (max edge difference)", &qualityFactor_, 0.0001f, 0.5f, "%.4f",
                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip(
        "Max allowed amplitude difference summed over the 5 samples "
        "around each edge (-1..1 scale). Lower = stricter matches, fewer "
        "(but better) candidates.");
    ImGui::SliderInt("Max candidates", &maxCandidates_, 50, 2500, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "Capped well below the original desktop app's default (50000): "
        "that assumes a background search with a progress bar, this runs "
        "synchronously on the button click.");
    ImGui::SliderInt("Loops to return", &loopsToReturn_, 1, 20, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderInt("Loop pool multiple", &maxLoopsMultiple_, 2, 50, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Checkbox("Brute force", &bruteForce_);
    ImGui::SetItemTooltip(
        "Match every candidate against every other instead of stopping "
        "early once enough loops are found. Slower, more thorough.");
}

std::vector<autoloop::AlgoResult> AutoLoopLoopAuditioneer::run(const AudioData& audio, int64_t s0,
                                                                int64_t s1) {
    loopauditioneerdsp::Params p;
    p.autoSustain = autoSustain_;
    p.derivativeThreshold = derivativeThresholdPct_ / 100.0;
    p.minLoopDurationSec = minLoopDurationSec_;
    p.minDistanceBetweenLoopsSec = minDistanceSec_;
    p.qualityFactor = qualityFactor_;
    p.maxCandidates = maxCandidates_;
    p.loopsToReturn = loopsToReturn_;
    p.maxLoopsMultiple = maxLoopsMultiple_;
    p.useBruteForce = bruteForce_;

    loopauditioneerdsp::Result r = loopauditioneerdsp::findLoops(audio, s0, s1, p);
    if (!r.ok) return {};

    std::vector<autoloop::AlgoResult> out;
    out.reserve(r.loops.size());
    for (const auto& c : r.loops) {
        autoloop::AlgoResult a;
        a.start = c.start;
        a.end = c.end;
        a.quality = c.quality;
        char buf[32];
        snprintf(buf, sizeof(buf), "quality %.5f", c.quality);
        a.info = buf;
        out.push_back(a);
    }
    return out;
}
