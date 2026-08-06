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
        "Infinity Freeze Looper style: all the energy is quantized to the "
        "harmonic series of the document's root note (phase vocoder, not "
        "naive) and the period is rounded to whole samples -- every cycle "
        "of the frozen chunk is BIT-IDENTICAL to the rest. On Apply a loop "
        "of a SINGLE cycle gets marked: ideal for very tight memory "
        "budgets (phones, old consoles). Set the root note to roughly the "
        "sample's real pitch, like Infinity's MIDI Note Number.");
    // The root note is edited outside this window (toolbar): if it changes
    // while the preview is active, it needs to be re-rendered.
    if (cycleMode_ && docRootNote_ != lastRootNote_) changed = true;
    lastRootNote_ = docRootNote_;

    changed |= ImGui::SliderFloat("Position", &freezePos_, 0.f, 1.f, "%.4f",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("0.0 = start of the sample, 1.0 = end of the sample");
    changed |= ImGui::SliderFloat("Duration", &durationMs_, 0.f, 5000.f, "%.1f ms",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(cycleMode_
                              ? "Rounded to an integer number of cycles (0 ms = a "
                                "single cycle, the minimum memory footprint). "
                                "Middle-click for the exact value."
                              : "Middle-click to type the exact value");
    changed |= ImGui::SliderFloat("Offset / phase", &offsetFrac_, 0.f, 1.f, "%.4f",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(cycleMode_
                              ? "Manual rotation within a SINGLE cycle (rotating "
                                "further would be identity: the cycles are "
                                "identical)."
                              : "Manual rotation of the frozen chunk. No "
                                "automatic phase search: adjust by ear/eye.");
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
    ImGui::Separator();
    changed |= ImGui::SliderFloat("Crossfade", &xfadeMs_, 0.f, 300.f, "%.1f ms",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Blends the real attack into the frozen chunk (entry); "
                          "the frozen chunk overwrites to the end of the sample, "
                          "there's no exit blend.");
    changed |= ImGui::SliderFloat("Curve", &xfadeCurve_, 0.f, 1.f,
                                  "equal gain < %.2f > equal power",
                                  ImGuiSliderFlags_AlwaysClamp);

    int64_t freezeStart = 0, xfadeNReq = 0, xfadeN = 0, nLoopSamples = 0;
    freezedsp::sizes(src_->frames(), src_->sampleRate, freezePos_, durationMs_, xfadeMs_,
                     kFftSizes[fftSizeIdx_], freezeStart, xfadeNReq, xfadeN, nLoopSamples);
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
        ImGui::Text("Note %d (%.2f Hz) -> cycle of %lld samples (%.2f Hz actual)",
                    docRootNote_, f0, (long long)period, sr / (double)period);
        if (lastOk_ && lastCyclePeriod_ > 0)
            ImGui::Text("%lld bit-identical cycles; on Apply a single-cycle loop gets marked",
                        (long long)(lastNLoopSamples_ / lastCyclePeriod_));
    }
    ImGui::Text("Entry crossfade: %lld samples (%.1f ms)", (long long)xfadeN,
                1000.0 * (double)xfadeN / sr);
    if (xfadeN < xfadeNReq)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "Limited: not enough audio before the freeze point, "
                           "or the crossfade exceeds the frozen duration");

    if (!lastOk_)
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f),
                           "Not enough audio around the freeze point for this "
                           "fft-size: try a smaller size");
    else if (cycleMode_)
        ImGui::TextDisabled("Fold purity: max residual %.2e (the cycle's wrap is "
                            "exact by construction)",
                            lastSeamError_);
    else
        ImGui::TextDisabled("Frozen chunk continuity: max error %.2e",
                            lastSeamError_);
    ImGui::TextDisabled(
        "Freezes the sample from 'Position' to the end -- the rest of the "
        "original sample is discarded. Doesn't mark loop start/end or try "
        "to align phases: that's adjusted by hand (offset, tuning and, "
        "afterward, if wanted, marking the loop).");

    return changed;
}

std::shared_ptr<AudioData> FreezeLoop::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    const int nch = src_->channels();
    d->ch.resize(nch);

    freezedsp::Result res;
    lastOk_ = freezedsp::buildFreeze(*src_, freezePos_, durationMs_, offsetFrac_, xfadeMs_,
                                     xfadeCurve_, tuneCents_, kFftSizes[fftSizeIdx_],
                                     cycleMode_, docRootNote_, res);
    if (!lastOk_) return d;
    lastSeamError_ = res.seamError;
    lastFreezeStart_ = res.freezeStart;
    lastXfadeN_ = res.xfadeN;
    lastNLoopSamples_ = res.nLoopSamples;
    lastCyclePeriod_ = res.cyclePeriod;
    for (int c = 0; c < nch; ++c) d->ch[c] = specloop::assemble(res.ch[c]);
    return d;
}

void FreezeLoop::apply(Document& doc) {
    freezedsp::Result res;
    if (!freezedsp::buildFreeze(*src_, freezePos_, durationMs_, offsetFrac_, xfadeMs_,
                               xfadeCurve_, tuneCents_, kFftSizes[fftSizeIdx_],
                               cycleMode_, docRootNote_, res))
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
