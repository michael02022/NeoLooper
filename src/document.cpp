#include "document.hpp"

#include <algorithm>
#include <cmath>

#include "resampler.hpp"
#include "sinc_interp.hpp"

namespace {
// Interpolated read (16-point sinc, same quality as live playback -- see
// Engine::sampleInterp) out of range = silence, same criterion as
// AudioData::at(). Only used from the GUI thread (constructing SincInterp16
// allocates memory, not RT-safe), like the rest of this project's spectral
// modules.
float sincReadSample(const std::vector<float>& x, double pos, const SincInterp16& sinc) {
    const int64_t i0 = (int64_t)std::floor(pos);
    const float t = (float)(pos - (double)i0);
    float w[SincInterp16::kTaps];
    sinc.weights(t, w);
    float acc = 0.f;
    for (int k = 0; k < SincInterp16::kTaps; ++k) {
        const int64_t idx = i0 + (k - (SincInterp16::kHalf - 1));
        if (idx >= 0 && idx < (int64_t)x.size()) acc += w[k] * x[(size_t)idx];
    }
    return acc;
}
} // namespace

Document::Document(SharedParams& params, std::function<void(AudioDataPtr)> publish)
    : params_(params), publish_(std::move(publish)) {}

// ---------------------------------------------------------------- snapshots

Document::Snapshot Document::makeSnapshot(const char* op) const {
    return Snapshot{cur_, params_.loopStart.load(), params_.loopEnd.load(),
                    params_.rootNote.load(), params_.tuneCents.load(),
                    env, sampleName_, op ? op : ""};
}

void Document::restoreSnapshot(const Snapshot& s) {
    cur_ = s.data;
    params_.loopStart.store(s.loopStart);
    params_.loopEnd.store(s.loopEnd);
    params_.rootNote.store(s.rootNote);
    params_.tuneCents.store(s.tuneCents);
    env = s.env;
    sampleName_ = s.sampleName;
    clearSelection();
    publish_(cur_);
}

void Document::pushEnvUndo(const char* opName) { pushUndo(opName); }

void Document::pushUndo(const char* opName) {
    undo_.push_back(makeSnapshot(opName));
    if (undo_.size() > kMaxUndo) undo_.pop_front();
    redo_.clear();
}

void Document::undo() {
    if (undo_.empty()) return;
    Snapshot s = undo_.back();
    undo_.pop_back();
    redo_.push_back(makeSnapshot(s.opName.c_str()));
    restoreSnapshot(s);
    toast("Undo: " + s.opName);
}

void Document::redo() {
    if (redo_.empty()) return;
    Snapshot s = redo_.back();
    redo_.pop_back();
    undo_.push_back(makeSnapshot(s.opName.c_str()));
    restoreSnapshot(s);
    toast("Redo: " + s.opName);
}

// ---------------------------------------------------------------- helpers

void Document::finalizeFormat(AudioData& d) {
    if (d.format == SampleFormat::Int16) {
        float peak = d.peakAbs();
        if (peak > kInt16Max + 1e-9f) {
            d.format = SampleFormat::Float32;
            toast("Amplitude out of 16-bit range: converted to 32-bit");
        } else {
            for (auto& v : d.ch)
                for (auto& s : v) s = quantize16(s);
        }
    }
}

void Document::commit(std::shared_ptr<AudioData> d, const char* opName,
                      bool keepLoop) {
    pushUndo(opName);
    finalizeFormat(*d);
    d->buildPeaks();
    cur_ = std::move(d);
    if (!keepLoop) {
        params_.loopStart.store(-1);
        params_.loopEnd.store(-1);
    }
    clampMarkersToLength();
    publish_(cur_);
}

void Document::clampMarkersToLength() {
    int64_t n = frames();
    auto clampPair = [n](std::atomic<int64_t>& a, std::atomic<int64_t>& b) {
        int64_t s = a.load(), e = b.load();
        if (s < 0 || n == 0) { a.store(-1); b.store(-1); return; }
        s = std::min(s, n - 1);
        e = std::min(e, n);
        if (e <= s) { a.store(-1); b.store(-1); return; }
        a.store(s);
        b.store(e);
    };
    clampPair(params_.loopStart, params_.loopEnd);
    clampPair(params_.selStart, params_.selEnd);
}

void Document::toast(const std::string& msg) { toast_ = msg; }

std::string Document::takeToast() {
    std::string t = std::move(toast_);
    toast_.clear();
    return t;
}

