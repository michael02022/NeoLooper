#pragma once

#include <cstdint>

#include "../audio_data.hpp"
#include "../document.hpp"
#include "amplitude_dsp.hpp"

// Amplitude: plain gain (dB), the simplest processor in the Process menu --
// one slider plus a Clip checkbox. Operates on the selection, or the whole
// sample if there's no selection (same criterion as Filter/Pitch/reverse/
// normalize). Doesn't require a marked loop; same non-LoopModule preview
// scheme as Filter/Pitch/Loris (Engine::publishDocPreview, an alternate
// document played back with normal semantics, not applying anything until
// Apply is pressed). Same length in/out (in-place), no marker remap needed.
//
// Clip (ON by default): hard-clamps the result to [-1,1] after the gain --
// the classic digital "clip" you get from pushing a signal too hot. OFF
// leaves samples beyond +-1.0 untouched (see amplitude_dsp.hpp for why
// that's still safe to keep around mid-edit).
class Engine;

class AmplitudeTool {
public:
    bool isOpen() const { return open_; }
    void open(AudioDataPtr src, int64_t s0, int64_t s1);
    void close(Engine& eng);
    void draw(Document& doc, Engine& eng);
    // Full document with the gained region spliced in (for VISUALIZATION on
    // the waveform only; nothing applied). Null when preview is off or the
    // tool is closed.
    AudioDataPtr previewDoc() const { return previewDoc_; }

private:
    void rebuildPreview(Engine& eng);
    void clearPreview(Engine& eng);

    bool open_ = false;
    AudioDataPtr src_;
    int64_t s0_ = 0, s1_ = 0;

    amplitudedsp::AmplitudeSpec spec_;

    bool previewOn_ = true;
    bool prevDirty_ = true;
    double lastEditT_ = 0.0; // ImGui::GetTime() of the last spec_ change
    AudioDataPtr previewDoc_;
};
