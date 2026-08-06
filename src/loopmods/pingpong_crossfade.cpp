#include "pingpong_crossfade.hpp"

#include <algorithm>

#include "imgui.h"

int64_t PingPongCrossfade::junctionLen() const {
    const int64_t L = le_ - ls_;
    const int64_t maxX1 = std::max<int64_t>(0, L - 1);
    return (int64_t)std::llround((double)maxX1 * pctJunction_ / 100.0);
}

int64_t PingPongCrossfade::outLength() const {
    const int64_t L = le_ - ls_;
    return 2 * L - junctionLen();
}

int64_t PingPongCrossfade::endLen() const {
    const int64_t total = outLength();
    const int64_t maxX2 = std::min<int64_t>(ls_, std::max<int64_t>(0, total - 1));
    return (int64_t)std::llround((double)maxX2 * pctEnd_ / 100.0);
}

bool PingPongCrossfade::drawParams() {
    bool ch = false;
    ImGui::TextUnformatted("Normal <-> reversed junction");
    ch |= ImGui::SliderFloat("Crossfade##j", &pctJunction_, 0.f, 100.f,
                             "%.2f%%", ImGuiSliderFlags_AlwaysClamp);
    ch |= ImGui::SliderFloat("Curve##j", &shapeJunction_, 0.f, 1.f,
                             "equal gain < %.2f > equal power",
                             ImGuiSliderFlags_AlwaysClamp);
    ImGui::Spacing();
    ImGui::TextUnformatted("Traditional crossfade (end of loop)");
    ch |= ImGui::SliderFloat("Crossfade##e", &pctEnd_, 0.f, 100.f, "%.2f%%",
                             ImGuiSliderFlags_AlwaysClamp);
    ch |= ImGui::SliderFloat("Curve##e", &shapeEnd_, 0.f, 1.f,
                             "equal gain < %.2f > equal power",
                             ImGuiSliderFlags_AlwaysClamp);

    ImGui::Text("Resulting loop: %lld samples (%.1f ms)",
                (long long)outLength(),
                1000.0 * (double)outLength() / src_->sampleRate);
    if (ls_ <= 0)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "No audio before the loop start: the traditional "
                           "blend has no effect");
    return ch;
}

void PingPongCrossfade::renderInto(std::vector<std::vector<float>>& out) const {
    const int64_t L = le_ - ls_;
    const int64_t x1 = junctionLen();
    const int64_t total = 2 * L - x1;
    const int64_t x2 = endLen();

    const int nch = src_->channels();
    out.resize(nch);
    for (int c = 0; c < nch; ++c) {
        const float* s = src_->ch[c].data() + ls_;
        auto& v = out[c];
        v.assign((size_t)total, 0.f);
        // normal part
        for (int64_t i = 0; i < L - x1; ++i) v[(size_t)i] = s[i];
        // crossfaded junction: the end of the normal part blends with the
        // start of the reversed part (rev[j] = s[L-1-j])
        for (int64_t j = 0; j < x1; ++j) {
            float t = (float)(j + 1) / (float)(x1 + 1);
            float gIn, gOut;
            xfadeGains(t, shapeJunction_, gIn, gOut);
            v[(size_t)(L - x1 + j)] =
                s[L - x1 + j] * gOut + s[L - 1 - j] * gIn;
        }
        // rest of the reversed part
        for (int64_t j = x1; j < L; ++j)
            v[(size_t)(L + j - x1)] = s[L - 1 - j];
        // traditional crossfade at the end of the doubled loop, with the
        // material before the loop start (so wrapping end -> start is
        // continuous)
        for (int64_t j = 0; j < x2; ++j) {
            float t = (float)(j + 1) / (float)x2;
            float gIn, gOut;
            xfadeGains(t, shapeEnd_, gIn, gOut);
            float pre = src_->ch[c][(size_t)(ls_ - x2 + j)];
            size_t k = (size_t)(total - x2 + j);
            v[k] = v[k] * gOut + pre * gIn;
        }
    }
}

std::shared_ptr<AudioData> PingPongCrossfade::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    renderInto(d->ch);
    return d;
}

void PingPongCrossfade::apply(Document& doc) {
    std::vector<std::vector<float>> out;
    renderInto(out);
    int64_t total = (int64_t)out[0].size();
    doc.replaceRange(ls_, le_, std::move(out), ls_, ls_ + total,
                     "Ping Pong Crossfade");
}
