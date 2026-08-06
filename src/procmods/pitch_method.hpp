// Base for the pluggable Pitch methods (Process > Pitch submenu, one menu
// item per method -- same one-window-per-entry pattern as Process >
// Stretch's StretchMethod list). Each method lives in its own file
// (pitch_builtin.*, pitch_signalsmith.*, pitch_loris.*); the actual DSP
// call is delegated to a *_dsp.hpp core with no ImGui dependency (testable
// from core_test), same split as stretch_method.hpp <-> its own methods.
//
// Unlike StretchMethod, the transpose itself (semitones + cents, and the
// "Take current Tune"/"Apply and reset tune value" convenience around the
// document's live Tune) is common to EVERY pitch method by definition --
// that's what "Pitch" means -- so it lives here in the base class (drawn
// directly by draw()) instead of being duplicated per-method; a subclass's
// drawParams() only draws whatever is specific to ITS OWN algorithm (e.g.
// built-in's "preserve duration" checkbox + FFT size, or Signalsmith's
// tonality limit + formant checkbox).
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"
#include "../document.hpp"
#include "../engine.hpp"
#include "pitch_dsp.hpp"

class PitchMethod {
public:
    virtual ~PitchMethod() = default;
    // Shown as the Process > Pitch submenu item, the window title, and
    // (via Document::applyPitch) the undo history entry.
    virtual const char* name() const = 0;

    bool isOpen() const { return open_; }
    void open(AudioDataPtr src, int64_t s0, int64_t s1);
    void close(Engine& eng);
    void draw(Document& doc, Engine& eng);
    // Full document with the pitch-shifted piece spliced in (for
    // VISUALIZATION on the waveform only; nothing applied). Null when
    // preview is off or the tool is closed.
    AudioDataPtr previewDoc() const { return previewDoc_; }

protected:
    // Draws this method's OWN extra parameter widgets (semitones/cents are
    // already drawn by draw() itself); returns true if something changed
    // (triggers a debounced preview rebuild, same convention as
    // StretchMethod::drawParams). 'doc' is passed through for methods that
    // need live document state (e.g. Loris' Auto Reference Frequency,
    // derived from Root Note + Tune); most methods just ignore it.
    virtual bool drawParams(const Document& doc) = 0;
    // Output-length multiplier for the CURRENT spec_ (>1 = longer) -- used
    // for the "region becomes N samples" readout before rendering.
    // Duration-preserving methods (e.g. Signalsmith) always return 1.0.
    virtual double lengthFactor() const = 0;
    // Pitch-shifts src[s0,s1) per channel using the current params. Empty
    // = failed gracefully (bad range, engine error, region too short for
    // this method's minimum window, etc.).
    virtual std::vector<std::vector<float>> render(const AudioData& src, int64_t s0, int64_t s1) = 0;

    AudioDataPtr src_; // snapshot of the full document when opened
    int64_t s0_ = 0, s1_ = 0;
    pitchdsp::PitchSpec spec_; // semitones/cents, shared by every method

private:
    void rebuildPreview(Engine& eng);
    void clearPreview(Engine& eng);

    bool open_ = false;
    bool previewOn_ = true;
    bool prevDirty_ = true;
    double lastEditT_ = 0.0; // ImGui::GetTime() of the last param change
    AudioDataPtr previewDoc_;
};
