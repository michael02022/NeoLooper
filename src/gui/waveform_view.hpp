// Waveform view: the editor's main focus. Mouse-wheel zoom centered on the
// cursor (Polyphone-style), sample-accurate selection via click+drag,
// draggable loop S/E markers (end never drops below start+1) and an
// envelope overlay with editable points (right-click creates/deletes).
#pragma once

#include "../document.hpp"
#include "../engine.hpp"
#include "../envelope.hpp"

class WaveformView {
public:
    // envMode: 0 = none, 1 = volume, 2 = pan, 3 = separation
    // overrideDoc: if not null, that document is displayed instead of the
    // real one (a loop module's preview: read-only, with its own
    // ovLoopStart/ovLoopEnd markers and the playhead remapped to the loop
    // region).
    void draw(Document& doc, Engine& eng, Envelopes& env, int envMode,
              float height, AudioDataPtr overrideDoc = nullptr,
              int64_t ovLoopStart = -1, int64_t ovLoopEnd = -1);

    // Envelope magnet (see envSnap in the .cpp): when creating or dragging a
    // point near the neutral value (0/1 depending on the envelope) OR the
    // value of ANOTHER point already on the same envelope, it snaps exactly
    // to that height.
    bool pointMagnetEnabled() const { return pointMagnet_; }
    void setPointMagnetEnabled(bool v) { pointMagnet_ = v; }

private:
    double viewStart_ = 0.0; // frame at the left edge
    double spp_ = 1.0;       // samples per pixel
    const AudioData* lastDoc_ = nullptr;

    enum Drag { None, Select, LoopStart, LoopEnd, EnvPoint, Scroll };
    int drag_ = None;
    int64_t dragAnchor_ = 0;
    int dragEnvIdx_ = -1;
    float dragStartX_ = 0.f;  // pixel where the drag started
    bool dragMoved_ = false;  // distinguishes a click from a drag
    float sbGrab_ = 0.f;      // grab offset within the scrollbar thumb
    bool pointMagnet_ = true;

    void clampView(int64_t frames, float width);
};
