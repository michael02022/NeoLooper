#include "autotune.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "imgui.h"

#include "../engine.hpp"

namespace {
// Same naming as the piano/toolbar (C4 = 60).
void noteLabel(int n, char* buf, size_t size) {
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F",
                                     "F#", "G", "G#", "A", "A#", "B"};
    if (n < 0) {
        snprintf(buf, size, "-");
        return;
    }
    snprintf(buf, size, "%s%d", kNames[n % 12], n / 12 - 1);
}
} // namespace

namespace {
bool sameSpec(const autotunedsp::Spec& a, const autotunedsp::Spec& b) {
    return a.useMidi == b.useMidi && (!a.useMidi || a.rootNote == b.rootNote) &&
           a.amountPct == b.amountPct && a.lowNotes == b.lowNotes;
}
} // namespace

void AutotuneTool::open(AudioDataPtr src, int64_t s0, int64_t s1) {
    stopWorker();
    src_ = std::move(src);
    s0_ = s0;
    s1_ = s1;
    open_ = true;
    dirty_ = true; // the first draw() renders
    lastEditT_ = 0.0;
    resultOk_ = false;
    result_ = autotunedsp::Result{};
    previewDoc_.reset();
}

void AutotuneTool::close(Engine& eng) {
    stopWorker();
    clearPreview(eng);
    open_ = false;
    src_.reset();
    result_ = autotunedsp::Result{};
    resultOk_ = false;
}

void AutotuneTool::clearPreview(Engine& eng) {
    previewDoc_.reset();
    eng.publishDocPreview(nullptr);
}

void AutotuneTool::stopWorker() {
    if (worker_.joinable()) {
        cancel_.store(true);
        worker_.join();
    }
    rendering_ = false;
}

void AutotuneTool::startRender() {
    dirty_ = false;
    stopWorker();
    if (!src_) return;
    cancel_.store(false);
    done_.store(false);
    progress_.store(0.f);
    pendingSpec_ = spec_;
    rendering_ = true;
    // The worker only reads src_ (an immutable snapshot it keeps alive) and
    // writes pending_*; the GUI reads those only after done_.
    worker_ = std::thread([this, src = src_, s0 = s0_, s1 = s1_, spec = spec_] {
        const auto t0 = std::chrono::steady_clock::now();
        pendingOk_ = autotunedsp::apply(*src, s0, s1, spec, pending_, &cancel_, &progress_);
        pendingMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        done_.store(true, std::memory_order_release);
    });
}

void AutotuneTool::pollRender(Engine& eng) {
    if (!rendering_ || !done_.load(std::memory_order_acquire)) return;
    worker_.join();
    rendering_ = false;
    if (!pendingOk_) return; // cancelled or failed: keep what's on screen
    result_ = std::move(pending_);
    pending_ = autotunedsp::Result{};
    resultSpec_ = pendingSpec_;
    resultOk_ = true;
    renderMs_ = pendingMs_;
    if (previewOn_) publishPreview(eng);
}

void AutotuneTool::publishPreview(Engine& eng) {
    if (!resultOk_ || !src_) return;
    // Full document with [s0_, s1_) replaced (same length: the correction
    // doesn't change duration) -- same scheme as FilterTool::rebuildPreview.
    auto pd = std::make_shared<AudioData>();
    pd->sampleRate = src_->sampleRate;
    pd->format = src_->format;
    pd->ch = src_->ch;
    for (size_t c = 0; c < result_.audio.size() && c < pd->ch.size(); ++c)
        std::copy(result_.audio[c].begin(), result_.audio[c].end(), pd->ch[c].begin() + (size_t)s0_);
    pd->buildPeaks();
    previewDoc_ = pd;
    eng.publishDocPreview(std::move(pd));
}

