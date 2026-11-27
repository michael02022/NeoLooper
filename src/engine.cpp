#include "engine.hpp"

#include "key_envelope.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

// ---------------------------------------------------------------- GUI side

void Engine::publishDocument(AudioDataPtr d) {
    graveyard.keep(d);
    docSlot_.set(std::move(d));
    graveyard.prune();
}

void Engine::publishPreview(AudioDataPtr d, int64_t loopOffset) {
    graveyard.keep(d);
    previewOn_.store(d != nullptr);
    previewLoopOffset_.store(loopOffset);
    prevSlot_.set(std::move(d));
    graveyard.prune();
}

void Engine::publishDocPreview(AudioDataPtr d) {
    graveyard.keep(d);
    docPreviewOn_.store(d != nullptr);
    docPrevSlot_.set(std::move(d));
    graveyard.prune();
}

void Engine::publishEnvelopes(EnvelopesPtr e) {
    envRetain_.push_back(e);
    if (envRetain_.size() > 8) envRetain_.pop_front();
    envSlot_.set(std::move(e));
}

void Engine::play() {
    curNote_.store(-1); // the Play button plays at the root note + finetune
    keyNote_.store(false); // ...and never through the piano envelope
    if (transport_.load() != Playing) transport_.store(Playing);
}

void Engine::pause() { transport_.store(Stopped); }

void Engine::playFromNote(int note) {
    curNote_.store(note);
    keyNote_.store(true);
    keySeq_.fetch_add(1); // restarts the piano envelope
    seek(0.0);
    transport_.store(Playing);
}

void Engine::seek(double frame) {
    seekPos_.store(frame);
    seekSeq_.fetch_add(1);
}

void Engine::startRecord() {
    freeChunks();
    recFrames_.store(0);
    recOverrun_.store(false);
    // two chunks ahead before starting
    for (int i = 0; i < 2; ++i)
        chunks_[i].store(new float[(size_t)kChunkFrames * 2]());
    chunksReady_.store(2, std::memory_order_release);
    transport_.store(Recording);
}

void Engine::pollRecording() {
    if (transport_.load() != Recording) return;
    int ready = chunksReady_.load();
    int64_t used = recFrames_.load() / kChunkFrames;
    while (ready < kMaxChunks && ready - (int)used < 2) {
        chunks_[ready].store(new float[(size_t)kChunkFrames * 2]());
        ++ready;
        chunksReady_.store(ready, std::memory_order_release);
    }
}

void Engine::waitAudioQuiescent() {
    // Waits for two generation bumps: guarantees a full block started (and
    // finished) after the transport change. If audio isn't running, the
    // timeout resolves it.
    uint64_t g0 = procGen_.load();
    int bumps = 0;
    for (int i = 0; i < 100 && bumps < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        uint64_t g = procGen_.load();
        if (g != g0) {
            ++bumps;
            g0 = g;
        }
    }
}

std::shared_ptr<AudioData> Engine::stopRecord() {
    transport_.store(Stopped);
    waitAudioQuiescent(); // the audio thread might still be mid-block
    int64_t n = recFrames_.load();
    if (n <= 0) {
        freeChunks();
        return nullptr;
    }
    auto d = std::make_shared<AudioData>();
    d->sampleRate = hostSr_.load();
    d->format = SampleFormat::Float32;
    d->ch.resize(2);
    d->ch[0].resize((size_t)n);
    d->ch[1].resize((size_t)n);
    for (int64_t i = 0; i < n; ++i) {
        float* c = chunks_[i / kChunkFrames].load();
        int64_t off = (i % kChunkFrames) * 2;
        d->ch[0][(size_t)i] = c[off];
        d->ch[1][(size_t)i] = c[off + 1];
    }
    freeChunks();
    d->buildPeaks();
    return d;
}

void Engine::freeChunks() {
    for (int i = 0; i < kMaxChunks; ++i) {
        delete[] chunks_[i].load();
        chunks_[i].store(nullptr);
    }
    chunksReady_.store(0);
    recFrames_.store(0);
}

// ---------------------------------------------------------------- RT side

