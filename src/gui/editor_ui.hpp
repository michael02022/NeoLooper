// Editor's main UI (drawn every frame on the GUI thread).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../document.hpp"
#include "../engine.hpp"
#include "../envelope.hpp"
#include "../loopmods/auto_loop_algorithm.hpp"
#include "../loopmods/loop_module.hpp"
#include "../procmods/amplitude.hpp"
#include "../procmods/filter.hpp"
#include "../procmods/pitch_method.hpp"
#include "../procmods/stretch_method.hpp"
#include "file_dialog.hpp"
#include "waveform_view.hpp"

class EditorUI {
public:
    EditorUI(Document& doc, Engine& eng);

    void draw();                             // full frame
    void openPath(const std::string& path);  // drag & drop / File->Open

private:
    void drawMenuBar();
    void drawToolbar();
    void drawInfoBar();
    void drawTuneAndScope();
    // Refreshes spectrumDb_ with an FFT of the last PLAYED samples
    // (Engine::scopeRing_, the same feed already driving the oscilloscope)
    // -- called once per GUI frame while the spectral view is active.
    // Without playback, it decays toward the floor instead of showing the
    // last frozen frame (same criterion as the oscilloscope, which simply
    // draws nothing when nothing is sounding).
    void updateLiveSpectrum();
    void drawPiano(float width);
    void drawDialogs();
    void drawToasts();
    void handleShortcuts();
    void toggleRecord();
    void exportRequested();
    // Transport toggle: playing starts from the selection if there is one;
    // stopping rewinds to the starting point.
    void playPressed();
    void stopPressed();
    bool anyModuleOpen() const;
    // The preview doc of whichever stretchMethods_ entry is currently
    // open, or null (only one is open at a time -- see the Stretch
    // submenu in drawMenuBar). Shared by draw()'s waveform overlay and
    // drawInfoBar()'s "STRETCH PREVIEW" label.
    AudioDataPtr activeStretchPreview() const;
    // Same as activeStretchPreview(), for pitchMethods_/the Pitch submenu.
    AudioDataPtr activePitchPreview() const;

    Document& doc_;
    Engine& eng_;
    WaveformView wave_;
    FileDialog fileDlg_;
    int fileDlgPurpose_ = 0; // 0 open, 1 export
    // Bit depth of the exported WAV (index into the dialog's combo: 0=16,
    // 1=24, 2=32 float). Remembers the last choice for the session; starts
    // at 16-bit, the most compatible option (32-bit float, though it's the
    // only lossless one from a 32-bit document, isn't read by all sampler
    // software -- e.g. Polyphone silently fails to import it).
    int exportBitsIdx_ = 0;
    int envMode_ = 0;        // 0 none, 1 vol, 2 pan, 3 sep, 4 pitch

    std::vector<std::unique_ptr<LoopModule>> modules_;
    FilterTool filter_;       // Process > Filter... (preview via publishDocPreview, not a LoopModule)
    AmplitudeTool amplitude_; // Process > Amplitude... (same non-LoopModule scheme as filter_)
    // Process > Pitch submenu: one entry per method (built-in, Signalsmith,
    // Loris), same one-window-per-entry pattern as Process > Stretch's
    // stretchMethods_ below. Registered in the constructor.
    std::vector<std::unique_ptr<PitchMethod>> pitchMethods_;
    // Process > Stretch submenu: one entry per method (built-in, Rubber
    // Band, Signalsmith), same one-window-per-entry pattern as Process >
    // Loop's modules_ above. Registered in the constructor.
    std::vector<std::unique_ptr<StretchMethod>> stretchMethods_;

    // sample rate dialog
    bool srPopup_ = false;
    int srTarget_ = 44100;

    // trim silence dialog
    bool trimSilencePopup_ = false;
    float trimSilenceDb_ = -90.f;

    // Auto Loop dialog: a menu of algorithms by author (see loopmods/
    // auto_loop_algorithm.hpp), each owning its own parameter widgets.
    // Registered in the constructor, same pattern as modules_. Search
    // range is automatic, not a checkbox: the selection if there is one,
    // the whole sample otherwise (same rule Filter/Pitch/Stretch use).
    std::vector<std::unique_ptr<autoloop::AutoLoopAlgorithm>> autoLoopAlgos_;
    int autoLoopAlgoIdx_ = 0;
    bool autoLoopPopup_ = false;
    // Candidates from the last "Find Loop(s)" run, best first; cleared
    // when the popup (re)opens or the algorithm changes since they belong
    // to a specific algorithm/parameter combination.
    std::vector<autoloop::AlgoResult> autoLoopResults_;

    // Oscilloscope <-> spectrum (click the panel to toggle). The spectrum is
    // LIVE, of the audio actually being played (not a static analysis of the
    // whole file) -- meant to give an at-a-glance view of whether there's
    // barely-perceptible frequency content (bass/subsonic) in what's
    // currently being heard.
    bool scopeSpectral_ = false;
    std::vector<float> spectrumDb_; // smoothed (fast attack/slow release) frame by frame
    int spectrumNFft_ = 0;
    double spectrumSr_ = 44100.0;

    struct Toast {
        std::string text;
        float age = 0.f;
    };
    std::vector<Toast> toasts_;
    void pushToast(const std::string& t);
};