void AutotuneTool::drawPitchGraph(Engine& eng, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + height);
    ImGui::InvisibleButton("##pitchgraph", ImVec2(w, height));
    const bool hovered = ImGui::IsItemHovered();

    dl->AddRectFilled(p0, p1, IM_COL32(14, 16, 20, 255));
    dl->AddRect(p0, p1, IM_COL32(60, 66, 76, 255));

    const auto& curve = result_.curve;
    // Vertical range: symmetric, at least +-50 cents (a semitone each way
    // around the target), widened to fit the largest deviation (capped at
    // an octave so a stray octave error doesn't flatten everything).
    float maxAbs = 50.f;
    for (const auto& p : curve)
        if (p.voiced && p.targetNote >= 0)
            maxAbs = std::max(maxAbs, std::fabs((p.inputNote - (float)p.targetNote) * 100.f));
    maxAbs = std::min(1200.f, std::ceil(maxAbs * 1.1f / 25.f) * 25.f);

    const double L = (double)(s1_ - s0_);
    auto xOf = [&](int64_t pos) { return p0.x + (float)((double)(pos - s0_) / L) * w; };
    auto yOf = [&](float centsDev) {
        float t = std::clamp(centsDev / maxAbs, -1.f, 1.f);
        return (p0.y + p1.y) * 0.5f - t * (height * 0.5f - 4.f);
    };

    // Grid: 0 = in tune, plus the +-50 cents lines (halfway to the next note).
    dl->AddLine(ImVec2(p0.x, yOf(0.f)), ImVec2(p1.x, yOf(0.f)), IM_COL32(90, 100, 110, 255));
    for (float g : {-50.f, 50.f})
        if (g > -maxAbs && g < maxAbs)
            dl->AddLine(ImVec2(p0.x, yOf(g)), ImVec2(p1.x, yOf(g)), IM_COL32(45, 50, 58, 255));
    char lbl[32];
    snprintf(lbl, sizeof(lbl), "+%.0f", maxAbs);
    dl->AddText(ImVec2(p0.x + 4, p0.y + 2), IM_COL32(110, 120, 130, 255), lbl);
    snprintf(lbl, sizeof(lbl), "-%.0f cents", maxAbs);
    dl->AddText(ImVec2(p0.x + 4, p1.y - 16), IM_COL32(110, 120, 130, 255), lbl);

    // Curves: detected pitch (orange) and corrected pitch (green), both as
    // the deviation from the hop's target note. Breaks where unvoiced or
    // the target changes (a note change isn't a deviation).
    auto drawCurve = [&](bool output, ImU32 col, float thick) {
        bool have = false;
        ImVec2 prev;
        int prevTarget = -1;
        for (const auto& p : curve) {
            if (!p.voiced || p.targetNote < 0) {
                have = false;
                continue;
            }
            const float note = output ? p.outputNote : p.inputNote;
            const ImVec2 cur(xOf(p.pos), yOf((note - (float)p.targetNote) * 100.f));
            if (have && p.targetNote == prevTarget && cur.x - prev.x < 12.f)
                dl->AddLine(prev, cur, col, thick);
            prev = cur;
            prevTarget = p.targetNote;
            have = true;
        }
    };
    drawCurve(false, IM_COL32(240, 150, 50, 220), 1.4f);
    drawCurve(true, IM_COL32(90, 220, 120, 255), 1.8f);

    if (curve.empty() || std::none_of(curve.begin(), curve.end(),
                                      [](const autotunedsp::PitchPoint& p) { return p.voiced; })) {
        const float cy = (p0.y + p1.y) * 0.5f;
        if (!resultOk_) {
            dl->AddText(ImVec2(p0.x + 8, cy - 8), IM_COL32(160, 160, 160, 255), "Analysing...");
        } else {
            // Backdrop so the grid lines don't run through the message.
            dl->AddRectFilled(ImVec2(p0.x + 1, cy - 22), ImVec2(p1.x - 1, cy + 20),
                              IM_COL32(14, 16, 20, 255));
            dl->AddText(ImVec2(p0.x + 8, cy - 18), IM_COL32(160, 160, 160, 255),
                        "No pitched audio detected in this region");
            // The usual reason: the note is below the detector's range
            // (65 Hz) -- the low-notes mode reaches an octave further down.
            dl->AddText(ImVec2(p0.x + 8, cy + 2), IM_COL32(240, 190, 90, 255),
                        spec_.lowNotes
                            ? "The pitch may be outside the detectable range (~33 Hz - 800 Hz here)."
                            : "If it's a low note (below ~65 Hz / C2), try enabling \"Process for low notes\".");
        }
    }

    // Where the readout looks: the mouse if it's over the graph, else the
    // playhead while it's inside the region (moves in real time as the
    // preview plays).
    int64_t at = -1;
    if (hovered) {
        at = s0_ + (int64_t)((ImGui::GetIO().MousePos.x - p0.x) / w * L);
    } else {
        const int64_t ph = (int64_t)eng.playheadFrame();
        if (eng.isPlaying() && ph >= s0_ && ph < s1_) at = ph;
    }
    if (eng.isPlaying()) {
        const int64_t ph = (int64_t)eng.playheadFrame();
        if (ph >= s0_ && ph < s1_)
            dl->AddLine(ImVec2(xOf(ph), p0.y), ImVec2(xOf(ph), p1.y), IM_COL32(255, 255, 255, 160));
    }
    if (at >= 0 && !curve.empty()) {
        auto it = std::lower_bound(curve.begin(), curve.end(), at,
                                   [](const autotunedsp::PitchPoint& p, int64_t v) { return p.pos < v; });
        if (it == curve.end()) --it;
        if (it->voiced && it->targetNote >= 0) {
            char note[16];
            noteLabel(it->targetNote, note, sizeof(note));
            char buf[96];
            snprintf(buf, sizeof(buf), "%s: %+.0f -> %+.0f cents", note,
                     (it->inputNote - (float)it->targetNote) * 100.f,
                     (it->outputNote - (float)it->targetNote) * 100.f);
            const ImVec2 ts = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(p1.x - ts.x - 6, p0.y + 2), IM_COL32(230, 230, 230, 255), buf);
        }
    }
}

