#pragma once

#include <cstdint>

#include "../audio_data.hpp"
#include "../document.hpp"
#include "exciter_dsp.hpp"

// Process > Exciter: M/S Exciter (see exciter_dsp.hpp), level-matched so
// applying it doesn't change the volume. Same non-LoopModule scheme as Filter/Amplitude: works on the selection or
// the whole sample, previews as an alternate document
// (Engine::publishDocPreview, debounced) and only touches the document on
// Apply.
class Engine;

class ExciterTool {
public:
    bool isOpen() const { return open_; }
    void open(AudioDataPtr src, int64_t s0, int64_t s1);
    void close(Engine& eng);
    void draw(Document& doc, Engine& eng);
    // Full document with the processed region spliced in (for VISUALIZATION
    // on the waveform only). Null when preview is off or the tool is closed.
    AudioDataPtr previewDoc() const { return previewDoc_; }

private:
    void rebuildPreview(Engine& eng);
    void clearPreview(Engine& eng);
    bool freqSlider(const char* label, float* hz, float def, const char* tip);
    bool drawControls(bool stereo);

    bool open_ = false;
    AudioDataPtr src_;
    int64_t s0_ = 0, s1_ = 0;

    exciterdsp::MSSpec ms_;

    bool previewOn_ = true;
    bool prevDirty_ = true;
    double lastEditT_ = 0.0;
    AudioDataPtr previewDoc_;
};
