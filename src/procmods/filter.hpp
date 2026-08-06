#pragma once

#include <cstdint>

#include "../audio_data.hpp"
#include "../document.hpp"
#include "filter_dsp.hpp"

// Filter: resonant high-pass (adjustable slope 12..96 dB/oct, Q on the first
// stage) + a bell band (peaking EQ with +/- gain, mainly meant to add
// brightness to the highs) -- replaces the previous graphic Equalizer
// (Envelope + FFT filtering), which the user found unreliable to read/use.
// Filtering happens in the time domain (classic biquads, see
// filter_dsp.hpp), with no FFT involved in the actual processing -- only the
// VISUALIZATION (a static spectrum analysis, not live) uses FFT, with the
// same calibration and style as the oscilloscope's live spectrum
// (EditorUI::updateLiveSpectrum), an explicit user request because it felt
// clearer than the previous attempt's per-frame point cloud.
//
// No draggable points ("no envelopes at all"): the graph is purely
// informative (real spectrum + the filter's response curve overlaid), all
// control happens through the sliders/combos below it.
//
// Like Reverse/the previous Equalizer: operates on the selection, or the
// whole sample if there's no selection (same criterion as
// Document::normalize/reverse). Doesn't require a marked loop, and although
// it has live preview it does NOT inherit from LoopModule: that contract
// forces looped playback of the published buffer, whereas here the preview
// is an alternate DOCUMENT (Engine::publishDocPreview) played back with
// normal semantics (markers, selection, envelopes and playhead intact) and
// shown on the waveform as an overlay, without applying anything until
// Apply is pressed.
//
// The preview is recomputed with a small debounce (not on every pixel of a
// slider drag): filtering the whole region + rebuilding peaks can cost
// hundreds of ms on long samples.
class Engine;

class FilterTool {
public:
    bool isOpen() const { return open_; }
    // s0/s1: range to analyze and, on Apply, to process.
    void open(AudioDataPtr src, int64_t s0, int64_t s1);
    void close(Engine& eng);
    void draw(Document& doc, Engine& eng);
    // Full document with the filtered region applied (for VISUALIZATION on
    // the waveform only; nothing applied). Null when preview is off or the
    // tool is closed.
    AudioDataPtr previewDoc() const { return previewDoc_; }

private:
    void rebuildSpectrum();
    void rebuildPreview(Engine& eng);
    void clearPreview(Engine& eng);

    bool open_ = false;
    AudioDataPtr src_;
    int64_t s0_ = 0, s1_ = 0;

    filterdsp::FilterSpec spec_;

    static constexpr int kFftSizes[5] = {512, 1024, 2048, 4096, 8192};
    int fftSizeIdx_ = 2; // 2048
    // Background "scale": displayed dynamic range (dB) -- purely visual,
    // doesn't trigger a new analysis (dbFloor_ only crops/rescales the
    // curve_ cache).
    float dbFloor_ = -60.f;

    filterdsp::SpectrumCurve curve_;
    bool bgDirty_ = true;

    bool previewOn_ = true;
    bool prevDirty_ = true;
    double lastEditT_ = 0.0; // ImGui::GetTime() of the last spec_ change
    AudioDataPtr previewDoc_;
};
