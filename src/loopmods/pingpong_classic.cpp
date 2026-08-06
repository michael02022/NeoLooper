#include "pingpong_classic.hpp"

#include <algorithm>

#include "crossfade_inside_core.hpp"
#include "imgui.h"

int64_t PingPongClassic::junctionLen() const {
    const int64_t L = le_ - ls_;
    const int64_t maxX1 = std::max<int64_t>(0, L - 1);
    return (int64_t)std::llround((double)maxX1 * pctJunction_ / 100.0);
}

int64_t PingPongClassic::attackLen() const {
    const int64_t L = le_ - ls_;
    const int64_t x1 = junctionLen();
    // Half-length h of the SHARED R (R = mirror(normal[0,2h)), a single
    // piece split between the two crossfades below, never duplicated):
    // capped by the attack material actually available (h<=ls_), by R's
    // mirror source fitting within normal (2h<=L), and by leaving the
    // loop-start crossfade's own tail (normal's head) short of the
    // junction region (h<=L-x1).
    const int64_t maxH =
        std::min<int64_t>(std::max<int64_t>(0, ls_), std::min<int64_t>(L / 2, L - x1));
    return (int64_t)std::llround((double)std::max<int64_t>(0, maxH) * pctJunction_ / 100.0);
}

int64_t PingPongClassic::preLen() const {
    const int64_t L = le_ - ls_;
    const int64_t x1 = junctionLen();
    const int64_t h = attackLen();
    return h + L - x1 + x1 / 2;
}

bool PingPongClassic::drawParams() {
    bool ch = false;
    ImGui::TextWrapped(
        "The historically-accurate construction: ONE shared R (a raw, "
        "mirrored copy of a stretch of normal's own head), split -- never "
        "duplicated -- between the attack and the loop start itself, plus "
        "the normal<->reversed junction. The loop's own backward pass is "
        "then a MECHANICAL mirror of the finished loop content, not a "
        "separate blend -- exact continuity at the wrap, not approximate. "
        "Loop start and loop end never move.");

    ImGui::TextUnformatted("Attack + loop-start crossfades (share one R, split in half)");
    ch |= ImGui::SliderFloat("Crossfade##j", &pctJunction_, 0.f, 100.f,
                             "%.2f%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "R = a raw mirrored copy of normal[0,2h). Its FIRST half fades the "
        "attack's own tail toward it (before loop start); its SECOND half "
        "-- which ends exactly on normal[0] -- fades normal's own head "
        "toward it (right after loop start), a separate operation. The two "
        "halves are adjacent samples of R, so the seam at loop start stays "
        "natural. Also sizes the normal<->reversed junction crossfade.");
    ch |= ImGui::SliderFloat("Curve##j", &shapeJunction_, 0.f, 1.f,
                             "equal gain < %.2f > equal power",
                             ImGuiSliderFlags_AlwaysClamp);

    ImGui::Spacing();
    ch |= ImGui::Checkbox("Keep original loop bounds (native ping-pong)", &nativePingPongEnd_);
    ImGui::SetItemTooltip(
        "For samplers with their own ping-pong/alternating loop type: "
        "loop end sits at the exact MIDDLE of the junction crossfade "
        "(where normal meets reversed), mirroring loop start's own "
        "position at the middle of R's blend -- instead of the very end "
        "of the mirrored (backward-pass) construction. The hardware "
        "supplies the reverse pass itself; the full construction stays in "
        "the file as a reference of the complete cycle.");

    const int64_t x1 = junctionLen();
    const int64_t h = attackLen();
    const int64_t pre = preLen();
    const int64_t loopStartOff = h + h / 2;
    const int64_t porcionLen = pre - loopStartOff;
    const int64_t loopLen = nativePingPongEnd_ ? (pre - loopStartOff) : (pre + porcionLen - loopStartOff);
    ImGui::Text("R half-width x%lld | Junction x%lld (samples)", (long long)h, (long long)x1);
    ImGui::Text("Resulting loop: %lld samples (%.1f ms)", (long long)loopLen,
                1000.0 * (double)loopLen / src_->sampleRate);
    if (ls_ <= 0)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                           "No audio before the loop start: the attack "
                           "crossfade has no effect");
    return ch;
}

