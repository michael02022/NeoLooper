// Base for the pluggable Stretch methods (Process > Stretch submenu, one
// menu item per method -- same one-window-per-entry pattern as Process >
// Loop's LoopModule list, just for a non-loop procmod). Each method lives
// in its own file (stretch_builtin.*, stretch_rubberband.*,
// stretch_signalsmith.*); the actual DSP call is delegated to a *_dsp.hpp
// core with no ImGui dependency (testable from core_test), same split as
// loopmods/rotation_loop.* <-> rotation_loop_dsp.*.
//
// Unlike LoopModule (which overwrites a LOOP REGION and previews via
// Engine::publishPreview), a stretch method changes the DURATION of
// [s0_,s1_) -- its preview splices a resized piece into a full copy of the
// document (Engine::publishDocPreview), the same mechanism Filter/Pitch/
// Loris already use, so markers/selection/envelopes/playhead keep working
// normally during preview.
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"
#include "../document.hpp"
#include "../engine.hpp"

class StretchMethod {
public:
    virtual ~StretchMethod() = default;
    // Shown as the Process > Stretch submenu item, the window title, and
    // (via Document::applyStretch) the undo history entry.
    virtual const char* name() const = 0;

    bool isOpen() const { return open_; }
    void open(AudioDataPtr src, int64_t s0, int64_t s1);
    void close(Engine& eng);
    void draw(Document& doc, Engine& eng);
    // Full document with the stretched piece spliced in (for
    // VISUALIZATION on the waveform only; nothing applied). Null when
    // preview is off or the tool is closed.
    AudioDataPtr previewDoc() const { return previewDoc_; }

protected:
    // Draws this method's own parameter widgets; returns true if
    // something changed (triggers a debounced preview rebuild, same
    // convention as LoopModule::drawParams).
    virtual bool drawParams() = 0;
    // Output-length multiplier for the CURRENT params (>1 = longer/slower)
    // -- used for the "Region becomes N samples" readout before rendering.
    virtual double lengthFactor() const = 0;
    // Time-stretches src[s0,s1) per channel using the current params.
    // Empty = failed gracefully (bad range, engine error, region too
    // short for this method's minimum window, etc.).
    virtual std::vector<std::vector<float>> render(const AudioData& src, int64_t s0, int64_t s1) = 0;

    AudioDataPtr src_; // snapshot of the full document when opened
    int64_t s0_ = 0, s1_ = 0;

private:
    void rebuildPreview(Engine& eng);
    void clearPreview(Engine& eng);

    bool open_ = false;
    bool previewOn_ = true;
    bool prevDirty_ = true;
    double lastEditT_ = 0.0; // ImGui::GetTime() of the last param change
    AudioDataPtr previewDoc_;
};
