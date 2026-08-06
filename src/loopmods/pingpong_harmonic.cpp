#include "pingpong_harmonic.hpp"

#include "imgui.h"

#include "pingpong_harmonic_dsp.hpp"

namespace {
// blended attack + synthesized laps, in that order: this is the content
// both the preview (with previewLoopOffset()==xfadeN) and Apply insert.
std::vector<std::vector<float>> assemble(const pingpongdsp::Result& res, int nch) {
    std::vector<std::vector<float>> out(nch);
    for (int c = 0; c < nch; ++c) {
        auto& v = out[c];
        v.reserve(res.ch[c].attackTail.size() + res.ch[c].body.size());
        v.insert(v.end(), res.ch[c].attackTail.begin(), res.ch[c].attackTail.end());
        v.insert(v.end(), res.ch[c].body.begin(), res.ch[c].body.end());
    }
    return out;
}
} // namespace

bool PingPongHarmonicLoop::drawParams() {
    bool changed = false;
    static const char* kSizeLabels[] = {"128", "256", "512", "1024", "2048", "4096", "8192"};
    changed |= ImGui::Combo("FFT size", &fftSizeIdx_, kSizeLabels, 7);

    changed |= ImGui::SliderFloat("Crossfade", &xfadeMs_, 0.f, 300.f, "%.1f ms",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Middle-click to type the exact value");
    changed |= ImGui::SliderFloat("Curve", &xfadeCurve_, 0.f, 1.f,
                                  "equal gain < %.2f > equal power",
                                  ImGuiSliderFlags_AlwaysClamp);

    int64_t xfadeNReq = 0, xfadeN = 0, oneLap = 0;
    pingpongdsp::sizes(ls_, le_, src_->sampleRate, kFftSizes[fftSizeIdx_], xfadeMs_,
                       xfadeNReq, xfadeN, oneLap);
    const double sr = src_->sampleRate;
    ImGui::Text("One lap: %lld samples (%.1f ms)", (long long)oneLap,
                1000.0 * (double)oneLap / sr);
    ImGui::Text("Entry crossfade: %lld samples (%.1f ms)", (long long)xfadeN,
                1000.0 * (double)xfadeN / sr);
    if (xfadeN < xfadeNReq)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "Limited: not enough audio before the loop start, "
                           "or the crossfade exceeds one lap's duration");

    if (!lastOk_)
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f),
                           "Loop region too short for this fft-size (at "
                           "least 3 STFT frames are needed): try a smaller "
                           "size or enlarge the marked loop");
    else
        ImGui::TextDisabled("Lap->lap continuity: max phase error %.2e", lastSeamError_);
    ImGui::TextDisabled(
        "Reverses the ORDER of the spectral frames (forward and backward), "
        "not the samples: the synthesis phase always advances forward "
        "(identity phase-locking on harmonic peaks), so only the envelope "
        "walks backward -- the pitch never sounds reversed. The crossfade "
        "blends the real attack into the start of the first lap; it's "
        "already audible in the preview.");

    return changed;
}

std::shared_ptr<AudioData> PingPongHarmonicLoop::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    const int nch = src_->channels();

    pingpongdsp::Result res;
    lastOk_ = pingpongdsp::buildLoop(*src_, ls_, le_, kFftSizes[fftSizeIdx_], /*laps=*/1,
                                     xfadeMs_, xfadeCurve_, res);
    if (!lastOk_) {
        d->ch.resize(nch);
        return d;
    }
    lastSeamError_ = res.seamError;
    lastXfadeN_ = res.xfadeN;
    lastOneLapSamples_ = res.oneLapSamples;
    // previewLoopOffset()==xfadeN tells the engine that only the synthesized
    // material (from xfadeN onward) repeats; the blended attack before that
    // point plays once when entering preview, same as Harmonic.
    d->ch = assemble(res, nch);
    return d;
}

void PingPongHarmonicLoop::apply(Document& doc) {
    pingpongdsp::Result res;
    if (!pingpongdsp::buildLoop(*src_, ls_, le_, kFftSizes[fftSizeIdx_], /*laps=*/1,
                                xfadeMs_, xfadeCurve_, res))
        return;
    auto out = assemble(res, src_->channels());
    // The marked loop covers exactly the single rendered lap.
    doc.replaceRange(ls_ - res.xfadeN, le_, std::move(out), ls_, ls_ + res.oneLapSamples,
                     "PingPong Harmonic");
}
