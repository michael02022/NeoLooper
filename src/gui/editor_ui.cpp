#include "editor_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgui.h"

#include "IconsFontAwesome4.h"

#include "../fft.hpp"
#include "../wav_io.hpp"
#include "../loopmods/classic_crossfade.hpp"
#include "../loopmods/classic_crossfade_inside.hpp"
#include "../loopmods/pingpong_crossfade.hpp"
#include "../loopmods/pingpong_crossfade_inside.hpp"
#include "../loopmods/pingpong_classic.hpp"
#include "../loopmods/harmonic_loop.hpp"
#include "../loopmods/freeze_loop.hpp"
#include "../loopmods/freeze_stretch.hpp"
#include "../loopmods/rotation_loop.hpp"
#include "../loopmods/granular_loop.hpp"
#include "../loopmods/pingpong_harmonic.hpp"
#include "../loopmods/auto_loop_milselarch.hpp"
#include "../loopmods/auto_loop_loopauditioneer.hpp"
#include "../procmods/pitch_builtin.hpp"
#include "../procmods/pitch_loris.hpp"
#include "../procmods/pitch_signalsmith.hpp"
#include "../procmods/stretch_builtin.hpp"
#include "../procmods/stretch_rubberband.hpp"
#include "../procmods/stretch_signalsmith.hpp"

namespace {

const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F",
                              "F#", "G", "G#", "A", "A#", "B"};

std::string noteName(int n) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%s%d", kNoteNames[n % 12], n / 12 - 1);
    return buf;
}

// Base name with no directory or extension, to prefill the export dialog.
std::string baseNameNoExt(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    return base;
}

std::string formatTime(double frames, double sr) {
    if (sr <= 0) sr = 1;
    double secs = frames / sr;
    int mm = (int)(secs / 60.0);
    double rem = secs - mm * 60;
    int ss = (int)rem;
    int ms = (int)std::lround((rem - ss) * 1000.0);
    if (ms >= 1000) { ms -= 1000; ss += 1; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d.%03d", mm, ss, ms);
    return buf;
}

bool toolbarButton(const char* label, bool enabled, bool lit = false,
                   float width = 0.f) {
    if (!enabled) ImGui::BeginDisabled();
    if (lit) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.55f, 0.2f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.4f, 0.1f, 1.f));
    }
    bool r = ImGui::Button(label, ImVec2(width, 0.f));
    if (lit) ImGui::PopStyleColor(3);
    if (!enabled) ImGui::EndDisabled();
    return r && enabled;
}

} // namespace

EditorUI::EditorUI(Document& doc, Engine& eng) : doc_(doc), eng_(eng) {
    modules_.emplace_back(new ClassicCrossfadeInside());
    modules_.emplace_back(new ClassicCrossfade());
    modules_.emplace_back(new PingPongClassic());
    modules_.emplace_back(new PingPongCrossfadeInside());
    modules_.emplace_back(new PingPongCrossfade());
    modules_.emplace_back(new HarmonicLoop());
    modules_.emplace_back(new PingPongHarmonicLoop());
    modules_.emplace_back(new FreezeLoop());
    modules_.emplace_back(new FreezeStretch());
    modules_.emplace_back(new RotationLoop());
    modules_.emplace_back(new GranularLoop());

    // Auto Loop's algorithm menu: one entry per author. Add new methods
    // here as they're implemented (see loopmods/auto_loop_algorithm.hpp).
    autoLoopAlgos_.emplace_back(new AutoLoopMilselarch());
    autoLoopAlgos_.emplace_back(new AutoLoopLoopAuditioneer());

    // Process > Pitch submenu: one entry per method.
    pitchMethods_.emplace_back(new PitchBuiltin());
    pitchMethods_.emplace_back(new PitchSignalsmith());
    pitchMethods_.emplace_back(new PitchLoris());

    // Process > Stretch submenu: one entry per method.
    stretchMethods_.emplace_back(new StretchBuiltin());
    stretchMethods_.emplace_back(new StretchRubberBand());
    stretchMethods_.emplace_back(new StretchSignalsmith());
}

void EditorUI::pushToast(const std::string& t) {
    if (t.empty()) return;
    toasts_.push_back({t, 0.f});
    if (toasts_.size() > 4) toasts_.erase(toasts_.begin());
}

void EditorUI::openPath(const std::string& path) {
    LoadResult r = loadAudioFile(path);
    if (!r.data) {
        pushToast("Error opening: " + r.error);
        return;
    }
    eng_.pause();
    for (auto& m : modules_) m->close(eng_);
    // The filter too: if left open it would keep operating (and sounding in
    // preview) on the snapshot of the PREVIOUS sample.
    filter_.close(eng_);
    doc_.setAudio(r.data, "Open file");
    doc_.setSampleName(baseNameNoExt(path));
    doc_.clearLoopPoints(false);
    // Clean start: root note and finetune go back to factory settings before
    // applying (if the file carries them) the loaded file's own values.
    doc_.setRootNote(60);
    doc_.setTuneCents(0.f);
    if (r.hasInstrument) {
        if (r.rootNote >= 0 && r.rootNote < 128) doc_.setRootNote(r.rootNote);
        doc_.setTuneCents(r.tuneCents);
        if (r.loopStart >= 0 && r.loopEnd > r.loopStart)
            doc_.setLoopPoints(r.loopStart, r.loopEnd, false);
    }
    doc_.env.clearAll();
    eng_.seek(0);
    size_t base = path.find_last_of('/');
    pushToast("Loaded: " + (base == std::string::npos
                                ? path : path.substr(base + 1)));
}

void EditorUI::toggleRecord() {
    if (eng_.isRecording()) {
        auto rec = eng_.stopRecord();
        // The recording is APPENDED to the loaded audio (never replaces it);
        // on an empty document it becomes the document.
        if (rec) doc_.appendAudio(rec, "Record (append)");
    } else {
        eng_.pause();
        eng_.startRecord();
        pushToast(doc_.hasAudio()
                      ? "Recording (will be appended to the end of the audio)..."
                      : "Recording the plugin's input...");
    }
}

