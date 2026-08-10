#include "engine.hpp"

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
    if (transport_.load() != Playing) transport_.store(Playing);
}

void Engine::pause() { transport_.store(Stopped); }

void Engine::playFromNote(int note) {
    curNote_.store(note);
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
                           bool looping, int64_t ls, int64_t le) const {
    int64_t i0 = (int64_t)std::floor(pos);
    float t = (float)(pos - (double)i0);
    float w[SincInterp16::kTaps];
    sinc_.weights(t, w);
    const int64_t loopLen = le - ls;
    // Only wrap the taps when the current position is ALREADY inside the
    // loop (not during the attack preceding ls, which plays once and must
    // sound exactly as-is, without mixing in content from the loop's end).
    const bool insideLoop = looping && loopLen > 0 && i0 >= ls && i0 < le;
    float acc = 0.f;
    for (int k = 0; k < SincInterp16::kTaps; ++k) {
        int64_t idx = i0 + (k - (SincInterp16::kHalf - 1));
        if (insideLoop && (idx < ls || idx >= le)) {
            int64_t rel = (idx - ls) % loopLen;
            if (rel < 0) rel += loopLen;
            idx = ls + rel;
        }
        acc += w[k] * d.at(c, idx);
    }
    return acc;
}

void Engine::process(const float* inL, const float* inR,
                     float* outL, float* outR, uint32_t n) {
    // FX passthrough: the input always passes through to the output
    for (uint32_t i = 0; i < n; ++i) {
        outL[i] = inL ? inL[i] : 0.f;
        outR[i] = inR ? inR[i] : (inL ? inL[i] : 0.f);
    }

    // refresh snapshot caches (never blocks)
    docSlot_.tryGet(rtDoc_);
    prevSlot_.tryGet(rtPrev_);
    docPrevSlot_.tryGet(rtDocPrev_);
    envSlot_.tryGet(rtEnv_);

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
    if (preview != wasPreview_) {
        wasPreview_ = preview;
        pos_ = 0.0;
    }

    const bool playing = (tr == Playing) && doc && doc->frames() > 0;
    const float gTarget = playing ? 1.f : 0.f;
    // ~3 ms anti-click ramp
    const float gCoef = 1.f - std::exp(-1.f / (0.003f * (float)hostSr));

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
        bool ended = false;
        for (uint32_t i = 0; i < n; ++i) {
            gain_ += (gTarget - gain_) * gCoef;
            float l = 0.f, r = 0.f;
            if (!ended) {
                l = sampleInterp(*doc, 0, pos_, looping, ls, le);
                r = (nch > 1) ? sampleInterp(*doc, 1, pos_, looping, ls, le) : l;
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
