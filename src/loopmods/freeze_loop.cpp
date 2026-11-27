#include "freeze_loop.hpp"

#include <algorithm>
#include <cmath>

#include "imgui.h"

#include "freeze_loop_dsp.hpp"

bool FreezeLoop::drawParams() {
    bool changed = false;
    // Freshly opened (dirty_ is only true on the first drawParams() after
    // open(), see loop_module.hpp): calibrates tuning to the document's,
    // sign inverted -- see the comment in the .hpp. The user can keep
    // adjusting it by hand afterward without it being overwritten again
    // while the window stays open.
    if (dirty_) tuneCents_ = std::clamp(-docTuneCents_, -99.f, 99.f);

    static const char* kSizeLabels[] = {"128", "256", "512", "1024", "2048", "4096", "8192"};
    changed |= ImGui::Combo("FFT size", &fftSizeIdx_, kSizeLabels, 7);

    changed |= ImGui::Checkbox("Exact cycle (root note)", &cycleMode_);
    ImGui::SetItemTooltip(
        "All the energy is quantized to the harmonic series of the "
        "document's root note (phase vocoder, not naive). With Loop cycles "
        "at 1 the loop is a SINGLE cycle rounded to whole samples (ideal for "
        "very tight memory budgets: phones, old consoles); more cycles make "
        "the loop k cycles in round(k*period) samples, which spreads that "
        "rounding and fixes the tuning. On Apply that loop gets marked. Set "
        "the root note to roughly the sample's real pitch.");
    // The root note is edited outside this window (toolbar): if it changes
    // while the preview is active, it needs to be re-rendered.
    if (cycleMode_ && docRootNote_ != lastRootNote_) changed = true;
    lastRootNote_ = docRootNote_;

    // Logarithmic: finer near the start, where the attack ends and the
    // freeze point usually is (Ctrl+click to type an exact value).
    changed |= ImGui::SliderFloat("Position", &freezePos_, 0.f, 1.f, "%.4f",
                                  ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("0.0 = start of the sample, 1.0 = end of the sample\n"
                          "Logarithmic: finer near the start (Ctrl+click to type)");
    ImGui::BeginDisabled(!cycleMode_);
    changed |= ImGui::SliderFloat("Average", &averageMs_, 0.f, 2000.f,
                                  averageMs_ > 0.f ? "%.0f ms" : "off (single frame)",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "Exact cycle mode: builds the cycle from this much audio AFTER the "
        "freeze point (every whole cycle, one FFT, each harmonic's energy "
        "on its exact harmonic) instead of "
        "a single FFT frame: the frozen timbre is that section's average, "
        "with the phase the audio has at the freeze point. Off = the "
        "original single-frame analysis.");
    ImGui::EndDisabled();
    if (cycleMode_) {
        changed |= ImGui::SliderInt("Loop cycles", &loopCycles_, 1, 1024, "%d cycles",
                                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip(
            "Length of the loop in cycles of the note. 1 = a single "
            "whole-sample cycle (minimum memory, pitch rounded by up to "
            "~866/period cents); more = k cycles in round(k*period) samples, "
            "the rounding spread over them: the worst case "
            "is ~866/loop-samples cents, so ~20 ms of loop keeps it under 1 "
            "cent at any note. Middle-click to type the exact value.");
        ImGui::SameLine();
        ImGui::TextDisabled("= %.1f ms", (double)durationForDsp());
    } else {
        changed |= ImGui::SliderFloat("Duration", &durationMs_, 0.f, 5000.f, "%.1f ms",
                                      ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Middle-click to type the exact value");
    }
    changed |= ImGui::Checkbox("Auto phase", &autoPhase_);
    ImGui::SetItemTooltip(
        "Rotates the frozen chunk to the phase that best matches the attack "
        "right before the freeze point (correlation, as Harmonic/Freeze "
        "(Alt)), so the entry crossfade starts aligned. Offset adds on top.");
    changed |= ImGui::SliderFloat("Offset / phase", &offsetFrac_, 0.f, 1.f, "%.4f",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(cycleMode_
                              ? "Manual rotation within a SINGLE cycle (rotating "
                                "further would be identity: the cycles are "
                                "identical); added to Auto phase when it's on."
                              : autoPhase_
                                    ? "Manual rotation of the frozen chunk, added "
                                      "to the automatic one."
                                    : "Manual rotation of the frozen chunk: adjust "
                                      "by ear/eye.");
    changed |= ImGui::SliderFloat("Tune", &tuneCents_, -99.f, 99.f, "%.1f cents",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        cycleMode_
            ? "Fine adjustment of the TARGET pitch (root note + cents) the "
              "harmonic series anchors to -- no resampling: the period stays "
              "an integer number of samples. Starts calibrated to the "
              "document's tuning (sign inverted: the real content is "
              "mistuned the opposite way from the correction that parameter "
              "applies)."
            : "Manual tuning of ONLY the synthesized chunk (not the real "
              "attack), by resampling -- slightly changes its duration. "
              "Starts calibrated to the document's tuning (sign inverted); "
              "adjust it by hand if the freeze (naive method, no phase "
              "vocoder) still sounds out of tune against the attack.");
    changed |= ImGui::SliderFloat("Loop gain", &loopGainDb_, -24.f, 24.f, "%+.1f dB",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "Level of the frozen chunk, set by you: dB over the level the "
        "freeze comes out at (0 dB). Only the frozen chunk changes -- the "
        "real attack before the freeze point keeps its level and the "
        "crossfade blends into the new one. Middle-click to type the exact "
        "value.");
    ImGui::Separator();
    changed |= ImGui::SliderFloat("Crossfade", &xfadeMs_, 0.f, 500.f, "%.1f ms",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Blends the real attack into the frozen chunk (entry); "
                          "the frozen chunk overwrites to the end of the sample, "
                          "there's no exit blend. Not limited by the chunk's length: "
                          "a short (even single-cycle) chunk is repeated under the "
                          "fade as many times as needed and keeps its own length.");
    changed |= ImGui::SliderFloat("Curve", &xfadeCurve_, 0.f, 1.f,
                                  "equal gain < %.2f > equal power",
                                  ImGuiSliderFlags_AlwaysClamp);

    int64_t freezeStart = 0, xfadeNReq = 0, xfadeN = 0, nLoopSamples = 0;
    freezedsp::sizes(src_->frames(), src_->sampleRate, freezePos_, durationForDsp(), xfadeMs_,
                     kFftSizes[fftSizeIdx_], freezeStart, xfadeNReq, xfadeN, nLoopSamples,
                     true);
    const double sr = src_->sampleRate;
    ImGui::Text("Freeze point: sample %lld (%.2f s)", (long long)freezeStart,
                (double)freezeStart / sr);
    // sizes()'s nLoopSamples is the REQUESTED duration; with tuning other
    // than 0 the resample adjusts it -- the real value (cached from the
    // last render()) is shown once one is available.
    const int64_t shownLoopSamples = lastOk_ ? lastNLoopSamples_ : nLoopSamples;
    ImGui::Text("Frozen chunk: %lld samples (%.1f ms), overwrites to the end",
                (long long)shownLoopSamples, 1000.0 * (double)shownLoopSamples / sr);
    if (cycleMode_) {
        const double f0 = 440.0 * std::pow(2.0, ((double)docRootNote_ - 69.0 +
                                                 (double)tuneCents_ / 100.0) / 12.0);
        const int64_t period = std::max<int64_t>(2, (int64_t)std::llround(sr / f0));
        if (lastOk_ && lastCyclePeriod_ > 0 && lastLoopCycles_ > 1) {
            const double actual = sr * (double)lastLoopCycles_ / (double)lastCyclePeriod_;
            ImGui::Text("Note %d (%.2f Hz) -> %lld cycles in %lld samples (%.3f Hz actual, "
                        "%+.2f cents)",
                        docRootNote_, f0, (long long)lastLoopCycles_, (long long)lastCyclePeriod_,
                        actual, 1200.0 * std::log2(actual / f0));
            ImGui::Text("On Apply the whole %lld-cycle loop gets marked", (long long)lastLoopCycles_);
        } else {
            ImGui::Text("Note %d (%.2f Hz) -> cycle of %lld samples (%.2f Hz actual, %+.2f cents)",
                        docRootNote_, f0, (long long)period, sr / (double)period,
                        1200.0 * std::log2(sr / (double)period / f0));
            if (lastOk_ && lastCyclePeriod_ > 0)
                ImGui::Text("%lld bit-identical cycles; on Apply a single-cycle loop gets marked",
                            (long long)(lastNLoopSamples_ / lastCyclePeriod_));
        }
    }
    if (lastOk_ && shownLoopSamples > 0 && xfadeN > shownLoopSamples)
        ImGui::Text("Entry crossfade: %lld samples (%.1f ms), the frozen chunk repeats "
                    "%.1f times under it",
                    (long long)xfadeN, 1000.0 * (double)xfadeN / sr,
                    (double)xfadeN / (double)shownLoopSamples);
    else
        ImGui::Text("Entry crossfade: %lld samples (%.1f ms)", (long long)xfadeN,
                    1000.0 * (double)xfadeN / sr);
    if (lastOk_ && lastLoopPeak_ > 0.f) {
        const double peakDb = 20.0 * std::log10((double)lastLoopPeak_);
        if (peakDb > 0.0)
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                               "Frozen chunk peak: %+.1f dBFS (over 0: lower Loop gain "
                               "before exporting to 16/24-bit)",
                               peakDb);
        else
            ImGui::Text("Frozen chunk peak: %+.1f dBFS", peakDb);
    }
    if (xfadeN < xfadeNReq)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "Limited: not enough audio before the freeze point");

    if (!lastOk_ && cycleMode_ && averageMs_ > 0.f)
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f),
                           "Not enough audio after the freeze point for the "
                           "Average section: shorten it or move the position");
    else if (!lastOk_)
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f),
                           "Not enough audio around the freeze point for this "
                           "fft-size: try a smaller size");
    else if (cycleMode_ && lastAveragedCycles_ > 0)
        ImGui::TextDisabled("Timbre averaged over %lld cycles after the freeze point",
                            (long long)lastAveragedCycles_);
    else if (cycleMode_)
        ImGui::TextDisabled("Fold purity: max residual %.2e (the cycle's wrap is "
                            "exact by construction)",
                            lastSeamError_);
    else
        ImGui::TextDisabled("Frozen chunk continuity: max error %.2e",
                            lastSeamError_);
    ImGui::TextDisabled(
        "Freezes the sample from 'Position' to the end -- the rest of the "
        "original sample is discarded. Auto phase aligns the entry with the "
        "attack; offset, tuning and (outside exact cycle mode) the loop "
        "markers are adjusted by hand.");

    return changed;
}