void EditorUI::exportRequested() {
    if (!doc_.hasAudio()) return;
    fileDlgPurpose_ = 1;
    std::string base = doc_.sampleName().empty() ? "loop" : doc_.sampleName();
    fileDlg_.open(FileDialog::SaveFile, "Export WAV", {".wav"}, base + ".wav");
    fileDlg_.setOptions("Depth",
                        {"16-bit PCM", "24-bit PCM", "32-bit float"},
                        exportBitsIdx_);
}

bool EditorUI::anyModuleOpen() const {
    for (auto& m : modules_)
        if (m->isOpen()) return true;
    return false;
}

AudioDataPtr EditorUI::activeStretchPreview() const {
    for (auto& m : stretchMethods_)
        if (m->isOpen()) return m->previewDoc();
    return nullptr;
}

AudioDataPtr EditorUI::activePitchPreview() const {
    for (auto& m : pitchMethods_)
        if (m->isOpen()) return m->previewDoc();
    return nullptr;
}

void EditorUI::playPressed() {
    if (!doc_.hasAudio()) return;
    if (eng_.isRecording()) toggleRecord();
    if (eng_.previewActive()) {
        // A loop module preview uses its OWN buffer, with its own
        // coordinate space -- the document's selection doesn't apply to
        // it. Always restart from the true beginning (sample 0 of the
        // preview, i.e. its own attack if it has one).
        eng_.seek(0.0);
    } else if (doc_.hasSelection()) {
        // With an active selection, playback starts at its beginning.
        eng_.seek((double)doc_.selStart());
    } else {
        // No selection: start from the true beginning of the sample (the
        // attack), even with a loop marked further in -- NOT from the
        // loop's own start. The marked loop still governs where playback
        // stops/wraps (see Engine::process), just not where it starts.
        eng_.seek(0.0);
    }
    eng_.play();
}

void EditorUI::stopPressed() {
    eng_.pause();
    if (eng_.previewActive()) {
        eng_.seek(0.0);
    } else if (doc_.hasSelection()) {
        eng_.seek((double)doc_.selStart());
    } else {
        eng_.seek(0.0);
    }
}

// ------------------------------------------------------------------ frame

void EditorUI::draw() {
    eng_.pollRecording();
    pushToast(doc_.takeToast());

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoScrollbar);

    drawMenuBar();
    drawToolbar();

    // height reserved for what goes below the waveform
    const float bottomH = ImGui::GetFrameHeightWithSpacing() /*info*/ +
                          ImGui::GetFrameHeightWithSpacing() /*tune*/ +
                          96.f /*piano*/ + 16.f;
    float waveH = std::max(140.f, ImGui::GetContentRegionAvail().y - bottomH);

    // With a loop module open, the waveform displays the document with the
    // temporary edit applied (without touching the real document).
    LoopModule* activeMod = nullptr;
    for (auto& m : modules_)
        if (m->isOpen()) activeMod = m.get();
    AudioDataPtr ov = activeMod ? activeMod->previewDoc() : nullptr;
    int64_t ovLs = ov ? activeMod->previewLoopStart() : -1;
    int64_t ovLe = ov ? activeMod->previewLoopEnd() : -1;
    // Filter/Amplitude/Pitch/Stretch preview: same visual overlay,
    // but with the real document's markers (doc-preview playback still
    // respects them, so they must be shown; display-only, not draggable in
    // preview mode). All share the Engine's single docPreview slot
    // (Engine::publishDocPreview), same as filter_ already did on its own
    // -- opening more than one at once isn't disallowed, but only one
    // overlay (in the order checked below) shows here and whichever draws
    // last below wins the audible preview.
    if (!ov && filter_.previewDoc()) {
        ov = filter_.previewDoc();
        ovLs = doc_.loopStart();
        ovLe = doc_.loopEnd();
    }
    if (!ov && amplitude_.previewDoc()) {
        ov = amplitude_.previewDoc();
        ovLs = doc_.loopStart();
        ovLe = doc_.loopEnd();
    }
    if (!ov && activePitchPreview()) {
        ov = activePitchPreview();
        ovLs = doc_.loopStart();
        ovLe = doc_.loopEnd();
    }
    if (!ov && activeStretchPreview()) {
        ov = activeStretchPreview();
        ovLs = doc_.loopStart();
        ovLe = doc_.loopEnd();
    }
    wave_.draw(doc_, eng_, doc_.env, envMode_, waveH, ov, ovLs, ovLe);

    drawInfoBar();
    drawTuneAndScope();

    ImGui::End();

    for (auto& m : modules_) m->draw(doc_, eng_);
    filter_.draw(doc_, eng_);
    amplitude_.draw(doc_, eng_);
    for (auto& m : pitchMethods_) m->draw(doc_, eng_);
    for (auto& m : stretchMethods_) m->draw(doc_, eng_);
    drawDialogs();
    drawToasts();
    handleShortcuts();

    // Publishes the envelopes being edited to the audio thread: so they're
    // HEARD live while being edited, before they're applied (Edison-style).
    eng_.publishEnvelopes(std::make_shared<Envelopes>(doc_.env));
}

// ------------------------------------------------------------------ menu