void Engine::activate(double sampleRate) {
    hostSr_.store(sampleRate);
    gain_ = 0.f;
}

float Engine::sampleInterp(const AudioData& d, int c, double pos,
                           bool looping, int64_t ls, int64_t le,
                           PlaybackInterp mode) const {
    int64_t i0 = (int64_t)std::floor(pos);
    float t = (float)(pos - (double)i0);
    const int64_t loopLen = le - ls;
    // Only wrap the taps when the current position is ALREADY inside the
    // loop (not during the attack preceding ls, which plays once and must
    // sound exactly as-is, without mixing in content from the loop's end).
    const bool insideLoop = looping && loopLen > 0 && i0 >= ls && i0 < le;
    auto wrap = [&](int64_t idx) {
        if (insideLoop && (idx < ls || idx >= le)) {
            int64_t rel = (idx - ls) % loopLen;
            if (rel < 0) rel += loopLen;
            idx = ls + rel;
        }
        return idx;
    };
    switch (mode) {
    case PlaybackInterp::None:
        return d.at(c, i0);
    case PlaybackInterp::Linear: {
        float a = d.at(c, i0), b = d.at(c, wrap(i0 + 1));
        return a + t * (b - a);
    }
    case PlaybackInterp::Cubic: {
        // Catmull-Rom spline through x[i0-1..i0+2]: passes through every
        // sample, continuous slope (the classic "cubic" of samplers/trackers).
        const float x0 = d.at(c, wrap(i0 - 1)), x1 = d.at(c, i0);
        const float x2 = d.at(c, wrap(i0 + 1)), x3 = d.at(c, wrap(i0 + 2));
        const float a1 = 0.5f * (x2 - x0);
        const float a2 = x0 - 2.5f * x1 + 2.f * x2 - 0.5f * x3;
        const float a3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
        return ((a3 * t + a2) * t + a1) * t + x1;
    }
    case PlaybackInterp::Sinc16:
        break;
    }
    float w[SincInterp16::kTaps];
    sinc_.weights(t, w);
    float acc = 0.f;
    for (int k = 0; k < SincInterp16::kTaps; ++k)
        acc += w[k] * d.at(c, wrap(i0 + (k - (SincInterp16::kHalf - 1))));
    return acc;
}