float FreezeLoop::durationForDsp() const {
    if (!cycleMode_) return durationMs_;
    const double f0 = 440.0 * std::pow(2.0, ((double)docRootNote_ - 69.0 +
                                             (double)tuneCents_ / 100.0) / 12.0);
    return (float)(1000.0 * (double)loopCycles_ / f0);
}

std::shared_ptr<AudioData> FreezeLoop::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    const int nch = src_->channels();
    d->ch.resize(nch);

    freezedsp::Result res;
    lastOk_ = freezedsp::buildFreeze(*src_, freezePos_, durationForDsp(), offsetFrac_, xfadeMs_,
                                     xfadeCurve_, tuneCents_, kFftSizes[fftSizeIdx_],
                                     cycleMode_, docRootNote_, res, extras());
    if (!lastOk_) return d;
    lastSeamError_ = res.seamError;
    lastFreezeStart_ = res.freezeStart;
    lastXfadeN_ = res.xfadeN;
    lastNLoopSamples_ = res.nLoopSamples;
    lastCyclePeriod_ = res.cyclePeriod;
    lastAveragedCycles_ = res.averagedCycles;
    lastLoopCycles_ = res.loopCycles;
    lastLoopPeak_ = res.loopPeak;
    for (int c = 0; c < nch; ++c) d->ch[c] = specloop::assemble(res.ch[c]);
    return d;
}

