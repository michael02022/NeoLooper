#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "../audio_data.hpp"
#include "../document.hpp"
#include "autotune_dsp.hpp"

// Process > Autotune: pitch correction with Snaptune (Studio mode only, see
// autotune_dsp.hpp). Same non-LoopModule scheme as Filter/Amplitude: works on
// the selection or the whole sample, previews as an alternate document
// (Engine::publishDocPreview) and only touches the document on Apply.
//
// The window also draws the pitch curve Snaptune reports for the region:
// how far the detected pitch is from its target note (in cents) and where
// the correction leaves it, with the playhead tracking it while the preview
// plays.
//
// Rendering runs on a worker thread (the correction is ~40x faster than
// real time, so a long sample would otherwise freeze the GUI for seconds):
// a newer setting cancels the render in flight and starts another; the last
// finished render stays on screen meanwhile, and Apply reuses it when it
// matches the current settings.
class Engine;

class AutotuneTool {
public:
    ~AutotuneTool() { stopWorker(); }
    bool isOpen() const { return open_; }
    void open(AudioDataPtr src, int64_t s0, int64_t s1);
    void close(Engine& eng);
    void draw(Document& doc, Engine& eng);
    // Full document with the corrected region spliced in (for VISUALIZATION
    // on the waveform only). Null when preview is off or the tool is closed.
    AudioDataPtr previewDoc() const { return previewDoc_; }

private:
    void startRender();
    void stopWorker();   // cancels and joins a render in flight
    void pollRender(Engine& eng); // takes a finished render, publishes the preview
    void publishPreview(Engine& eng);
    void clearPreview(Engine& eng);
    void drawPitchGraph(Engine& eng, float height);

    bool open_ = false;
    AudioDataPtr src_;
    int64_t s0_ = 0, s1_ = 0;

    autotunedsp::Spec spec_;
    autotunedsp::Result result_; // last finished render: audio + pitch curve
    autotunedsp::Spec resultSpec_; // the settings result_ was rendered with
    bool resultOk_ = false;
    double renderMs_ = 0.0;

    // Worker: renders into pending_, raises done_ when finished.
    std::thread worker_;
    std::atomic<bool> cancel_{false}, done_{false};
    std::atomic<float> progress_{0.f};
    autotunedsp::Result pending_;
    autotunedsp::Spec pendingSpec_;
    bool pendingOk_ = false;
    double pendingMs_ = 0.0;
    bool rendering_ = false;

    bool previewOn_ = true;
    bool dirty_ = true;
    double lastEditT_ = 0.0; // ImGui::GetTime() of the last spec_ change
    AudioDataPtr previewDoc_;
};
