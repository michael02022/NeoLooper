#include "classic_crossfade.hpp"

#include <algorithm>

#include "imgui.h"

int64_t ClassicCrossfade::fadeLen() const {
    const int64_t L = le_ - ls_;
    const int64_t maxAvail = std::min(L, ls_);
    return (int64_t)std::llround((double)maxAvail * pct_ / 100.0);
}

bool ClassicCrossfade::drawParams() {
    bool ch = false;
    // Slider with decimal precision + exact entry to dial in the exact point.
    ch |= ImGui::SliderFloat("Crossfade", &pct_, 0.f, 100.f, "%.2f%%",
                             ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Middle-click to type the exact value");
    ch |= ImGui::SliderFloat("Curve", &shape_, 0.f, 1.f,
                             "equal gain < %.2f > equal power",
                             ImGuiSliderFlags_AlwaysClamp);

    const int64_t avail = fadeLen();
    ImGui::Text("Blend: %lld samples (%.1f ms)", (long long)avail,
                1000.0 * (double)avail / src_->sampleRate);
    if (ls_ <= 0)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "No audio before the loop start: the blend has no effect");
    return ch;
}

void ClassicCrossfade::renderInto(std::vector<std::vector<float>>& out) const {
    const int64_t L = le_ - ls_;
    const int64_t avail = fadeLen();
    const int nch = src_->channels();
    out.resize(nch);
    for (int c = 0; c < nch; ++c) {
        out[c].assign(src_->ch[c].begin() + ls_, src_->ch[c].begin() + le_);
        for (int64_t j = 0; j < avail; ++j) {
            float t = (float)(j + 1) / (float)avail;
            float gIn, gOut;
            xfadeGains(t, shape_, gIn, gOut);
            int64_t k = L - avail + j;          // position within the loop
            float pre = src_->ch[c][(size_t)(ls_ - avail + j)];
            out[c][(size_t)k] = out[c][(size_t)k] * gOut + pre * gIn;
        }
    }
}

std::shared_ptr<AudioData> ClassicCrossfade::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    renderInto(d->ch);
    return d;
}

void ClassicCrossfade::apply(Document& doc) {
    std::vector<std::vector<float>> out;
    renderInto(out);
    doc.replaceRange(ls_, le_, std::move(out), ls_, le_, "Classic Crossfade");
}
