#include "classic_crossfade_inside.hpp"

#include "crossfade_inside_core.hpp"
#include "imgui.h"

int64_t ClassicCrossfadeInside::fadeLen() const {
    return ccinside::fadeLen(le_ - ls_, pct_);
}

bool ClassicCrossfadeInside::drawParams() {
    bool ch = false;
    ch |= ImGui::SliderFloat("Crossfade", &pct_, 0.f, 100.f, "%.2f%%",
                             ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Middle-click to type the exact value");
    ch |= ImGui::SliderFloat("Curve", &shape_, 0.f, 1.f,
                             "equal gain < %.2f > equal power",
                             ImGuiSliderFlags_AlwaysClamp);

    const int64_t L = le_ - ls_;
    const int64_t avail = fadeLen();
    ImGui::Text("Blend: %lld samples (%.1f ms)", (long long)avail,
                1000.0 * (double)avail / src_->sampleRate);
    ImGui::Text("Resulting loop: %lld samples (%.1f ms)", (long long)(L - avail),
                1000.0 * (double)(L - avail) / src_->sampleRate);
    if (avail > 0)
        ImGui::TextDisabled(
            "The loop start advances %lld samples (that head then plays "
            "once, as an attack); the loop end doesn't move.",
            (long long)avail);
    return ch;
}

void ClassicCrossfadeInside::blendTail(std::vector<std::vector<float>>& out) const {
    const int64_t avail = fadeLen();
    const int nch = src_->channels();
    out.resize(nch);
    for (int c = 0; c < nch; ++c) {
        std::vector<float> head(src_->ch[c].begin() + ls_, src_->ch[c].begin() + ls_ + avail);
        std::vector<float> tail(src_->ch[c].begin() + le_ - avail, src_->ch[c].begin() + le_);
        out[c] = ccinside::blendTail(head, tail, shape_);
    }
}

std::shared_ptr<AudioData> ClassicCrossfadeInside::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    const int64_t avail = fadeLen();
    std::vector<std::vector<float>> tail;
    blendTail(tail);
    const int nch = src_->channels();
    d->ch.resize(nch);
    for (int c = 0; c < nch; ++c) {
        auto& v = d->ch[c];
        // previewed loop content: [ls_+avail, le_-avail) intact followed by
        // the blended tail -- same order it will end up in the document
        // after Apply.
        v.assign(src_->ch[c].begin() + ls_ + avail, src_->ch[c].begin() + le_ - avail);
        v.insert(v.end(), tail[c].begin(), tail[c].end());
    }
    return d;
}

void ClassicCrossfadeInside::apply(Document& doc) {
    std::vector<std::vector<float>> tail;
    blendTail(tail);
    const int64_t avail = fadeLen();
    doc.replaceRange(le_ - avail, le_, std::move(tail), ls_ + avail, le_,
                     "Classic Crossfade (Inside)");
}
