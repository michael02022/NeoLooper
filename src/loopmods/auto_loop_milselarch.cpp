#include "auto_loop_milselarch.hpp"

#include <cstdio>

#include "imgui.h"

#include "auto_loop_finder.hpp"

void AutoLoopMilselarch::drawParams(double /*sampleRate*/) {
    ImGui::TextWrapped(
        "Coarse-to-fine matching: tries several candidate loop lengths "
        "between Min and Max across the search range, then refines the "
        "end point sample-accurately. github.com/milselarch/autolooper");
    ImGui::SliderFloat("Min loop length", &minMs_, 5.f, 5000.f, "%.0f ms",
                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Max loop length", &maxMs_, 20.f, 20000.f, "%.0f ms",
                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    if (maxMs_ < minMs_) maxMs_ = minMs_;
}

std::vector<autoloop::AlgoResult> AutoLoopMilselarch::run(const AudioData& audio, int64_t s0, int64_t s1) {
    autoloopdsp::Params p;
    p.minLoopSec = minMs_ / 1000.0;
    p.maxLoopSec = maxMs_ / 1000.0;
    autoloopdsp::Result r = autoloopdsp::findLoopPoints(audio, s0, s1, p);
    if (!r.ok) return {};

    autoloop::AlgoResult out;
    out.start = r.start;
    out.end = r.end;
    out.quality = r.score;
    char buf[32];
    snprintf(buf, sizeof(buf), "score %.4f", r.score);
    out.info = buf;
    return {out};
}