void FreezeLoop::apply(Document& doc) {
    freezedsp::Result res;
    if (!freezedsp::buildFreeze(*src_, freezePos_, durationForDsp(), offsetFrac_, xfadeMs_,
                               xfadeCurve_, tuneCents_, kFftSizes[fftSizeIdx_],
                               cycleMode_, docRootNote_, res, extras()))
        return;
    const int nch = src_->channels();
    std::vector<std::vector<float>> out(nch);
    for (int c = 0; c < nch; ++c) out[c] = specloop::assemble(res.ch[c]);
    // overwrites to the end of the document: the rest of the original
    // sample isn't kept.
    if (res.cyclePeriod > 0) {
        // Exact-cycle mode: unlike normal Freeze (which doesn't touch
        // markers), here the loop DOES get marked -- of a single cycle,
        // the mode's whole point (minimal memory); the cycle's exact
        // boundary (res.cyclePeriod samples) is impossible to mark by hand.
        doc.replaceRange(res.freezeStart - res.xfadeN, src_->frames(), std::move(out),
                         res.freezeStart, res.freezeStart + res.cyclePeriod,
                         "Freeze (exact cycle)");
    } else {
        doc.insertPiece(res.freezeStart - res.xfadeN, src_->frames(), std::move(out), "Freeze");
    }
}
