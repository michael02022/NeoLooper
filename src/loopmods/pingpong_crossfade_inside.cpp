#include "pingpong_crossfade_inside.hpp"

#include <algorithm>

#include "imgui.h"

int64_t PingPongCrossfadeInside::junctionLen() const {
    const int64_t L = le_ - ls_;
    const int64_t maxX1 = std::max<int64_t>(0, L - 1);
    return (int64_t)std::llround((double)maxX1 * pctJunction_ / 100.0);
}

int64_t PingPongCrossfadeInside::outLength() const {
    const int64_t L = le_ - ls_;
    return 2 * L - junctionLen();
}

int64_t PingPongCrossfadeInside::endLen() const {
    const int64_t L = le_ - ls_;
    const int64_t x1 = junctionLen();
    // capped at the unreversed normal part (the clean head, before the
    // normal<->reversed junction); this always fits within half of
    // outLength(), so the reference head never overlaps the tail being
    // blended (same reasoning as Classic Crossfade Inside's L/2, see the
    // .hpp).
    const int64_t maxX2 = std::max<int64_t>(0, L - x1);
    return (int64_t)std::llround((double)maxX2 * pctEnd_ / 100.0);
}

bool PingPongCrossfadeInside::drawParams() {
    bool ch = false;
    ImGui::TextUnformatted("Normal <-> reversed junction");
    ch |= ImGui::SliderFloat("Crossfade##j", &pctJunction_, 0.f, 100.f,
                             "%.2f%%", ImGuiSliderFlags_AlwaysClamp);
    ch |= ImGui::SliderFloat("Curve##j", &shapeJunction_, 0.f, 1.f,
                             "equal gain < %.2f > equal power",
                             ImGuiSliderFlags_AlwaysClamp);
    ImGui::Spacing();
    ImGui::TextUnformatted("Traditional crossfade (ends, inside the loop)");
    ch |= ImGui::SliderFloat("Crossfade##e", &pctEnd_, 0.f, 100.f, "%.2f%%",
                             ImGuiSliderFlags_AlwaysClamp);
    ch |= ImGui::SliderFloat("Curve##e", &shapeEnd_, 0.f, 1.f,
                             "equal gain < %.2f > equal power",
                             ImGuiSliderFlags_AlwaysClamp);

    const int64_t x2 = endLen();
    const int64_t total = outLength();
    ImGui::Text("Blend (ends): %lld samples (%.1f ms)", (long long)x2,
                1000.0 * (double)x2 / src_->sampleRate);
    ImGui::Text("Resulting loop: %lld samples (%.1f ms)",
                (long long)(total - x2),
                1000.0 * (double)(total - x2) / src_->sampleRate);
    if (x2 > 0)
        ImGui::TextDisabled(
            "The loop start advances %lld samples (that head then plays "
            "once, as an attack); the end of the doubled loop doesn't move.",
            (long long)x2);
    return ch;
}

void PingPongCrossfadeInside::renderFull(std::vector<std::vector<float>>& out) const {
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
        // crossfaded junction: normal->reversed (unchanged from the normal
        // variant, as the user asked)
        for (int64_t j = 0; j < x1; ++j) {
            float t = (float)(j + 1) / (float)(x1 + 1);
            float gIn, gOut;
            xfadeGains(t, shapeJunction_, gIn, gOut);
            v[(size_t)(L - x1 + j)] = s[L - x1 + j] * gOut + s[L - 1 - j] * gIn;
        }
        // rest of the reversed part
        for (int64_t j = x1; j < L; ++j)
            v[(size_t)(L + j - x1)] = s[L - 1 - j];
        // traditional crossfade at the end of the doubled loop, with the
        // loop's HEAD as reference (inside, instead of audio before ls_)
        for (int64_t j = 0; j < x2; ++j) {
            float t = (float)(j + 1) / (float)x2;
            float gIn, gOut;
            xfadeGains(t, shapeEnd_, gIn, gOut);
            float head = s[j]; // j < x2 <= L-x1: normal part, unblended
            size_t k = (size_t)(total - x2 + j);
            v[k] = v[k] * gOut + head * gIn;
        }
    }
}

std::shared_ptr<AudioData> PingPongCrossfadeInside::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    std::vector<std::vector<float>> full;
    renderFull(full);
    const int64_t x2 = endLen();
    const int nch = src_->channels();
    d->ch.resize(nch);
    for (int c = 0; c < nch; ++c)
        d->ch[c].assign(full[c].begin() + x2, full[c].end());
    return d;
}

void PingPongCrossfadeInside::apply(Document& doc) {
    std::vector<std::vector<float>> full;
    renderFull(full);
    const int64_t total = outLength();
    const int64_t x2 = endLen();
    doc.replaceRange(ls_, le_, std::move(full), ls_ + x2, ls_ + total,
                     "Ping Pong Crossfade (Inside)");
}
