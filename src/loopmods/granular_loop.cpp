#include "granular_loop.hpp"

#include "imgui.h"

#include "granular_loop_dsp.hpp"

namespace {
// head ++ body, in that order: this is the content both the preview (with
// previewLoopOffset()==headLen) and Apply insert.
std::vector<std::vector<float>> assemble(const granulardsp::Result& res, int nch) {
    std::vector<std::vector<float>> out((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        auto& v = out[(size_t)c];
        const auto& h = res.head[(size_t)c];
        const auto& b = res.body[(size_t)c];
        v.reserve(h.size() + b.size());
        v.insert(v.end(), h.begin(), h.end());
        v.insert(v.end(), b.begin(), b.end());
    }
    return out;
}
} // namespace

bool GranularLoop::drawParams() {
    bool changed = false;
    changed |= ImGui::SliderInt("Grains", &nGrains_, 2, 60, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "Number of crossfade-chained fragments that make up the loop's "
        "body; more grains = longer body with more internal variation "
        "before it repeats.");

    changed |= ImGui::SliderFloat("Grain min", &grainMinPct_, 1.f, 50.f, "%.1f%%",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Minimum length of each grain, as %% of the marked region.");
    changed |= ImGui::SliderFloat("Grain max", &grainMaxPct_, 1.f, 50.f, "%.1f%%",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "Maximum length of each grain, as %% of the marked region. Each "
        "grain draws its length in [min, max] (except the last one, which "
        "is adjusted to close the loop exactly).");

    changed |= ImGui::SliderFloat("Head", &headPct_, 10.f, 90.f, "%.1f%%",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "%% of the marked region kept as a fixed head (unmodified): plays "
        "once, before entering the loop's repeating body.");

    ImGui::Separator();
    changed |= ImGui::SliderInt("Seed", &seed_, -1, kMaxSeed, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "-1 = random grains on every render; any other value = "
        "reproducible. Middle-click to type the exact value.");

    int64_t headLen = 0, splitPoint = 0, estMin = 0, estMax = 0;
    granulardsp::sizes(ls_, le_, headPct_, grainMinPct_, grainMaxPct_, nGrains_, headLen,
                       splitPoint, estMin, estMax);
    const double sr = src_->sampleRate;
    ImGui::Text("Head (plays once): %lld samples (%.1f ms)", (long long)headLen,
                1000.0 * (double)headLen / sr);
    ImGui::Text("Loop body (estimated): %.1f - %.1f ms", 1000.0 * (double)estMin / sr,
                1000.0 * (double)estMax / sr);
    ImGui::TextDisabled(
        "The body's real size depends on the seed and each grain's phase "
        "search -- this range is only an estimate.");

    if (lastPeriod_ > 0)
        ImGui::Text("Detected period: %lld samples (~%.1f Hz)", (long long)lastPeriod_,
                    sr / (double)lastPeriod_);
    else
        ImGui::TextDisabled(
            "No clear period detected (untuned sound, or too short): the "
            "phase search falls back to a single candidate.");

    if (!lastOk_)
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f),
                           "The marked region is too short for these parameters.");
    else if (lastClamped_)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "A grain got trimmed for lack of room in the marked region -- "
                           "try lowering 'Grain max' or marking a longer region.");
    ImGui::TextDisabled(
        "The loop's closure (end of body -> start of body) uses real, "
        "unedited audio, same as the rest of the body is built from "
        "fragments of the sustain itself blended together.");

    return changed;
}

std::shared_ptr<AudioData> GranularLoop::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    const int nch = src_->channels();

    lastOk_ = granulardsp::buildGranularLoop(*src_, ls_, le_, nGrains_, grainMinPct_,
                                             grainMaxPct_, headPct_, (int64_t)seed_,
                                             lastResult_);
    if (!lastOk_) {
        d->ch.resize((size_t)nch);
        return d;
    }
    lastHeadLen_ = lastResult_.headLen;
    lastNLoopSamples_ = lastResult_.nLoopSamples;
    lastPeriod_ = lastResult_.period;
    lastClamped_ = lastResult_.grainsClamped;
    // previewLoopOffset()==headLen tells the engine that only the body (from
    // there on) repeats; the head plays once when entering preview, same
    // mechanism as Harmonic/Freeze/Rotation.
    d->ch = assemble(lastResult_, nch);
    return d;
}

void GranularLoop::apply(Document& doc) {
    // Reuses lastResult_ (the last previewed render()) AS-IS -- doesn't call
    // buildGranularLoop again, which with Seed=-1 would draw a grain chain
    // different from the one that was heard. See the comment in the .hpp.
    if (!lastOk_) return;
    auto out = assemble(lastResult_, src_->channels());
    doc.replaceRange(ls_, le_, std::move(out), ls_ + lastResult_.headLen,
                     ls_ + lastResult_.headLen + lastResult_.nLoopSamples, "Granular");
}