bool Document::effectiveLoop(int64_t& s, int64_t& e) const {
    if (hasLoop()) {
        s = loopStart();
        e = loopEnd();
        return true;
    }
    if (hasSelection()) {
        s = selStart();
        e = selEnd();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- metadata

void Document::setRootNote(int n) {
    params_.rootNote.store(std::min(127, std::max(0, n)));
}

void Document::setTuneCents(float c) {
    params_.tuneCents.store(std::min(99.f, std::max(-99.f, c)));
}

void Document::setSelection(int64_t s, int64_t e) {
    if (s < 0 || e <= s) {
        params_.selStart.store(-1);
        params_.selEnd.store(-1);
        return;
    }
    int64_t n = frames();
    s = std::max<int64_t>(0, std::min(s, n));
    e = std::max<int64_t>(0, std::min(e, n));
    params_.selStart.store(s);
    params_.selEnd.store(e);
}

void Document::setLoopPoints(int64_t s, int64_t e, bool undoable) {
    if (undoable) pushUndo("Set loop");
    int64_t n = frames();
    s = std::max<int64_t>(0, std::min(s, n - 1));
    e = std::max<int64_t>(s + 1, std::min(e, n)); // end never <= start
    params_.loopStart.store(s);
    params_.loopEnd.store(e);
}

void Document::clearLoopPoints(bool undoable) {
    if (undoable) pushUndo("Remove loop");
    params_.loopStart.store(-1);
    params_.loopEnd.store(-1);
}

// ---------------------------------------------------------------- loading

void Document::setAudio(AudioDataPtr d, const char* opName) {
    pushUndo(opName);
    cur_ = std::move(d);
    params_.selStart.store(-1);
    params_.selEnd.store(-1);
    clampMarkersToLength();
    fullViewRequested_ = true;
    publish_(cur_);
}

bool Document::consumeFullViewRequest() {
    bool v = fullViewRequested_;
    fullViewRequested_ = false;
    return v;
}

void Document::appendAudio(AudioDataPtr rec, const char* opName) {
    if (!rec || rec->frames() == 0) return;
    if (!hasAudio()) {
        setAudio(std::move(rec), opName);
        return;
    }
    // The recording arrives at the host's sample rate; it's adapted to the
    // document's so pitch/speed don't change when concatenating.
    std::vector<std::vector<float>> chans = rec->ch;
    if (rec->sampleRate != cur_->sampleRate)
        chans = resampleHQ(chans, rec->sampleRate, cur_->sampleRate);
    const int64_t recLen = chans.empty() ? 0 : (int64_t)chans[0].size();
    if (recLen == 0) return;

    auto d = std::make_shared<AudioData>(*cur_);
    const int dstCh = d->channels();
    const int srcCh = (int)chans.size();
    const int64_t oldLen = d->frames();
    for (int c = 0; c < dstCh; ++c) {
        auto& v = d->ch[c];
        v.reserve(v.size() + (size_t)recLen);
        for (int64_t i = 0; i < recLen; ++i) {
            float s;
            if (dstCh == 1 && srcCh > 1)
                s = 0.5f * (chans[0][i] + chans[1][i]); // mono doc: mix down
            else
                s = chans[std::min(c, srcCh - 1)][i];
            v.push_back(s);
        }
    }
    // Markers stay intact: the new audio goes after everything.
    commit(std::move(d), opName);
    setSelection(oldLen, oldLen + recLen);
    toast("Recording appended: +" + std::to_string(recLen) + " samples");
}

void Document::loadFromState(AudioDataPtr d, int64_t loopStart, int64_t loopEnd,
                             int rootNote, float tuneCents,
                             const std::string& sampleName) {
    undo_.clear();
    redo_.clear();
    clipboard_.reset();
    env.clearAll();
    sampleName_ = sampleName;
    cur_ = std::move(d);
    params_.rootNote.store(std::min(127, std::max(0, rootNote)));
    params_.tuneCents.store(std::min(99.f, std::max(-99.f, tuneCents)));
    params_.loopStart.store(loopStart);
    params_.loopEnd.store(loopEnd);
    params_.selStart.store(-1);
    params_.selEnd.store(-1);
    clampMarkersToLength();
    fullViewRequested_ = true;
    publish_(cur_);
}

void Document::reset() {
    cur_.reset();
    clipboard_.reset();
    env.clearAll();
    sampleName_.clear();
    undo_.clear();
    redo_.clear();
    params_.loopStart.store(-1);
    params_.loopEnd.store(-1);
    params_.selStart.store(-1);
    params_.selEnd.store(-1);
    publish_(nullptr);
}

// ---------------------------------------------------------------- editing

bool Document::copySelection() {
    if (!hasAudio() || !hasSelection()) return false;
    int64_t s = selStart(), e = selEnd();
    auto clip = std::make_shared<AudioData>();
    clip->sampleRate = cur_->sampleRate;
    clip->format = cur_->format;
    clip->ch.resize(cur_->channels());
    for (int c = 0; c < cur_->channels(); ++c)
        clip->ch[c].assign(cur_->ch[c].begin() + s, cur_->ch[c].begin() + e);
    clipboard_ = clip;
    toast("Copied: " + std::to_string(e - s) + " samples");
    return true;
}

bool Document::deleteSelection() {
    if (!hasAudio() || !hasSelection()) return false;
    int64_t s = selStart(), e = selEnd(), removed = e - s;
    auto d = std::make_shared<AudioData>(*cur_);
    for (auto& v : d->ch)
        v.erase(v.begin() + s, v.begin() + e);
    // Loop markers shift along with the audio.
    auto shift = [s, e, removed](int64_t p) {
        if (p < 0) return p;
        if (p >= e) return p - removed;
        if (p > s) return s;
        return p;
    };
    int64_t ls = shift(params_.loopStart.load());
    int64_t le = shift(params_.loopEnd.load());
    if (le - ls < 1) { ls = -1; le = -1; }
    commit(std::move(d), "Delete");
    params_.loopStart.store(ls);
    params_.loopEnd.store(le);
    clampMarkersToLength();
    clearSelection();
    return true;
}

bool Document::cutSelection() {
    if (!copySelection()) return false;
    bool ok = deleteSelection();
    if (ok) toast("Cut");
    return ok;
}

bool Document::paste() {
    if (!clipboard_ || !hasAudio()) {
        if (clipboard_ && !hasAudio()) {
            // Pasting into an empty document = the clipboard becomes the audio.
            auto d = std::make_shared<AudioData>(*clipboard_);
            d->buildPeaks();
            setAudio(d, "Paste");
            return true;
        }
        return false;
    }
    int64_t at = hasSelection() ? selStart() : frames();
    auto d = std::make_shared<AudioData>(*cur_);
    int dstCh = d->channels();
    int srcCh = clipboard_->channels();
    int64_t n = clipboard_->frames();
    for (int c = 0; c < dstCh; ++c) {
        std::vector<float> piece((size_t)n);
        for (int64_t i = 0; i < n; ++i) {
            if (srcCh == dstCh) {
                piece[i] = clipboard_->ch[c][i];
            } else if (srcCh == 1) {
                piece[i] = clipboard_->ch[0][i]; // mono -> both channels
            } else {
                // stereo -> mono: mix down
                float acc = 0.f;
                for (int sc = 0; sc < srcCh; ++sc) acc += clipboard_->ch[sc][i];
                piece[i] = acc / srcCh;
            }
        }
        d->ch[c].insert(d->ch[c].begin() + at, piece.begin(), piece.end());
    }
    // Shifts markers past the insertion point.
    auto shift = [at, n](int64_t p) { return (p >= 0 && p >= at) ? p + n : p; };
    int64_t ls = shift(params_.loopStart.load());
    int64_t le = shift(params_.loopEnd.load());
    commit(std::move(d), "Paste");
    params_.loopStart.store(ls);
    params_.loopEnd.store(le);
    clampMarkersToLength();
    setSelection(at, at + n);
    return true;
}

void Document::normalize() {
    if (!hasAudio()) return;
    // With an active selection only that region is normalized; otherwise, everything.
    int64_t s0 = 0, s1 = frames();
    bool selOnly = hasSelection();
    if (selOnly) {
        s0 = selStart();
        s1 = selEnd();
    }
    auto d = std::make_shared<AudioData>(*cur_);
    float peak = 0.f;
    for (auto& v : d->ch)
        for (int64_t i = s0; i < s1; ++i)
            peak = std::max(peak, std::fabs(v[(size_t)i]));
    if (peak <= 0.f) return;
    // At 16 bit the ceiling is 32767/32768; at 32 bit, 1.0.
    float target = (d->format == SampleFormat::Int16) ? kInt16Max : 1.0f;
    float g = target / peak;
    for (auto& v : d->ch)
        for (int64_t i = s0; i < s1; ++i) v[(size_t)i] *= g;
    commit(std::move(d), selOnly ? "Normalize selection" : "Normalize");
    toast(selOnly ? "Selection normalized" : "Normalized");
}

void Document::reverse() {
    if (!hasAudio()) return;
    // With an active selection only that region is reversed; otherwise, everything.
    int64_t s0 = 0, s1 = frames();
    bool selOnly = hasSelection();
    if (selOnly) {
        s0 = selStart();
        s1 = selEnd();
    }
    if (s1 <= s0) return;
    auto d = std::make_shared<AudioData>(*cur_);
    for (auto& v : d->ch) std::reverse(v.begin() + s0, v.begin() + s1);
    // Duration doesn't change -> commit()/clampMarkersToLength() don't move
    // loop start/end, they stay exactly where they were.
    commit(std::move(d), selOnly ? "Reverse selection" : "Reverse");
    toast(selOnly ? "Selection reversed" : "Sample reversed");
}

void Document::applyFilter(const filterdsp::FilterSpec& spec) {
    if (!hasAudio()) return;
    int64_t s0 = 0, s1 = frames();
    bool selOnly = hasSelection();
    if (selOnly) {
        s0 = selStart();
        s1 = selEnd();
    }
    if (s1 <= s0) return;

    auto processed = filterdsp::apply(*cur_, s0, s1, spec);
    if (processed.empty()) return;

    // Duration doesn't change (in-place filtering, same criterion as
    // reverse()): loop start/end stay intact with no need to remap them.
    auto d = std::make_shared<AudioData>(*cur_);
    for (int c = 0; c < d->channels(); ++c)
        std::copy(processed[(size_t)c].begin(), processed[(size_t)c].end(),
                  d->ch[(size_t)c].begin() + s0);
    commit(std::move(d), selOnly ? "Filter (selection)" : "Filter");
    toast(selOnly ? "Filter applied to selection" : "Filter applied");
}

void Document::applyAmplitude(const amplitudedsp::AmplitudeSpec& spec) {
    if (!hasAudio()) return;
    int64_t s0 = 0, s1 = frames();
    bool selOnly = hasSelection();
    if (selOnly) {
        s0 = selStart();
        s1 = selEnd();
    }
    if (s1 <= s0) return;

    auto processed = amplitudedsp::apply(*cur_, s0, s1, spec);
    if (processed.empty()) return;

    // Duration doesn't change (in-place gain, same criterion as
    // applyFilter): loop start/end stay intact with no need to remap them.
    auto d = std::make_shared<AudioData>(*cur_);
    for (int c = 0; c < d->channels(); ++c)
        std::copy(processed[(size_t)c].begin(), processed[(size_t)c].end(),
                  d->ch[(size_t)c].begin() + s0);
    commit(std::move(d), selOnly ? "Amplitude (selection)" : "Amplitude");
    toast(selOnly ? "Amplitude applied to selection" : "Amplitude applied");
}

void Document::spliceAndRemap(int64_t s0, int64_t s1, std::vector<std::vector<float>>&& piece,
                              const char* opName) {
    const int64_t oldLen = s1 - s0;
    const int64_t newLen = piece.empty() ? 0 : (int64_t)piece[0].size();
    const int64_t delta = newLen - oldLen;

    // Erase [s0,s1) and insert 'piece' in its place -- same erase+insert
    // shape as replaceRange/insertPiece, needed here (rather than a
    // same-length std::copy like applyFilter) because the region's length
    // can change.
    auto d = std::make_shared<AudioData>(*cur_);
    for (int c = 0; c < d->channels(); ++c) {
        auto& v = d->ch[(size_t)c];
        const auto& p = piece[std::min((size_t)c, piece.size() - 1)];
        v.erase(v.begin() + s0, v.begin() + s1);
        v.insert(v.begin() + s0, p.begin(), p.end());
    }

    // Loop markers: untouched before s0, shifted by 'delta' past s1 (same
    // idea as paste()'s marker shift), and RESCALED proportionally when
    // they fall inside the edited region -- same idea as changeSampleRate's
    // whole-document rescale, just scoped to [s0,s1).
    auto remap = [&](int64_t pos) -> int64_t {
        if (pos < 0 || pos <= s0) return pos;
        if (pos >= s1) return pos + delta;
        const double t = (double)(pos - s0) / (double)oldLen;
        return s0 + (int64_t)std::llround(t * (double)newLen);
    };
    const int64_t newLs = remap(params_.loopStart.load());
    const int64_t newLe = remap(params_.loopEnd.load());

    commit(std::move(d), opName);
    // Written AFTER commit(): commit()'s pushUndo() must capture the OLD
    // loopStart_/loopEnd_ values, same ordering rule as applyEnvelopes'
    // marker remap (writing them earlier would make undo inconsistent).
    params_.loopStart.store(newLs);
    params_.loopEnd.store(newLe);
    clampMarkersToLength();
    setSelection(s0, s0 + newLen);
}

void Document::applyPitch(const PitchRenderFn& render, const char* methodName, bool resetTune) {
    if (!hasAudio()) return;
    int64_t s0 = 0, s1 = frames();
    bool selOnly = hasSelection();
    if (selOnly) {
        s0 = selStart();
        s1 = selEnd();
    }
    if (s1 <= s0) return;

    auto piece = render(*cur_, s0, s1);
    if (piece.empty()) return;
    std::string opName = std::string(methodName) + (selOnly ? " (selection)" : "");
    // spliceAndRemap degrades to a plain same-length in-place replace (no
    // marker rescale) when the piece's length matches the input region --
    // which is every method except the built-in one's traditional mode --
    // so it's safe to always go through it here, same as applyStretch.
    spliceAndRemap(s0, s1, std::move(piece), opName.c_str());
    // Written AFTER the commit() inside spliceAndRemap(): must capture the
    // OLD tuneCents_ for undo, same ordering rule as applyEnvelopes'
    // marker remap.
    if (resetTune) params_.tuneCents.store(0.f);
    toast(std::string(methodName) + (selOnly ? " applied to selection" : " applied"));
}

void Document::applyStretch(const StretchRenderFn& render, const char* methodName) {
    if (!hasAudio()) return;
    int64_t s0 = 0, s1 = frames();
    bool selOnly = hasSelection();
    if (selOnly) {
        s0 = selStart();
        s1 = selEnd();
    }
    if (s1 <= s0) return;

    auto piece = render(*cur_, s0, s1);
    if (piece.empty()) return;
    std::string opName = std::string(methodName) + (selOnly ? " (selection)" : "");
    spliceAndRemap(s0, s1, std::move(piece), opName.c_str());
    toast(std::string(methodName) + (selOnly ? " applied to selection" : " applied"));
}

void Document::toMono(int mode) {
    if (!hasAudio() || cur_->channels() < 2) return;
    auto d = std::make_shared<AudioData>();
    d->sampleRate = cur_->sampleRate;
    d->format = cur_->format;
    d->ch.resize(1);
    int64_t n = cur_->frames();
    d->ch[0].resize((size_t)n);
    for (int64_t i = 0; i < n; ++i) {
        float s;
        if (mode == 0) s = cur_->ch[0][i];
        else if (mode == 1) s = cur_->ch[1][i];
        else s = 0.5f * (cur_->ch[0][i] + cur_->ch[1][i]);
        d->ch[0][i] = s;
    }
    const char* names[] = {"To mono (L channel)", "To mono (R channel)", "To mono (mix)"};
    commit(std::move(d), names[std::min(2, std::max(0, mode))]);
}

void Document::changeSampleRate(double sr, bool resample) {
    if (!hasAudio() || sr < 1000 || sr > 768000) return;
    double oldSr = cur_->sampleRate;
    if (sr == oldSr) return;
    auto d = std::make_shared<AudioData>(*cur_);
    if (resample) {
        d->ch = resampleHQ(cur_->ch, oldSr, sr);
        d->sampleRate = sr;
        // Relocates markers to the new time scale.
        double k = sr / oldSr;
        auto scale = [k](std::atomic<int64_t>& p) {
            int64_t v = p.load();
            if (v >= 0) p.store((int64_t)std::llround(v * k));
        };
        scale(params_.loopStart);
        scale(params_.loopEnd);
        commit(std::move(d), "Resample");
        clampMarkersToLength();
        toast("Resampled to " + std::to_string((int)sr) + " Hz (HQ)");
    } else {
        d->sampleRate = sr;
        commit(std::move(d), "Change sample rate");
        toast("Sample rate: " + std::to_string((int)sr) +
              " Hz (no resampling: changes speed/pitch)");
    }
    clearSelection();
}

void Document::printTuneToSampleRate() {
    if (!hasAudio()) return;
    const float cents = params_.tuneCents.load();
    if (cents == 0.f) return;
    auto d = std::make_shared<AudioData>(*cur_);
    const double newSr = cur_->sampleRate * std::pow(2.0, (double)cents / 1200.0);
    d->sampleRate = newSr;
    commit(std::move(d), "Print tune into sample rate");
    params_.tuneCents.store(0.f);
    clearSelection();
    toast("Tune (" + std::to_string((int)std::lround(cents)) +
          " cents) printed into sample rate (" + std::to_string((int)std::llround(newSr)) + " Hz)");
}

void Document::toggleBits() {
    if (!hasAudio()) return;
    auto d = std::make_shared<AudioData>(*cur_);
    if (d->format == SampleFormat::Int16) {
        d->format = SampleFormat::Float32;
        pushUndo("To 32-bit");
        d->buildPeaks();
        cur_ = std::move(d);
        publish_(cur_);
        toast("Format: 32-bit float");
    } else {
        d->format = SampleFormat::Int16;
        pushUndo("To 16-bit");
        float peak = d->peakAbs();
        if (peak > kInt16Max) {
            // Forcing 16-bit: scale the WHOLE sample down so its peak
            // lands exactly on the 16-bit ceiling, instead of clipping
            // every sample above it (clipping would distort the
            // waveform; scaling only makes it quieter).
            float scale = kInt16Max / peak;
            for (auto& v : d->ch)
                for (auto& s : v) s *= scale;
            toast("Format: 16-bit (amplitude scaled down to fit)");
        } else {
            toast("Format: 16-bit");
        }
        for (auto& v : d->ch)
            for (auto& s : v) s = quantize16(s);
        d->buildPeaks();
        cur_ = std::move(d);
        publish_(cur_);
    }
}

void Document::applyEnvelopes() {
    if (!hasAudio() || !env.anyActive()) return;
    const int64_t n = cur_->frames();
    const bool stereo = cur_->channels() >= 2;

    if (!env.pitch.isActive()) {
        // Usual path: amp/pan/sep are point-by-point, they don't change
        // duration -- processed in place.
        auto d = std::make_shared<AudioData>(*cur_);
        for (int64_t i = 0; i < n; ++i) {
            float a = env.amp.valueAt((double)i);
            float p = env.pan.valueAt((double)i);
            float s = env.sep.valueAt((double)i);
            if (stereo) {
                float L = d->ch[0][i] * a;
                float R = d->ch[1][i] * a;
                // Stereo separation via mid/side: +1 cancels the side (mono),
                // -1 doubles it (wider).
                if (s != 0.f) {
                    float mid = 0.5f * (L + R);
                    float side = 0.5f * (L - R);
                    side *= (1.f - s); // s=+1 -> side 0 (mono); s=-1 -> side x2

                    L = mid + side;
                    R = mid - side;
                }
                // Balance with sine-law panning: the center stays at unity gain.
                if (p > 0.f) L *= std::cos(p * (float)M_PI * 0.5f);
                else if (p < 0.f) R *= std::cos(-p * (float)M_PI * 0.5f);
                d->ch[0][i] = L;
                d->ch[1][i] = R;
            } else {
                d->ch[0][i] *= a;
            }
        }
        commit(std::move(d), "Apply envelopes");
    } else {
        // With pitch active, resampling (varispeed) is required: local
        // duration is no longer 1:1 with the source, so instead of
        // processing in place the source is walked with a floating-point
        // readPos that advances 2^(cents/1200) per OUTPUT sample -- exactly
        // the same computation Engine::process does live (see that file),
        // so Apply reproduces sample-for-sample what was already heard in
        // preview. readPosOfOutput stores, for each output sample, the
        // readPos it was read with -- this lets loop/selection (given in
        // source samples) be relocated to their corresponding position in
        // the output via binary search, the same idea as the marker remap in
        // changeSampleRate(resample=true) but for a NON-uniform mapping.
        SincInterp16 sinc;
        auto d = std::make_shared<AudioData>();
        d->sampleRate = cur_->sampleRate;
        d->format = cur_->format;
        const int nch = cur_->channels();
        d->ch.resize(nch);
        for (auto& v : d->ch) v.reserve((size_t)n);
        std::vector<double> readPosOfOutput;
        readPosOfOutput.reserve((size_t)n);

        double pos = 0.0;
        while (pos < (double)n) {
            readPosOfOutput.push_back(pos);
            float a = env.amp.valueAt(pos);
            float p = env.pan.valueAt(pos);
            float s = env.sep.valueAt(pos);
            if (stereo) {
                float L = sincReadSample(cur_->ch[0], pos, sinc) * a;
                float R = sincReadSample(cur_->ch[1], pos, sinc) * a;
                if (s != 0.f) {
                    float mid = 0.5f * (L + R);
                    float side = 0.5f * (L - R);
                    side *= (1.f - s);
                    L = mid + side;
                    R = mid - side;
                }
                if (p > 0.f) L *= std::cos(p * (float)M_PI * 0.5f);
                else if (p < 0.f) R *= std::cos(-p * (float)M_PI * 0.5f);
                d->ch[0].push_back(L);
                d->ch[1].push_back(R);
            } else {
                d->ch[0].push_back(sincReadSample(cur_->ch[0], pos, sinc) * a);
            }
            // pow(2, cents/1200) is always > 0, so pos advances strictly --
            // no risk of an infinite loop even if the envelope requests an
            // extreme pitch.
            pos += std::pow(2.0, (double)env.pitch.valueAt(pos) / 1200.0);
        }

        // Relocates loop/selection (in SOURCE samples) to their position in
        // the output: the first output index whose readPos already reached
        // that source sample (same criterion for both endpoints). Computed
        // into LOCAL variables, from the old markers, BEFORE calling
        // commit(): commit() does pushUndo() as its first step, which saves
        // cur_ (the old one) together with whatever params_.loopStart/
        // loopEnd hold AT THAT MOMENT -- if they were already remapped by
        // then, undo would end up inconsistent (old audio + new markers).
        // They're only written AFTER commit().
        auto remap = [&](int64_t srcSample) -> int64_t {
            auto it = std::lower_bound(readPosOfOutput.begin(), readPosOfOutput.end(),
                                       (double)srcSample);
            return (int64_t)(it - readPosOfOutput.begin());
        };
        int64_t newLs = -1, newLe = -1, newSs = -1, newSe = -1;
        {
            int64_t s = params_.loopStart.load(), e = params_.loopEnd.load();
            if (s >= 0 && e > s) { newLs = remap(s); newLe = remap(e); }
            s = params_.selStart.load(); e = params_.selEnd.load();
            if (s >= 0 && e > s) { newSs = remap(s); newSe = remap(e); }
        }

        commit(std::move(d), "Apply envelopes");

        params_.loopStart.store(newLs);
        params_.loopEnd.store(newLe);
        params_.selStart.store(newSs);
        params_.selEnd.store(newSe);
        clampMarkersToLength(); // safety net, same as changeSampleRate
    }

    env.clearAll(); // envelopes reset after being applied
    toast("Envelopes applied to sample");
}

void Document::clearAll() {
    if (!hasAudio()) return;
    pushUndo("Clear editor");
    cur_.reset(); // the clipboard survives the clear
    env.clearAll();
    sampleName_.clear();
    params_.loopStart.store(-1);
    params_.loopEnd.store(-1);
    params_.selStart.store(-1);
    params_.selEnd.store(-1);
    params_.tuneCents.store(0.f); // new project: factory tune (0 cents)
    params_.rootNote.store(60);   // and factory root note (C4 / middle C)
    publish_(nullptr);
    toast("Editor cleared (Ctrl+Z to recover)");
}

void Document::trimAfterLoopEnd() {
    if (!hasAudio() || !hasLoop()) return;
    int64_t le = loopEnd();
    if (le >= frames()) {
        toast("No audio after loop end");
        return;
    }
    auto d = std::make_shared<AudioData>(*cur_);
    for (auto& v : d->ch) v.resize((size_t)le);
    commit(std::move(d), "Trim after loop end");
    clearSelection();
    toast("Audio after loop end removed");
}

void Document::trimBeforeLoopStart() {
    if (!hasAudio() || !hasLoop()) return;
    int64_t ls = loopStart(), le = loopEnd();
    if (ls <= 0) {
        toast("No audio before loop start");
        return;
    }
    auto d = std::make_shared<AudioData>(*cur_);
    for (auto& v : d->ch) v.erase(v.begin(), v.begin() + ls);
    commit(std::move(d), "Trim before loop start");
    // Audio shifted backward: the loop now starts at 0.
    params_.loopStart.store(0);
    params_.loopEnd.store(le - ls);
    clampMarkersToLength();
    clearSelection();
    toast("Audio before loop start removed (attack trimmed)");
}

void Document::extendLoop() {
    if (!hasAudio() || !hasLoop()) return;
    const int64_t ls = loopStart(), le = loopEnd();
    const int64_t n = le - ls; // loop length, > 0 guaranteed by hasLoop()

    auto d = std::make_shared<AudioData>(*cur_);
    for (auto& v : d->ch) {
        std::vector<float> piece(v.begin() + ls, v.begin() + le);
        v.insert(v.begin() + le, piece.begin(), piece.end());
    }
    // Shifts markers at/after the insertion point (le) -- same rule as
    // paste()'s marker shift. loopStart (< le) is untouched; loopEnd
    // (== le) becomes le+n, now spanning the duplicated copy too.
    auto shift = [le, n](int64_t p) { return (p >= 0 && p >= le) ? p + n : p; };
    const int64_t newLs = shift(params_.loopStart.load());
    const int64_t newLe = shift(params_.loopEnd.load());
    const int64_t newSelS = shift(params_.selStart.load());
    const int64_t newSelE = shift(params_.selEnd.load());
    commit(std::move(d), "Extend loop");
    // Written AFTER commit(): must capture the OLD marker values for undo,
    // same ordering rule as spliceAndRemap.
    params_.loopStart.store(newLs);
    params_.loopEnd.store(newLe);
    params_.selStart.store(newSelS);
    params_.selEnd.store(newSelE);
    clampMarkersToLength();
    toast("Loop extended (duplicated)");
}

void Document::trimSilence(float thresholdDb) {
    if (!hasAudio()) return;
    const float threshold = std::pow(10.f, thresholdDb / 20.f);
    const int64_t n = frames();
    const int nch = cur_->channels();

    auto isLoud = [&](int64_t i) {
        for (int c = 0; c < nch; ++c)
            if (std::fabs(cur_->ch[(size_t)c][(size_t)i]) > threshold) return true;
        return false;
    };

    int64_t head = 0;
    while (head < n && !isLoud(head)) ++head;
    if (head >= n) {
        toast("Sample is silent: nothing to trim");
        return;
    }
    int64_t tail = n; // exclusive end of the kept region
    while (tail > head && !isLoud(tail - 1)) --tail;
    if (head == 0 && tail == n) {
        toast("No silence to trim at the start/end");
        return;
    }

    auto d = std::make_shared<AudioData>();
    d->sampleRate = cur_->sampleRate;
    d->format = cur_->format;
    d->ch.resize((size_t)nch);
    for (int c = 0; c < nch; ++c)
        d->ch[(size_t)c].assign(cur_->ch[(size_t)c].begin() + head, cur_->ch[(size_t)c].begin() + tail);

    // Content before 'head' is gone: every marker shifts back by 'head'
    // (clampMarkersToLength() below invalidates any pair that lands
    // negative, same rule as every other length-changing edit).
    auto shift = [head](int64_t p) -> int64_t { return (p < 0) ? p : p - head; };
    const int64_t newLs = shift(params_.loopStart.load());
    const int64_t newLe = shift(params_.loopEnd.load());
    const int64_t newSelS = shift(params_.selStart.load());
    const int64_t newSelE = shift(params_.selEnd.load());
    commit(std::move(d), "Trim silence");
    params_.loopStart.store(newLs);
    params_.loopEnd.store(newLe);
    params_.selStart.store(newSelS);
    params_.selEnd.store(newSelE);
    clampMarkersToLength();
    toast("Silence trimmed from start/end");
}

void Document::replaceRange(int64_t start, int64_t end,
                            std::vector<std::vector<float>>&& piece,
                            int64_t newLoopStart, int64_t newLoopEnd,
                            const char* opName) {
    if (!hasAudio()) return;
    int64_t n = frames();
    start = std::max<int64_t>(0, std::min(start, n));
    end = std::max(start, std::min(end, n));
    auto d = std::make_shared<AudioData>(*cur_);
    for (int c = 0; c < d->channels(); ++c) {
        auto& v = d->ch[c];
        const auto& p = piece[std::min((size_t)c, piece.size() - 1)];
        v.erase(v.begin() + start, v.begin() + end);
        v.insert(v.begin() + start, p.begin(), p.end());
    }
    commit(std::move(d), opName);
    setLoopPoints(newLoopStart, newLoopEnd, false);
    clearSelection();
}

void Document::insertPiece(int64_t start, int64_t end,
                           std::vector<std::vector<float>>&& piece,
                           const char* opName) {
    if (!hasAudio()) return;
    int64_t n = frames();
    start = std::max<int64_t>(0, std::min(start, n));
    end = std::max(start, std::min(end, n));
    auto d = std::make_shared<AudioData>(*cur_);
    for (int c = 0; c < d->channels(); ++c) {
        auto& v = d->ch[c];
        const auto& p = piece[std::min((size_t)c, piece.size() - 1)];
        v.erase(v.begin() + start, v.begin() + end);
        v.insert(v.begin() + start, p.begin(), p.end());
    }
    commit(std::move(d), opName); // keepLoop=true (default): only clamped
    clearSelection();
}