void Engine::process(const float* inL, const float* inR,
                     float* outL, float* outR, uint32_t n) {
    // FX passthrough: the input passes through to the output (off in the
    // standalone, see setPassThrough)
    const bool thru = passThrough_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
        outL[i] = thru && inL ? inL[i] : 0.f;
        outR[i] = thru ? (inR ? inR[i] : (inL ? inL[i] : 0.f)) : 0.f;
    }

    // refresh snapshot caches (never blocks)
    docSlot_.tryGet(rtDoc_);
    prevSlot_.tryGet(rtPrev_);
    docPrevSlot_.tryGet(rtDocPrev_);
    envSlot_.tryGet(rtEnv_);
    const float monTarget = monitorGainTarget_.load(std::memory_order_relaxed);
    const float monStep = 1.f - std::exp(-1.f / (0.005f * (float)hostSr_.load()));

    const int tr = transport_.load();
    const double hostSr = hostSr_.load();

    // --- recording ---
    if (tr == Recording) {
        int64_t w = recFrames_.load();
        int ready = chunksReady_.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i) {
            int64_t ci = w / kChunkFrames;
            if (ci >= ready || ci >= kMaxChunks) {
                recOverrun_.store(true);
                break;
            }
            float* c = chunks_[ci].load(std::memory_order_relaxed);
            int64_t off = (w % kChunkFrames) * 2;
            c[off] = inL ? inL[i] : 0.f;
            c[off + 1] = inR ? inR[i] : (inL ? inL[i] : 0.f);
            ++w;
        }
        recFrames_.store(w, std::memory_order_release);
    }

    // --- playback ---
    const bool preview = previewOn_.load() && rtPrev_ && rtPrev_->frames() > 0;
    // Document preview (Filter): behaves exactly like the real document in
    // EVERYTHING (markers, envelopes, transport); only where samples are
    // read from changes. Module preview takes priority.
    const bool docPreview =
        !preview && docPreviewOn_.load() && rtDocPrev_ && rtDocPrev_->frames() > 0;
    const AudioData* doc = preview      ? rtPrev_.get()
                           : docPreview ? rtDocPrev_.get()
                                        : (rtDoc_ ? rtDoc_.get() : nullptr);

    // pending seek / switch to preview mode
    uint32_t seq = seekSeq_.load();
    if (seq != seekSeen_) {
        seekSeen_ = seq;
        pos_ = seekPos_.load();
    }
    uint32_t kseq = keySeq_.load();
    if (kseq != keySeen_) {
        keySeen_ = kseq;
        keyEnvT_ = 0.0;
    }
    if (preview != wasPreview_) {
        wasPreview_ = preview;
        pos_ = 0.0;
    }

    const bool playing = (tr == Playing) && doc && doc->frames() > 0;
    const float gTarget = playing ? 1.f : 0.f;
    // ~3 ms anti-click ramp
    const float gCoef = 1.f - std::exp(-1.f / (0.003f * (float)hostSr));
    // ...except when playback starts at the very first sample: that's the
    // sample's own attack (Play with no selection, a piano key, a preview
    // restart), and a sampler plays it as-is -- the ramp would soften the
    // transient (a hard "pop" turned into a small fade-in). Resuming
    // mid-sample keeps the ramp, where it does prevent a click; so does
    // stopping.
    if (playing && !wasPlaying_ && pos_ < 0.5) gain_ = 1.f;
    wasPlaying_ = playing;

    if (doc && doc->frames() > 0 && (playing || gain_ > 1e-4f)) {
        const int note = curNote_.load();
        const int root = params.rootNote.load();
        const float cents = params.tuneCents.load();
        const double semis = (note < 0 ? 0.0 : (double)(note - root)) +
                             (double)cents / 100.0;
        const double ratio = std::pow(2.0, semis / 12.0) *
                            (doc->sampleRate / hostSr) *
                            (double)params.playbackSpeed.load();

        // effective loop region
        int64_t ls = 0, le = doc->frames();
        bool looping;
        if (preview) {
            looping = true; // module preview always loops
            // If the module published an offset > 0, everything before it
            // (e.g. Harmonic's entry crossfade) is "attack": it plays once
            // (pos_ starts at 0 when entering preview, see below) and only
            // [offset, frames()) repeats forever -- the same mechanism as a
            // marked loop during normal playback, not a new one. offset=0
            // (Classic/PingPong) loops the whole buffer, same as before.
            int64_t off = previewLoopOffset_.load();
            if (off > 0 && off < le) ls = off;
        } else {
            looping = params.loopEnabled.load();
            int64_t mls = params.loopStart.load(), mle = params.loopEnd.load();
            int64_t ssl = params.selStart.load(), sse = params.selEnd.load();
            // Selection wins over a marked loop when both exist (matches
            // EditorUI::playPressed()'s own priority for where playback
            // starts -- a selection means "audition just this", overriding
            // whatever loop happens to be marked). A marked-but-disabled
            // loop must NOT clip playback to its end -- only an active loop
            // governs where playback stops/wraps; otherwise it plays to the
            // true end of the sample.
            if (ssl >= 0 && sse > ssl) {
                ls = ssl; le = sse;
            } else if (looping && mls >= 0 && mle > mls) {
                ls = mls; le = mle;
            }
            le = std::min(le, doc->frames());
            ls = std::min(ls, le - 1 < 0 ? 0 : le - 1);
        }

        const int nch = doc->channels();
        // Envelopes being edited: audible live over the document (not over a
        // module preview, which is separate audio).
        const Envelopes* envp = (!preview && rtEnv_) ? rtEnv_.get() : nullptr;
        const bool envAmp = envp && envp->amp.isActive();
        const bool envPan = envp && envp->pan.isActive() && nch > 1;
        const bool envSep = envp && envp->sep.isActive() && nch > 1;
        const bool envPitch = envp && envp->pitch.isActive();
        // Piano preview envelope (key_envelope.hpp), in real time since the
        // key press; when it reaches silence playback stops like at the end.
        const bool keyEnv = keyNote_.load() && params.keyEnvEnabled.load();
        const auto keyCurve = (keyenv::Curve)std::clamp(
            params.keyEnvCurve.load(), 0, keyenv::kCurveCount - 1);
        const double keyHold = std::max(0.f, params.keyEnvHoldMs.load()) * 1e-3;
        const double keyDecay = std::max(0.f, params.keyEnvDecayMs.load()) * 1e-3;
        const double keyDt = 1.0 / hostSr;
        const auto interp = (PlaybackInterp)std::clamp(params.playbackInterp.load(), 0,
                                                       kPlaybackInterpCount - 1);
        bool ended = false;
        for (uint32_t i = 0; i < n; ++i) {
            gain_ += (gTarget - gain_) * gCoef;
            float l = 0.f, r = 0.f;
            if (!ended) {
                l = sampleInterp(*doc, 0, pos_, looping, ls, le, interp);
                r = (nch > 1) ? sampleInterp(*doc, 1, pos_, looping, ls, le, interp) : l;
                if (envAmp) {
                    float a = envp->amp.valueAt(pos_);
                    l *= a;
                    r *= a;
                }
                if (envSep) {
                    float s = envp->sep.valueAt(pos_);
                    float mid = 0.5f * (l + r);
                    float side = 0.5f * (l - r) * (1.f - s);
                    l = mid + side;
                    r = mid - side;
                }
                if (keyEnv && playing) {
                    bool done = false;
                    float g = keyenv::gain(keyCurve, keyEnvT_, keyHold, keyDecay, done);
                    keyEnvT_ += keyDt;
                    l *= g;
                    r *= g;
                    if (done) ended = true;
                }
                if (envPan) {
                    float p = envp->pan.valueAt(pos_);
                    if (p > 0.f) l *= std::cos(p * (float)M_PI * 0.5f);
                    else if (p < 0.f) r *= std::cos(-p * (float)M_PI * 0.5f);
                }
                // Pitch envelope: varispeed, same mechanism as Tune/pitch-
                // bend (the ratio above) -- it modulates how much pos_
                // advances per sample instead of touching the already-read
                // interpolation, so Apply (Document::applyEnvelopes) can
                // reproduce this exact same computation offline.
                double r = ratio;
                if (envPitch) r *= std::pow(2.0, (double)envp->pitch.valueAt(pos_) / 1200.0);
                pos_ += playing ? r : 0.0;
                if (looping && le > ls) {
                    if (pos_ >= (double)le)
                        pos_ = (double)ls + std::fmod(pos_ - (double)ls,
                                                      (double)(le - ls));
                } else if (pos_ >= (double)le) {
                    // le is doc->frames() by default, but narrows to an
                    // active selection above (regardless of loop state) --
                    // so playback stops there instead of running past it to
                    // the end of the whole document. A marked-but-disabled
                    // loop does NOT narrow le (see above), so this only
                    // fires here for a selection.
                    ended = true;
                }
            }
            l *= gain_;
            r *= gain_;
            // Monitoring gain (see setMonitorGain): ~5 ms ramp, clamp while boosting.
            monitorGain_ += (monTarget - monitorGain_) * monStep;
            if (monitorGain_ != 1.f) {
                l *= monitorGain_;
                r *= monitorGain_;
                if (monitorGain_ > 1.f) {
                    l = std::clamp(l, -1.f, 1.f);
                    r = std::clamp(r, -1.f, 1.f);
                }
            }
            outL[i] += l;
            outR[i] += r;
            // oscilloscope: mono mix of what was played
            int64_t sw = scopeWrite_.load(std::memory_order_relaxed);
            scopeRing_[(size_t)(sw & (kScopeSize - 1))] = 0.5f * (l + r);
            scopeWrite_.store(sw + 1, std::memory_order_release);
        }
        if (ended && playing) {
            transport_.store(Stopped);
            pos_ = 0.0;
            curNote_.store(-1);
        }
    }

    uiPlayhead_.store(pos_);
    procGen_.fetch_add(1, std::memory_order_release);
}
