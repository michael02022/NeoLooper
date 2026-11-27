#include "autotune_dsp.hpp"

#include <algorithm>
#include <memory>

#include "AutoTuneEngine.h"

namespace autotunedsp {

bool apply(const AudioData& src, int64_t s0, int64_t s1, const Spec& spec, Result& out,
           const std::atomic<bool>* cancel, std::atomic<float>* progress) {
    out = Result{};
    const int64_t frames = src.frames();
    s0 = std::clamp<int64_t>(s0, 0, frames);
    s1 = std::clamp<int64_t>(s1, 0, frames);
    if (s1 <= s0 || src.channels() < 1) return false;

    const int nch = std::min(2, src.channels());
    const double sr = src.sampleRate;
    const double octave = spec.lowNotes ? 2.0 : 1.0; // engine timeline / real timeline

    const int64_t ctx = (int64_t)(kContextSec * sr);
    const int64_t c0 = std::max<int64_t>(0, s0 - ctx);
    const int64_t c1 = std::min<int64_t>(frames, s1 + ctx);
    const int64_t len = c1 - c0;

    // ~70 KB of state (sinc table, frame FIFO): heap, not stack.
    auto engine = std::make_unique<snaptune::AutoTuneEngine>();
    snaptune::EngineSettings st;
    st.mode = snaptune::LatencyMode::studio;
    st.scale = snaptune::Scale::chromatic;
    st.key = 0;
    st.voice = snaptune::VoiceType::instrument;
    st.amount = std::clamp(spec.amountPct, 0.f, 100.f) * 0.01f;
    st.mix = 1.f;
    // Snaptune's default retune speed (20 ms), in the engine's timeline.
    st.retuneMs = (float)(20.0 / octave);
    st.midiTargets = spec.useMidi;
    engine->setSettings(st); // before prepare(): it picks the mode's latency
    engine->prepare(sr * octave, nch);
    if (spec.useMidi) engine->noteOn(std::clamp(spec.rootNote, 0, 127));

    const int64_t latency = engine->getLatencySamples();
    out.latency = latency;

    // Region + context, then 'latency' zeros to flush the engine.
    std::vector<std::vector<float>> buf((size_t)nch);
    for (int c = 0; c < nch; ++c) {
        buf[(size_t)c].assign((size_t)(len + latency), 0.f);
        std::copy(src.ch[(size_t)c].begin() + c0, src.ch[(size_t)c].begin() + c1,
                  buf[(size_t)c].begin());
    }
    float* ptrs[2] = {buf[0].data(), nch > 1 ? buf[1].data() : nullptr};

    std::vector<snaptune::PitchFrame> frameTmp(512);
    const int64_t total = len + latency;
    constexpr int kBlock = 512;
    for (int64_t pos = 0; pos < total; pos += kBlock) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            out = Result{};
            return false;
        }
        if (progress && (pos & 0xFFFF) == 0) progress->store((float)pos / (float)total);
        const int n = (int)std::min<int64_t>(kBlock, total - pos);
        engine->process(ptrs, nch, (int)pos, n);
        // Drain every block: the engine's frame FIFO holds 2048 hops.
        int got;
        while ((got = engine->popFrames(frameTmp.data(), (int)frameTmp.size())) > 0) {
            for (int i = 0; i < got; ++i) {
                const auto& f = frameTmp[(size_t)i];
                const int64_t p = c0 + (int64_t)f.time;
                if (p < s0 || p >= s1) continue;
                const float shift = spec.lowNotes ? 12.f : 0.f;
                PitchPoint pp;
                pp.pos = p;
                pp.voiced = f.voiced;
                pp.inputNote = f.inputNote - shift;
                pp.outputNote = f.outputNote - shift;
                pp.targetNote = f.targetNote >= 0 ? f.targetNote - (int)shift : -1;
                out.curve.push_back(pp);
            }
        }
    }
    std::sort(out.curve.begin(), out.curve.end(),
              [](const PitchPoint& a, const PitchPoint& b) { return a.pos < b.pos; });

    // The wet output of input sample i comes out at i + latency.
    const int64_t regionLen = s1 - s0;
    const int64_t fadeN = std::min<int64_t>((int64_t)(kEdgeFadeSec * sr), regionLen / 2);
    out.audio.assign((size_t)src.channels(), {});
    for (int c = 0; c < src.channels(); ++c) {
        const auto& wet = buf[(size_t)std::min(c, nch - 1)];
        const auto& dry = src.ch[(size_t)c];
        auto& o = out.audio[(size_t)c];
        o.resize((size_t)regionLen);
        for (int64_t i = 0; i < regionLen; ++i)
            o[(size_t)i] = wet[(size_t)(s0 - c0 + i + latency)];
        if (fadeN > 0) {
            if (s0 > 0)
                for (int64_t i = 0; i < fadeN; ++i) {
                    const float t = (float)(i + 1) / (float)(fadeN + 1);
                    o[(size_t)i] = dry[(size_t)(s0 + i)] * (1.f - t) + o[(size_t)i] * t;
                }
            if (s1 < frames)
                for (int64_t i = 0; i < fadeN; ++i) {
                    const int64_t k = regionLen - 1 - i;
                    const float t = (float)(i + 1) / (float)(fadeN + 1);
                    o[(size_t)k] = dry[(size_t)(s0 + k)] * (1.f - t) + o[(size_t)k] * t;
                }
        }
    }
    if (progress) progress->store(1.f);
    return true;
}

} // namespace autotunedsp