void AutotuneTool::draw(Document& doc, Engine& eng) {
    if (!open_) return;
    if (!src_ || s1_ <= s0_) {
        close(eng);
        return;
    }
    pollRender(eng);
    bool specChanged = false;
    // The root note is edited outside this window (toolbar/piano): with
    // Use MIDI on, a change retargets the correction.
    if (doc.rootNote() != spec_.rootNote) {
        spec_.rootNote = doc.rootNote();
        if (spec_.useMidi) specChanged = true;
    }

    bool keep = true;
    ImVec2 viewportSize = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::min(640.f, viewportSize.x - 20.f),
                                    std::min(460.f, viewportSize.y - 20.f)),
                             ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Autotune", &keep, ImGuiWindowFlags_NoCollapse)) {
        const int64_t L = s1_ - s0_;
        ImGui::Text("Region: %lld samples (%.1f ms)%s", (long long)L,
                    1000.0 * (double)L / src_->sampleRate,
                    (s0_ == 0 && s1_ == src_->frames()) ? " -- whole sample" : " -- selection");
        ImGui::TextWrapped(
            "Pitch correction by Snaptune (Studio mode: TD-PSOLA, keeps the "
            "formants). Snaps the pitch to the nearest semitone, or to the "
            "root note with Use MIDI.");

        char root[16];
        noteLabel(spec_.rootNote, root, sizeof(root));
        char midiLabel[48];
        snprintf(midiLabel, sizeof(midiLabel), "Use MIDI (root note: %s)", root);
        specChanged |= ImGui::Checkbox(midiLabel, &spec_.useMidi);
        ImGui::SetItemTooltip(
            "Corrects toward the sample's root note (as if that MIDI note were\n"
            "held: Snaptune's MIDI Target Notes) instead of the nearest semitone.\n"
            "Any octave of the root note counts; set it with right-click on the piano.");

        ImGui::SetNextItemWidth(-90.f);
        specChanged |= ImGui::SliderFloat("Amount", &spec_.amountPct, 0.f, 100.f, "%.0f%%",
                                          ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemClicked(1)) { // right-click: reset
            spec_.amountPct = 100.f;
            specChanged = true;
        }
        ImGui::SetItemTooltip("How much of the pitch error is corrected (100%% = all of it).\n"
                              "Right-click resets to 100%%.");

        specChanged |= ImGui::Checkbox("Process for low notes", &spec_.lowNotes);
        ImGui::SetItemTooltip(
            "Processes the sample an octave up and brings the result back\n"
            "down, so notes below the detector's range (65 Hz, about C2) can be\n"
            "tuned -- down to ~33 Hz (about C1).");

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.94f, 0.59f, 0.2f, 1.f), "Detected");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.35f, 0.86f, 0.47f, 1.f), "Corrected");
        ImGui::SameLine();
        ImGui::TextDisabled("(cents from the target note; hover to read)");
        drawPitchGraph(eng, 150.f);
        if (rendering_)
            ImGui::TextDisabled("Rendering... %.0f%%", 100.f * progress_.load());
        else if (resultOk_)
            ImGui::TextDisabled("Rendered in %.0f ms", renderMs_);

        if (ImGui::Checkbox("Preview", &previewOn_)) {
            if (previewOn_) {
                if (resultOk_ && sameSpec(resultSpec_, spec_)) publishPreview(eng);
                else dirty_ = true;
            } else {
                clearPreview(eng);
            }
        }
        ImGui::SetItemTooltip(
            "The waveform and playback use the corrected version (without\n"
            "applying anything). Recomputed when a control is released.");

        ImGui::Separator();
        if (ImGui::Button("Apply", ImVec2(120, 0))) {
            // A render of these very settings still in flight: let it
            // finish (cheaper than starting over in Document).
            if (rendering_ && !dirty_ && sameSpec(pendingSpec_, spec_)) {
                worker_.join();
                done_.store(true);
                pollRender(eng);
            }
            // Reuse the finished render when it's of this exact snapshot,
            // range and settings; otherwise Document corrects again.
            const bool fresh = resultOk_ && !rendering_ && !dirty_ && sameSpec(resultSpec_, spec_) &&
                               doc.data() == src_;
            stopWorker();
            doc.applyAutotune(spec_, fresh ? &result_.audio : nullptr);
            keep = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(120, 0))) {
            const int rootNote = spec_.rootNote;
            spec_ = autotunedsp::Spec{};
            spec_.rootNote = rootNote;
            specChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) keep = false;

        // Same debounce as FilterTool: the correction renders the whole
        // region, too expensive for every pixel of a drag.
        if (specChanged) {
            dirty_ = true;
            lastEditT_ = ImGui::GetTime();
        }
        if (dirty_ && (ImGui::GetTime() - lastEditT_ > 0.15 || !ImGui::IsAnyItemActive()))
            startRender();
    }
    ImGui::End();
    if (!keep) close(eng);
}