void PingPongClassic::renderInto(std::vector<std::vector<float>>& out) const {
    const int64_t L = le_ - ls_;
    const int64_t x1 = junctionLen();
    const int64_t halfX1 = x1 / 2;
    const int64_t h = attackLen();
    const int64_t pre = preLen(); // attack + loop-start crossfades + rest of normal + half-junction
    const int64_t loopStartOff = h + h / 2;
    const int64_t porcionLen = pre - loopStartOff;
    const int64_t total = pre + porcionLen;

    const int nch = src_->channels();
    out.resize(nch);
    for (int c = 0; c < nch; ++c) {
        const float* s = src_->ch[c].data() + ls_;
        auto& v = out[c];
        v.assign((size_t)total, 0.f);

        // R = mirror(normal[0,2h)), a SINGLE piece: R[k] = s[2h-1-k] for
        // k in [0,2h). Split in half below -- never recomputed/duplicated.

        // Attack crossfade [0, h), entirely BEFORE ls_: ccinside::
        // blendTail with tail=the attack's own real tail (what gets
        // modified) and head=R's FIRST half, R[0,h) = {s[2h-1],...,s[h]}.
        // Normal's own head is untouched here -- see the loop-start
        // crossfade below.
        if (h > 0) {
            std::vector<float> head((size_t)h), tail((size_t)h);
            for (int64_t m = 0; m < h; ++m) {
                head[(size_t)m] = s[2 * h - 1 - m];
                tail[(size_t)m] = src_->ch[c][(size_t)(ls_ - h + m)];
            }
            std::vector<float> blended = ccinside::blendTail(head, tail, shapeJunction_);
            for (int64_t m = 0; m < h; ++m) v[(size_t)m] = blended[(size_t)m];
        }

        // Loop-start crossfade [h, 2h), right AFTER ls_: ccinside::
        // blendTail with tail=R's SECOND half, R[h,2h) = {s[h-1],...,s[0]}
        // and head=normal's own real head (what gets modified this time).
        if (h > 0) {
            std::vector<float> head((size_t)h), tail((size_t)h);
            for (int64_t m = 0; m < h; ++m) {
                head[(size_t)m] = s[m];
                tail[(size_t)m] = s[h - 1 - m];
            }
            std::vector<float> blended = ccinside::blendTail(head, tail, shapeJunction_);
            for (int64_t m = 0; m < h; ++m) v[(size_t)(h + m)] = blended[(size_t)m];
        }

        // rest of normal, unblended (skips [0,h): already covered above)
        for (int64_t i = h; i < L - x1; ++i) v[(size_t)(h + i)] = s[i];

        // Junction crossfade: normal's tail fades into reversed's head --
        // but only the FIRST HALF is rendered here, up to its own
        // midpoint (halfX1 samples): that's where "pre" ends and loop end
        // sits with the native-ping-pong checkbox. The rest of the
        // crossfade is reconstructed automatically below, by mirroring.
        for (int64_t j = 0; j < halfX1; ++j) {
            float t = (float)(j + 1) / (float)(x1 + 1);
            float gIn, gOut;
            xfadeGains(t, shapeJunction_, gIn, gOut);
            v[(size_t)(h + L - x1 + j)] = s[L - x1 + j] * gOut + s[L - 1 - j] * gIn;
        }

        // The loop's backward pass: "porcion" (pre's own tail, from loop
        // start onward) mirrored and APPENDED right after pre. Since this
        // mirrors already-committed samples (not a fresh blend against
        // raw source), the wrap from the mirrored copy's own end back to
        // loop start lands EXACTLY on porcion[0] -- bit-exact, not an
        // approximation -- so no separate "end crossfade" is needed here.
        for (int64_t k = 0; k < porcionLen; ++k) v[(size_t)(pre + k)] = v[(size_t)(pre - 1 - k)];
    }
}

std::shared_ptr<AudioData> PingPongClassic::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;
    renderInto(d->ch);
    return d;
}

void PingPongClassic::apply(Document& doc) {
    std::vector<std::vector<float>> out;
    renderInto(out);
    const int64_t h = attackLen();
    const int64_t pre = preLen();
    // Loop start sits at the MIDDLE of R's tail (ls_+h/2), never ls_
    // itself. Loop end sits either at pre's own end (native ping-pong:
    // the junction crossfade's true midpoint) or at the very end of the
    // mirrored construction (default: forward-loop compatible).
    const int64_t loopStart = ls_ + h / 2;
    const int64_t loopEnd =
        ls_ - h + (nativePingPongEnd_ ? pre : (int64_t)out[0].size());
    doc.replaceRange(ls_ - h, le_, std::move(out), loopStart, loopEnd, "Ping Pong Classic");
}
