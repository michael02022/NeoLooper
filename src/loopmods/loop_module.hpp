// Base for the loop modules. Each loop type lives in its own source file
// (classic_crossfade.cpp, pingpong_crossfade.cpp, ...). A module receives
// the FULL audio as raw PCM plus the [loopStart, loopEnd) range and can
// modify it however it wants; its changes sound in real time via the
// Engine's preview and are only printed into the document when "Apply" is
// pressed.
#pragma once

#include <cmath>
#include <cstdint>
#include <memory>

#include "../audio_data.hpp"
#include "../document.hpp"
#include "../engine.hpp"

// Crossfade gains with a morph between equal-gain (linear) and equal-power
// (sinusoidal). t in [0,1], shape in [0,1] (0 = equal gain, 1 = equal power).
inline void xfadeGains(float t, float shape, float& gIn, float& gOut) {
    float powIn = std::sin(t * (float)M_PI * 0.5f);
    float powOut = std::cos(t * (float)M_PI * 0.5f);
    gIn = t + (powIn - t) * shape;
    gOut = (1.f - t) + (powOut - (1.f - t)) * shape;
}

class LoopModule {
public:
    virtual ~LoopModule() = default;
    virtual const char* name() const = 0;
    // false if the module can be opened without a marked loop/selection
    // (e.g. Freeze, which computes its own anchor point with a slider
    // instead of depending on [ls, le)). Queried by EditorUI before open(),
    // without needing an open instance.
    virtual bool requiresLoopRegion() const { return true; }

    bool isOpen() const { return open_; }

    void open(AudioDataPtr doc, int64_t ls, int64_t le) {
        src_ = std::move(doc);
        ls_ = ls;
        le_ = le;
        open_ = true;
        dirty_ = true;
        preview_.reset();
    }

    void close(Engine& eng) {
        if (!open_) return;
        open_ = false;
        eng.publishPreview(nullptr);
        preview_.reset();
        previewDoc_.reset();
    }

    // Draws the module's window; manages live preview and applying.
    void draw(Document& doc, Engine& eng);

    // Full document with the module's temporary edit placed in the loop
    // region (for VISUALIZATION on the waveform only; nothing is applied
    // yet).
    AudioDataPtr previewDoc() const { return previewDoc_; }
    int64_t previewLoopStart() const { return previewRangeStart(); }
    int64_t previewLoopEnd() const {
        return previewRangeStart() + (preview_ ? preview_->frames() : le_ - ls_);
    }

protected:
    virtual bool drawParams() = 0;                  // true if something changed
    virtual std::shared_ptr<AudioData> render() = 0; // rendered loop
    virtual void apply(Document& doc) = 0;
    // Point where preview_'s content starts within the document. Defaults
    // to ls_ (preview_ exactly replaces [ls_, le_), same as Classic/PingPong
    // Crossfade). A module can shift it backward if its render() also
    // modifies audio before the loop start (e.g. Harmonic, which blends the
    // real attack with the start of the synthesized loop and so includes
    // that span in preview_ so it can be heard live).
    virtual int64_t previewRangeStart() const { return ls_; }
    // Point where the original document resumes sounding unchanged after
    // preview_ (see rebuildPreviewDoc). Defaults to le_. A module can
    // decouple it from le_ if its "region" doesn't come from a marked loop
    // but from an internally computed point (e.g. Freeze).
    virtual int64_t previewRangeEnd() const { return le_; }
    // Offset within preview_ (not the document) where the region that
    // repeats forever starts; anything before that is "attack" and plays
    // once (see Engine::publishPreview). Defaults to 0: the whole buffer
    // repeats, same as Classic/PingPong. Harmonic and Freeze shift it to
    // xfadeN (the entry crossfade) so only the pure period repeats.
    virtual int64_t previewLoopOffset() const { return 0; }
    // false to skip drawing the generic "Loop region: ..." header (which
    // doesn't make sense when the module doesn't operate on [ls_,le_), e.g.
    // Freeze).
    virtual bool showLoopRegionHeader() const { return true; }

    AudioDataPtr src_; // snapshot of the full document when opened
    // Document's root note, refreshed on every draw() -- for modules that
    // depend on the sample's nominal pitch (Freeze in exact-cycle mode, same
    // as Infinity's Freeze Looper "MIDI Note Number"). Doesn't travel
    // through open() because it can change while the window is open.
    int docRootNote_ = 60;
    // Document's tuning (cents), refreshed alongside docRootNote_. The
    // recorded CONTENT's actual tuning, relative to the root note, is the
    // INVERSE of this (see the comment in wav_io.cpp: tuneCents is the
    // CORRECTION the user applied, not the sample's mistuning) -- that's why
    // Freeze uses -docTuneCents_ (not docTuneCents_ as-is) to calibrate its
    // own tuning when it opens.
    float docTuneCents_ = 0.f;
    int64_t ls_ = 0, le_ = 0;
    bool open_ = false;
    bool dirty_ = false;
    std::shared_ptr<AudioData> preview_;
    std::shared_ptr<AudioData> previewDoc_;
    void rebuildPreviewDoc();
};