void EditorUI::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Ctrl+O")) {
            fileDlgPurpose_ = 0;
            fileDlg_.open(FileDialog::OpenFile, "Open audio",
                          {".wav", ".aiff", ".aif", ".flac", ".ogg"});
        }
        if (ImGui::MenuItem("Export WAV...", nullptr, false, doc_.hasAudio()))
            exportRequested();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, doc_.canUndo()))
            doc_.undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, doc_.canRedo()))
            doc_.redo();
        ImGui::Separator();
        bool sel = doc_.hasSelection();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, sel)) doc_.cutSelection();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, sel)) doc_.copySelection();
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, doc_.hasClipboard()))
            doc_.paste();
        if (ImGui::MenuItem("Delete", "Del", false, sel)) doc_.deleteSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Select all", "Ctrl+A", false, doc_.hasAudio()))
            doc_.setSelection(0, doc_.frames());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Process")) {
        if (ImGui::BeginMenu("Loop")) {
            int64_t ls, le;
            bool canLoop = doc_.hasAudio() && doc_.effectiveLoop(ls, le);
            bool anyWithoutLoop = false;
            for (auto& m : modules_) {
                bool isOpen = m->isOpen();
                bool needsLoop = m->requiresLoopRegion();
                anyWithoutLoop |= !needsLoop;
                bool enabled = needsLoop ? canLoop : doc_.hasAudio();
                if (ImGui::MenuItem(m->name(), nullptr, isOpen, enabled || isOpen)) {
                    if (isOpen) {
                        m->close(eng_);
                    } else {
                        for (auto& o : modules_) o->close(eng_);
                        // Modules without a marked loop (e.g. Freeze) compute
                        // their own anchor point; ls/le only satisfy the
                        // LoopModule contract (le>ls), they aren't used as a
                        // region.
                        m->open(doc_.data(), needsLoop ? ls : 0,
                               needsLoop ? le : doc_.frames());
                    }
                }
            }
            if (!canLoop && !anyWithoutLoop) {
                ImGui::Separator();
                ImGui::TextDisabled("Mark a loop or select a region");
            } else if (!canLoop) {
                ImGui::Separator();
                ImGui::TextDisabled("Mark a loop or select a region for the rest of the methods");
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        // Non-loop processing: operates directly on the document (undoable),
        // with no window of its own.
        if (ImGui::MenuItem("Reverse", nullptr, false, doc_.hasAudio()))
            doc_.reverse();
        ImGui::SetItemTooltip(
            "Reverses the selection; with no selection, reverses the whole "
            "sample. Loop start/end markers don't move.");
        // Filter: unlike Reverse it DOES have its own window (high-pass +
        // bell band, see over the real spectrum), but it's not a LoopModule
        // either -- it doesn't require a marked loop and its preview uses
        // the document's normal playback (publishDocPreview), not the
        // modules' looped preview.
        if (ImGui::MenuItem("Filter...", nullptr, filter_.isOpen(),
                            doc_.hasAudio() || filter_.isOpen())) {
            if (filter_.isOpen()) {
                filter_.close(eng_);
            } else {
                int64_t s0 = 0, s1 = doc_.frames();
                if (doc_.hasSelection()) { s0 = doc_.selStart(); s1 = doc_.selEnd(); }
                filter_.open(doc_.data(), s0, s1);
            }
        }
        ImGui::SetItemTooltip(
            "Resonant high-pass + bell band (boost or cut) over the real "
            "spectrum of the selection (or the whole sample, with no "
            "selection). With Preview enabled, the waveform and playback use "
            "the filtered version without applying anything; it's printed "
            "when you hit Apply in its window.");
        // Pitch: transpose (see procmods/pitch_method.hpp) -- unlike the
        // Tune slider (a live varispeed parameter) this bakes a real pitch
        // change into the audio. Submenu instead of a single item: several
        // methods are available, same one-window-per-entry pattern as the
        // Stretch submenu below (only one method's window open at a time,
        // opening one closes any other).
        if (ImGui::BeginMenu("Pitch")) {
            for (auto& m : pitchMethods_) {
                bool isOpen = m->isOpen();
                if (ImGui::MenuItem(m->name(), nullptr, isOpen, doc_.hasAudio() || isOpen)) {
                    if (isOpen) {
                        m->close(eng_);
                    } else {
                        for (auto& o : pitchMethods_) o->close(eng_);
                        int64_t s0 = 0, s1 = doc_.frames();
                        if (doc_.hasSelection()) { s0 = doc_.selStart(); s1 = doc_.selEnd(); }
                        m->open(doc_.data(), s0, s1);
                    }
                }
            }
            ImGui::EndMenu();
        }
        // Stretch: the mirror image of Pitch -- changes duration while
        // holding pitch fixed. Submenu instead of a single item: several
        // methods are available (see procmods/stretch_method.hpp), same
        // one-window-per-entry pattern as the Loop submenu above, just for
        // a non-loop procmod. Only one method's window open at a time
        // (opening one closes any other, same rule as the Loop submenu).
        if (ImGui::BeginMenu("Stretch")) {
            for (auto& m : stretchMethods_) {
                bool isOpen = m->isOpen();
                if (ImGui::MenuItem(m->name(), nullptr, isOpen, doc_.hasAudio() || isOpen)) {
                    if (isOpen) {
                        m->close(eng_);
                    } else {
                        for (auto& o : stretchMethods_) o->close(eng_);
                        int64_t s0 = 0, s1 = doc_.frames();
                        if (doc_.hasSelection()) { s0 = doc_.selStart(); s1 = doc_.selEnd(); }
                        m->open(doc_.data(), s0, s1);
                    }
                }
            }
            ImGui::EndMenu();
        }
        // Amplitude: the simplest processor here -- plain gain (dB) with a
        // Clip checkbox. Same non-LoopModule scheme as Filter/Pitch/
        // Stretch.
        if (ImGui::MenuItem("Amplitude...", nullptr, amplitude_.isOpen(),
                            doc_.hasAudio() || amplitude_.isOpen())) {
            if (amplitude_.isOpen()) {
                amplitude_.close(eng_);
            } else {
                int64_t s0 = 0, s1 = doc_.frames();
                if (doc_.hasSelection()) { s0 = doc_.selStart(); s1 = doc_.selEnd(); }
                amplitude_.open(doc_.data(), s0, s1);
            }
        }
        ImGui::SetItemTooltip(
            "Plain gain (dB), baked into the audio. Clip (on by default) "
            "hard-clamps the result to [-1, 1]; turn it off to allow the "
            "signal to go 'hot' beyond that. Operates on the selection, or "
            "the whole sample with no selection.");
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

// ------------------------------------------------------------------ toolbar

void EditorUI::drawToolbar() {
    const bool has = doc_.hasAudio();
    const bool sel = doc_.hasSelection();

    // --- transport ---
    // Icon only (Font Awesome, see third_party/fontawesome/): same fixed
    // width across all 4 buttons so the row doesn't jump when toggling
    // Play<->Stop; the tooltip is the textual backup (explicit user request
    // -- scoped to just this transport row).
    const float kTransportW = 32.f;
    bool loopOn = eng_.params.loopEnabled.load();
    if (toolbarButton(ICON_FA_RETWEET, true, loopOn, kTransportW))
        eng_.params.loopEnabled.store(!loopOn);
    ImGui::SetItemTooltip(loopOn ? "Loop: on" : "Loop: off");
    ImGui::SameLine();
    // Play <-> Stop toggle: stop rewinds; play starts from the beginning of
    // the selection if there is one, otherwise from the playhead.
    if (eng_.isPlaying()) {
        if (toolbarButton(ICON_FA_STOP, true, true, kTransportW)) stopPressed();
        ImGui::SetItemTooltip("Stop");
    } else {
        if (toolbarButton(ICON_FA_PLAY, has, false, kTransportW)) playPressed();
        ImGui::SetItemTooltip("Play");
    }
    ImGui::SameLine();
    if (toolbarButton(ICON_FA_PAUSE, true, false, kTransportW)) {
        if (eng_.isRecording()) toggleRecord();
        else eng_.pause();
    }
    ImGui::SetItemTooltip("Pause");
    ImGui::SameLine();
    if (toolbarButton(ICON_FA_CIRCLE, true, eng_.isRecording(), kTransportW))
        toggleRecord();
    ImGui::SetItemTooltip(eng_.isRecording() ? "Recording... (click to stop)" : "Record");
    if (eng_.isRecording()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "REC %s",
                           formatTime((double)eng_.recordedFrames(),
                                      eng_.hostSampleRate()).c_str());
    }

    ImGui::SameLine(0, 18);
    // --- format ---
    AudioDataPtr d = doc_.data();
    if (toolbarButton(d ? formatName(d->format) : "bits", has))
        doc_.toggleBits();
    ImGui::SetItemTooltip("Toggles 16 <-> 32 bit (shows the bits currently in use)");
    ImGui::SameLine();
    {
        char srLabel[32];
        snprintf(srLabel, sizeof(srLabel), "%d Hz",
                 d ? (int)d->sampleRate : 0);
        if (toolbarButton(srLabel, has)) {
            srPopup_ = true;
            srTarget_ = (int)d->sampleRate;
        }
    }
    ImGui::SameLine();
    if (toolbarButton("Mono", has && d && d->channels() > 1))
        ImGui::OpenPopup("##monoPopup");
    if (ImGui::BeginPopup("##monoPopup")) {
        if (ImGui::MenuItem("Take L side")) doc_.toMono(0);
        if (ImGui::MenuItem("Take R side")) doc_.toMono(1);
        if (ImGui::MenuItem("Mix L+R")) doc_.toMono(2);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (toolbarButton("Normalize", has)) doc_.normalize();
    ImGui::SameLine();
    if (toolbarButton("Trim Silence", has)) trimSilencePopup_ = true;
    ImGui::SetItemTooltip(
        "Deletes leading/trailing silence, keeping only the region in "
        "between. Opens a popup to pick how strict \"silence\" is.");

    ImGui::SameLine(0, 18);
    // --- looping --- (Cut/Copy/Paste/Delete toolbar buttons removed --
    // still available from the Edit menu and their shortcuts)
    if (toolbarButton("Sel->Loop", sel))
        doc_.setLoopPoints(doc_.selStart(), doc_.selEnd(), true);
    ImGui::SetItemTooltip("Sets loop start / loop end from the selection");
    ImGui::SameLine();
    if (toolbarButton("Remove loop", doc_.hasLoop())) doc_.clearLoopPoints(true);
    ImGui::SameLine();
    if (toolbarButton("Remove attack", doc_.hasLoop() && doc_.loopStart() > 0))
        doc_.trimBeforeLoopStart();
    ImGui::SetItemTooltip(
        "Deletes all audio before the loop start\n"
        "(the loop now starts at sample 0)");
    ImGui::SameLine();
    if (toolbarButton("Trim tail",
                      doc_.hasLoop() && doc_.loopEnd() < doc_.frames()))
        doc_.trimAfterLoopEnd();
    ImGui::SetItemTooltip(
        "Deletes all audio after the loop end\n"
        "(to export without leftovers)");
    ImGui::SameLine();
    if (toolbarButton("Extend Loop", doc_.hasLoop())) doc_.extendLoop();
    ImGui::SetItemTooltip(
        "Duplicates the loop region and inserts the copy right after it,\n"
        "extending the loop end to cover it: a one-cycle loop becomes a\n"
        "two-cycle loop of the same audio.");
    ImGui::SameLine();
    if (toolbarButton("Auto Loop", has)) autoLoopPopup_ = true;
    ImGui::SetItemTooltip(
        "Searches the sample for a repeating section and sets Loop\n"
        "Start/End to it automatically. Opens a menu to pick which\n"
        "author's method to use.");

    // --- row 2: envelopes + loop modules ---
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Envelope:");
    ImGui::SameLine();
    const char* envNames[] = {"None", "Volume", "Pan", "Separation", "Pitch"};
    // pan and separation only make sense in stereo; volume and pitch also
    // apply in mono.
    const bool envNeedsStereo[] = {false, false, true, true, false};
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        bool en = has && (!envNeedsStereo[i] || (d && d->channels() > 1));
        if (toolbarButton(envNames[i], en, envMode_ == i)) envMode_ = i;
    }
    ImGui::SameLine();
    if (toolbarButton("Apply envelopes", has && doc_.env.anyActive()))
        doc_.applyEnvelopes();
    ImGui::SetItemTooltip(
        "Prints the envelopes into the sample with sample-accurate "
        "precision;\nthey're then reset. Right-click on the waveform "
        "creates points.\nPitch (varispeed) changes local duration: the "
        "marked loop/selection\nare automatically relocated to their new "
        "position.");
    ImGui::SameLine(0, 24);
    if (toolbarButton(ICON_FA_MAGNET " Magnet", true, wave_.pointMagnetEnabled()))
        wave_.setPointMagnetEnabled(!wave_.pointMagnetEnabled());
    ImGui::SetItemTooltip(
        "Envelope magnet: when creating or dragging a point near the\n"
        "neutral center (0/1 depending on the envelope) or the value of\n"
        "another point already on the same envelope, it snaps exactly to\n"
        "that height. Disabled, points land exactly where they're dropped.");

    ImGui::SameLine(0, 24);
    if (toolbarButton(ICON_FA_UNDO " Undo", doc_.canUndo())) doc_.undo();
    ImGui::SameLine();
    if (toolbarButton(ICON_FA_REPEAT " Redo", doc_.canRedo())) doc_.redo();

    // Indicates whether a processing module is open (modules are now opened
    // from the Process menu, see drawMenuBar).
    for (auto& m : modules_) {
        if (!m->isOpen()) continue;
        ImGui::SameLine(0, 24);
        ImGui::TextColored(ImVec4(0.94f, 0.71f, 0.24f, 1.f), "Processing: %s",
                           m->name());
        break;
    }

    // sample rate popup
    if (srPopup_) {
        ImGui::OpenPopup("Sample rate");
        srPopup_ = false;
    }
    if (ImGui::BeginPopupModal("Sample rate", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputInt("New sample rate", &srTarget_);
        srTarget_ = std::min(384000, std::max(1000, srTarget_));
        ImGui::TextDisabled("No resampling: only changes the number (speed\n"
                            "and pitch shift). Resample: converts the\n"
                            "audio with high-quality sinc.");
        if (ImGui::Button("Change without resampling")) {
            doc_.changeSampleRate(srTarget_, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Resample (HQ)")) {
            doc_.changeSampleRate(srTarget_, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();

        ImGui::Separator();
        float tuneNow = eng_.params.tuneCents.load();
        if (tuneNow != 0.f) {
            ImGui::Text("Current tune: %.0f cents", tuneNow);
            if (ImGui::Button("Print tune into sample rate")) {
                doc_.printTuneToSampleRate();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemTooltip(
                "Multiplies the sample rate by 2^(cents/1200) without "
                "resampling and sets Tune to 0: same pitch, no longer "
                "depending on the tune parameter.");
        } else {
            ImGui::TextDisabled("Tune is already at 0: nothing to print.");
        }
        ImGui::EndPopup();
    }

    // trim silence popup
    if (trimSilencePopup_) {
        ImGui::OpenPopup("Trim Silence");
        trimSilencePopup_ = false;
    }
    if (ImGui::BeginPopupModal("Trim Silence", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SliderFloat("Threshold", &trimSilenceDb_, -100.f, -20.f, "%.0f dB",
                           ImGuiSliderFlags_AlwaysClamp);
        ImGui::TextWrapped(
            "Deletes leading/trailing silence, keeping only the region in "
            "between. -100 dB only removes exact digital silence; raise it "
            "to also trim a quiet noise floor at the start/end.");
        if (ImGui::Button("Apply")) {
            doc_.trimSilence(trimSilenceDb_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // auto loop popup
    if (autoLoopPopup_) {
        ImGui::OpenPopup("Auto Loop");
        autoLoopPopup_ = false;
        autoLoopResults_.clear();
    }
    if (ImGui::BeginPopupModal("Auto Loop", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Searches the audio for a repeating section and sets Loop "
            "Start/End to it automatically. Several methods are available, "
            "each from a different author -- pick one below.");

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Algorithm:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(320);
        if (ImGui::BeginCombo("##autoLoopAlgo", autoLoopAlgos_[autoLoopAlgoIdx_]->name())) {
            for (int i = 0; i < (int)autoLoopAlgos_.size(); ++i) {
                bool selected = (i == autoLoopAlgoIdx_);
                if (ImGui::Selectable(autoLoopAlgos_[i]->name(), selected)) {
                    if (autoLoopAlgoIdx_ != i) autoLoopResults_.clear();
                    autoLoopAlgoIdx_ = i;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Separator();
        AudioDataPtr dForParams = doc_.data();
        const double sr = dForParams ? dForParams->sampleRate : 44100.0;
        autoLoopAlgos_[autoLoopAlgoIdx_]->drawParams(sr);
        ImGui::Separator();

        const bool hasSel = doc_.hasSelection();
        ImGui::TextDisabled(hasSel ? "Searches only inside the current selection."
                                   : "No selection: searches the whole sample.");

        const bool has = doc_.hasAudio();
        ImGui::BeginDisabled(!has);
        if (ImGui::Button("Find Loop(s)", ImVec2(120, 0))) {
            AudioDataPtr d = doc_.data();
            int64_t s0 = 0, s1 = doc_.frames();
            if (hasSel) {
                s0 = doc_.selStart();
                s1 = doc_.selEnd();
            }
            autoLoopResults_ = autoLoopAlgos_[autoLoopAlgoIdx_]->run(*d, s0, s1);
            if (!autoLoopResults_.empty()) {
                const auto& best = autoLoopResults_[0];
                doc_.setLoopPoints(best.start, best.end, true);
                char buf[64];
                snprintf(buf, sizeof(buf), "Auto Loop: found %d candidate%s, applied #1",
                        (int)autoLoopResults_.size(), autoLoopResults_.size() == 1 ? "" : "s");
                pushToast(buf);
            } else {
                pushToast("Auto Loop: no loop found with the current parameters");
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            autoLoopResults_.clear();
            ImGui::CloseCurrentPopup();
        }

        if (!autoLoopResults_.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted("Candidates (best first) -- click one to try it, then Play/Loop to listen:");
            ImGui::BeginChild("##autoLoopResults", ImVec2(420, std::min(160.f, 22.f * autoLoopResults_.size())),
                              ImGuiChildFlags_Borders);
            for (int i = 0; i < (int)autoLoopResults_.size(); ++i) {
                const auto& c = autoLoopResults_[i];
                bool isCurrent = doc_.loopStart() == c.start && doc_.loopEnd() == c.end;
                char label[160];
                snprintf(label, sizeof(label), "#%d  %s .. %s%s%s", i + 1,
                        formatTime((double)c.start, sr).c_str(), formatTime((double)c.end, sr).c_str(),
                        c.info.empty() ? "" : "  ", c.info.c_str());
                ImGui::PushID(i);
                if (ImGui::Selectable(label, isCurrent)) doc_.setLoopPoints(c.start, c.end, true);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        ImGui::EndPopup();
    }
}

// ------------------------------------------------------------------ info

void EditorUI::drawInfoBar() {
    AudioDataPtr d = doc_.data();
    double sr = d ? d->sampleRate : 0;
    int64_t total = doc_.frames();
    ImGui::Text("Total: %lld samples (%s)", (long long)total,
                formatTime((double)total, sr).c_str());
    ImGui::SameLine(0, 24);
    if (doc_.hasSelection()) {
        int64_t s = doc_.selStart(), e = doc_.selEnd();
        ImGui::Text("Selection: %lld .. %lld (%lld samples | %s .. %s)",
                    (long long)s, (long long)e, (long long)(e - s),
                    formatTime((double)s, sr).c_str(),
                    formatTime((double)e, sr).c_str());
    } else {
        ImGui::TextDisabled("No selection");
    }
    ImGui::SameLine(0, 24);
    if (doc_.hasLoop())
        ImGui::Text("Loop: %lld .. %lld", (long long)doc_.loopStart(),
                    (long long)doc_.loopEnd());
    if (d) {
        ImGui::SameLine(0, 24);
        ImGui::TextDisabled("%s | %d channel%s | %s", formatName(d->format),
                            d->channels(), d->channels() > 1 ? "s" : "",
                            eng_.previewActive()      ? "LOOP PREVIEW"
                            : filter_.previewDoc()    ? "FILTER PREVIEW"
                            : amplitude_.previewDoc() ? "AMPLITUDE PREVIEW"
                            : activePitchPreview()    ? "PITCH PREVIEW"
                            : activeStretchPreview()  ? "STRETCH PREVIEW"
                                                      : "");
    }
}

// ---------------------------------------------------------------- spectrum

void EditorUI::updateLiveSpectrum() {
    constexpr int nFft = 2048; // ~43-46ms at 44.1/48kHz: good resolution/60fps-response tradeoff
    const int nBins = nFft / 2 + 1;
    if ((int)spectrumDb_.size() != nBins) spectrumDb_.assign((size_t)nBins, -60.f);

    const int64_t w = eng_.scopeCounter();
    if (!eng_.isPlaying() || w < nFft) {
        // No playback: decays toward the floor instead of freezing on the
        // last played frame (same criterion as the oscilloscope, which
        // simply draws nothing when nothing is sounding).
        for (auto& v : spectrumDb_) v = std::max(-60.f, v - 1.5f);
        return;
    }

    static const std::vector<double> win = [] {
        std::vector<double> ww((size_t)nFft);
        for (int i = 0; i < nFft; ++i) ww[(size_t)i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / nFft);
        return ww;
    }();
    std::vector<double> buf((size_t)nFft);
    for (int i = 0; i < nFft; ++i)
        buf[(size_t)i] = (double)eng_.scopeSample(w - nFft + i) * win[(size_t)i];
    auto X = dsp::rfft(buf);

    // Rough calibration: with a Hann window (coherent gain 0.5), the peak bin
    // of a full-scale sinusoid lands around nFft/4 -- so 0 dB ends up close
    // to "full scale" (an absolute level), not "the loudest bin right now" --
    // unlike a peak-normalized analysis, this lets low-frequency content show
    // up even when it's quiet.
    const double ref = nFft / 4.0;
    for (int k = 0; k < nBins; ++k) {
        float db = (float)(20.0 * std::log10(std::max(1e-9, std::abs(X[(size_t)k]) / ref)));
        float prev = spectrumDb_[(size_t)k];
        float coef = (db > prev) ? 0.5f : 0.15f; // fast attack, slower release
        spectrumDb_[(size_t)k] = prev + (db - prev) * coef;
    }
    spectrumNFft_ = nFft;
    spectrumSr_ = eng_.hostSampleRate();
}

// ---------------------------------------------------------- tune + scope + piano

void EditorUI::drawTuneAndScope() {
    const float scopeW = 250.f;
    float leftW = ImGui::GetContentRegionAvail().x - scopeW - 12.f;

    ImGui::BeginGroup();
    // tune slider above the piano
    ImGui::SetNextItemWidth(leftW);
    float cents = eng_.params.tuneCents.load();
    int centsInt = (int)std::lround(cents);
    if (ImGui::SliderInt("##tune", &centsInt, -99, 99,
                         "Tune: %d cents", ImGuiSliderFlags_AlwaysClamp))
        doc_.setTuneCents((float)centsInt);
    if (ImGui::IsItemClicked(1)) doc_.setTuneCents(0); // right-click: reset
    drawPiano(leftW);
    ImGui::EndGroup();

    // oscilloscope <-> spectrum on the right (click to toggle)
    ImGui::SameLine(0, 12);
    ImGui::BeginGroup();
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float h = 96.f + ImGui::GetFrameHeightWithSpacing() - 4.f;
        ImVec2 p1 = ImVec2(p0.x + scopeW, p0.y + h);

        ImGui::InvisibleButton("##scope", ImVec2(scopeW, h));
        if (ImGui::IsItemClicked()) scopeSpectral_ = !scopeSpectral_;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(scopeSpectral_ ? "Click: view oscilloscope"
                                             : "Click: view spectrum of the loaded audio");

        dl->AddRectFilled(p0, p1, IM_COL32(12, 16, 12, 255));
        dl->AddRect(p0, p1, IM_COL32(60, 70, 60, 255));

        if (scopeSpectral_) {
            updateLiveSpectrum();

            // Logarithmic X axis (20 Hz .. Nyquist or 20 kHz, whichever is
            // lower), live spectrum-analyzer style (Vital, etc.).
            const int nBins = (int)spectrumDb_.size();
            const double fMax = std::min(20000.0, spectrumSr_ * 0.5);
            const double logMin = std::log10(20.0), logMax = std::log10(fMax);
            const float dbFloor = -60.f; // displayed dynamic range
            const int cols = (int)scopeW - 2;

            auto dbAt = [&](double freq) {
                double bin = std::min((double)(nBins - 1),
                                      std::max(0.0, freq * spectrumNFft_ / spectrumSr_));
                int b0 = (int)bin, b1 = std::min(nBins - 1, b0 + 1);
                float frac = (float)(bin - b0);
                return spectrumDb_[(size_t)b0] + (spectrumDb_[(size_t)b1] - spectrumDb_[(size_t)b0]) * frac;
            };

            float prevX = 0, prevY = 0;
            bool first = true;
            for (int x = 0; x <= cols; ++x) {
                double t = (double)x / cols;
                double freq = std::pow(10.0, logMin + t * (logMax - logMin));
                float norm = std::clamp((dbAt(freq) - dbFloor) / -dbFloor, 0.f, 1.f);
                float px = p0.x + 1 + (float)x;
                float py = p1.y - norm * h;
                // fill: semi-transparent column from the curve down to the
                // base, same trick as the waveform's peak columns
                // (waveform_view.cpp) -- avoids relying on the shape being
                // convex.
                dl->AddLine(ImVec2(px, py), ImVec2(px, p1.y), IM_COL32(90, 170, 255, 45));
                if (!first)
                    dl->AddLine(ImVec2(prevX, prevY), ImVec2(px, py),
                                IM_COL32(130, 200, 255, 255), 1.3f);
                prevX = px; prevY = py; first = false;
            }
            // frequency reference marks
            struct { double f; const char* label; } marks[] = {
                {100.0, "100"}, {1000.0, "1k"}, {10000.0, "10k"}};
            for (auto& m : marks) {
                if (m.f < 20.0 || m.f > fMax) continue;
                double t = (std::log10(m.f) - logMin) / (logMax - logMin);
                float x = p0.x + (float)(t * cols);
                dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(50, 60, 70, 200));
                dl->AddText(ImVec2(x + 2, p1.y - 14), IM_COL32(110, 150, 170, 255), m.label);
            }
            dl->AddText(ImVec2(p0.x + 6, p0.y + 4), IM_COL32(150, 200, 255, 255),
                        eng_.isPlaying() ? "Live spectrum" : "Live spectrum (no signal)");
        } else {
            float cy = (p0.y + p1.y) * 0.5f;
            dl->AddLine(ImVec2(p0.x, cy), ImVec2(p1.x, cy),
                        IM_COL32(40, 60, 40, 255));

            int root = eng_.params.rootNote.load();
            double fRef = 440.0 * std::pow(2.0, (root - 69) / 12.0);
            double hostSr = eng_.hostSampleRate();
            double period = hostSr / fRef;
            const double nPeriods = 3.0;
            double window = period * nPeriods;
            int64_t w = eng_.scopeCounter();
            if (eng_.isPlaying() && w > (int64_t)window + 4) {
                // triggers on exact multiples of the period: if the pitch
                // matches the root note, the waveform looks still
                double start = std::floor(((double)w - window) / period) * period;
                float prevX = 0, prevY = 0;
                bool first = true;
                int cols = (int)scopeW - 2;
                for (int x = 0; x < cols; ++x) {
                    double pos = start + window * (double)x / (double)cols;
                    int64_t i0 = (int64_t)pos;
                    double fr = pos - (double)i0;
                    if (w - i0 >= Engine::kScopeSize || i0 + 1 >= w) continue;
                    float s0 = eng_.scopeSample(i0);
                    float s1 = eng_.scopeSample(i0 + 1);
                    float s = s0 + (float)fr * (s1 - s0);
                    float px = p0.x + 1 + (float)x;
                    float py = cy - s * (h * 0.46f);
                    if (!first)
                        dl->AddLine(ImVec2(prevX, prevY), ImVec2(px, py),
                                    IM_COL32(120, 255, 140, 255), 1.2f);
                    prevX = px; prevY = py; first = false;
                }
            }
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "Osc @ %s (%.1f Hz)",
                     noteName(root).c_str(), fRef);
            dl->AddText(ImVec2(p0.x + 6, p0.y + 4), IM_COL32(150, 200, 150, 255), lbl);
            dl->AddText(ImVec2(p0.x + 6, p1.y - 18), IM_COL32(110, 140, 110, 255),
                        "still = in tune");
        }
    }
    ImGui::EndGroup();
}

void EditorUI::drawPiano(float width) {
    const float pianoH = 88.f;
    const float whiteW = 13.f;
    // 128 MIDI notes: 75 white
    const bool blackMap[12] = {false, true, false, true, false, false,
                               true, false, true, false, true, false};
    float contentW = 76 * whiteW;

    ImGui::BeginChild("##piano",
                      ImVec2(width, pianoH + ImGui::GetStyle().ScrollbarSize + 4),
                      false, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pianokeys", ImVec2(contentW, pianoH),
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    const int root = eng_.params.rootNote.load();
    const int playingNote =
        eng_.isPlaying() ? eng_.currentNote() : -1;

    // per-key geometry
    struct Key { float x0, x1; bool black; };
    static Key keys[128];
    static bool geomDone = false;
    if (!geomDone) {
        geomDone = true;
        int whiteIdx = 0;
        for (int n = 0; n < 128; ++n) {
            if (!blackMap[n % 12]) {
                keys[n] = {whiteIdx * whiteW, (whiteIdx + 1) * whiteW, false};
                ++whiteIdx;
            } else {
                float cx = whiteIdx * whiteW;
                keys[n] = {cx - whiteW * 0.32f, cx + whiteW * 0.32f, true};
            }
        }
    }

    // hit-test: black keys first (they're on top)
    int hitNote = -1;
    if (hovered) {
        float mx = mouse.x - p0.x, my = mouse.y - p0.y;
        for (int n = 0; n < 128 && hitNote < 0; ++n)
            if (keys[n].black && my < pianoH * 0.62f && mx >= keys[n].x0 &&
                mx < keys[n].x1)
                hitNote = n;
        for (int n = 0; n < 128 && hitNote < 0; ++n)
            if (!keys[n].black && mx >= keys[n].x0 && mx < keys[n].x1)
                hitNote = n;
    }

    // drawing: white keys, then black
    for (int pass = 0; pass < 2; ++pass) {
        for (int n = 0; n < 128; ++n) {
            const Key& k = keys[n];
            if (k.black != (pass == 1)) continue;
            float x0 = p0.x + k.x0, x1 = p0.x + k.x1;
            float y1 = p0.y + (k.black ? pianoH * 0.62f : pianoH);
            ImU32 col;
            if (n == root)
                col = IM_COL32(240, 150, 40, 255); // marked root note
            else if (n == playingNote)
                col = IM_COL32(120, 190, 255, 255);
            else if (n == hitNote)
                col = k.black ? IM_COL32(90, 90, 100, 255)
                              : IM_COL32(230, 230, 235, 255);
            else
                col = k.black ? IM_COL32(25, 25, 30, 255)
                              : IM_COL32(245, 245, 248, 255);
            dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, y1), col);
            if (!k.black)
                dl->AddRect(ImVec2(x0, p0.y), ImVec2(x1, y1),
                            IM_COL32(90, 90, 95, 255));
            if (n % 12 == 0 && !k.black) // label on each C
                dl->AddText(ImVec2(x0 + 1, p0.y + pianoH - 16),
                            IM_COL32(90, 90, 95, 255),
                            noteName(n).c_str());
        }
    }

    if (hitNote >= 0) {
        ImGui::SetTooltip("%s (%d)%s", noteName(hitNote).c_str(), hitNote,
                          hitNote == root ? " - root note" : "");
        if (ImGui::IsItemClicked(0) && doc_.hasAudio())
            eng_.playFromNote(hitNote); // plays relative to the root note
        if (ImGui::IsItemClicked(1))
            doc_.setRootNote(hitNote); // right-click: sets the root note
    }

    // centers the view on the root note the first time
    static bool scrolled = false;
    if (!scrolled) {
        scrolled = true;
        ImGui::SetScrollX(std::max(0.f, keys[root].x0 - 300.f));
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ misc

void EditorUI::drawDialogs() {
    if (fileDlg_.draw()) {
        if (fileDlgPurpose_ == 0) {
            openPath(fileDlg_.result());
        } else {
            AudioDataPtr d = doc_.data();
            if (d) {
                exportBitsIdx_ = fileDlg_.optionIndex();
                const int bitDepths[] = {16, 24, 32};
                std::string err;
                bool scaledDown = false;
                if (exportWavSmpl(fileDlg_.result(), *d,
                                  bitDepths[std::min(2, std::max(0, exportBitsIdx_))],
                                  doc_.loopStart(), doc_.loopEnd(), doc_.rootNote(),
                                  doc_.tuneCents(), err, &scaledDown))
                    pushToast(scaledDown
                                  ? "Exported: " + fileDlg_.result() +
                                        " (amplitude scaled down to fit the chosen bit depth)"
                                  : "Exported: " + fileDlg_.result());
                else
                    pushToast("Error exporting: " + err);
            }
        }
    }
}

void EditorUI::drawToasts() {
    if (toasts_.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    float y = io.DisplaySize.y - 34.f;
    for (int i = (int)toasts_.size() - 1; i >= 0; --i) {
        toasts_[i].age += io.DeltaTime;
        float alpha = std::min(1.f, 3.0f - toasts_[i].age);
        if (alpha <= 0.f) {
            toasts_.erase(toasts_.begin() + i);
            continue;
        }
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 ts = ImGui::CalcTextSize(toasts_[i].text.c_str());
        ImVec2 p(io.DisplaySize.x - ts.x - 24.f, y);
        dl->AddRectFilled(ImVec2(p.x - 8, p.y - 4),
                          ImVec2(p.x + ts.x + 8, p.y + ts.y + 4),
                          IM_COL32(30, 30, 36, (int)(220 * alpha)), 4.f);
        dl->AddText(p, IM_COL32(230, 230, 235, (int)(255 * alpha)),
                    toasts_[i].text.c_str());
        y -= ts.y + 14.f;
    }
}

void EditorUI::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return; // don't steal keys from text fields
    const bool ctrl = io.KeyCtrl;

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (io.KeyShift) doc_.redo(); else doc_.undo();
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) doc_.redo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) doc_.cutSelection();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) doc_.copySelection();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) doc_.paste();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false) && doc_.hasAudio())
        doc_.setSelection(0, doc_.frames());
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        fileDlgPurpose_ = 0;
        fileDlg_.open(FileDialog::OpenFile, "Open audio",
                      {".wav", ".aiff", ".aif", ".flac", ".ogg"});
    }
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        if (doc_.hasSelection()) {
            doc_.deleteSelection();
        } else if (!anyModuleOpen() && doc_.hasAudio()) {
            // Delete with no selection and nothing else active: clear the
            // editor (undoable)
            eng_.pause();
            doc_.clearAll();
        }
    }
    // The space bar mirrors the button's Play <-> Stop toggle.
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (eng_.isRecording()) toggleRecord();
        else if (eng_.isPlaying()) stopPressed();
        else playPressed();
    }
}
