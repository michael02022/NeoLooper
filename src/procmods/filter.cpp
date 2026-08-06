#include "filter.hpp"

#include <algorithm>
#include <cmath>

#include "imgui.h"

#include "../engine.hpp"

void FilterTool::open(AudioDataPtr src, int64_t s0, int64_t s1) {
    src_ = std::move(src);
    s0_ = s0;
    s1_ = s1;
    open_ = true;
    bgDirty_ = true;
    prevDirty_ = true; // the first draw() publishes the initial preview
    lastEditT_ = 0.0;
    previewDoc_.reset();
}

void FilterTool::close(Engine& eng) {
    clearPreview(eng);
    open_ = false;
    src_.reset();
}

void FilterTool::clearPreview(Engine& eng) {
    previewDoc_.reset();
    eng.publishDocPreview(nullptr);
}

void FilterTool::rebuildPreview(Engine& eng) {
    prevDirty_ = false;
    if (!src_) return;
    auto filtered = filterdsp::apply(*src_, s0_, s1_, spec_);
    if (filtered.empty()) {
        clearPreview(eng);
        return;
    }
    // Full document with [s0_, s1_) replaced by the filtered version (same
    // length: the filter doesn't change duration) -- same scheme as
    // LoopModule::rebuildPreviewDoc, but with no splices to move.
    auto pd = std::make_shared<AudioData>();
    pd->sampleRate = src_->sampleRate;
    pd->format = src_->format;
    pd->ch = src_->ch;
    for (size_t c = 0; c < filtered.size() && c < pd->ch.size(); ++c)
        std::copy(filtered[c].begin(), filtered[c].end(),
                  pd->ch[c].begin() + (size_t)s0_);
    pd->buildPeaks();
    previewDoc_ = pd;
    eng.publishDocPreview(std::move(pd));
}

void FilterTool::rebuildSpectrum() {
    curve_.db.clear();
    if (!src_) return;
    filterdsp::analyzeStaticSpectrum(*src_, s0_, s1_, kFftSizes[fftSizeIdx_], /*maxFrames=*/150, curve_);
}

void FilterTool::draw(Document& doc, Engine& eng) {
    if (!open_) return;
    if (!src_ || s1_ <= s0_) {
        close(eng);
        return;
    }
    // Any widget that touches spec_ sets this; at the end of the frame it's
    // decided (with a debounce) whether to recompute the preview.
    bool specChanged = false;

    bool keep = true;
    // Initial size clamped to the available viewport -- same reason as the
    // previous Equalizer: if the host opens the editor canvas smaller than
    // requested, the window would otherwise be born taller than what's
    // visible and the clipping happens in the viewport (no scrolling
    // possible there).
    ImVec2 viewportSize = ImGui::GetMainViewport()->WorkSize;
    ImVec2 initSize(std::min(820.f, viewportSize.x - 20.f), std::min(600.f, viewportSize.y - 20.f));
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(initSize, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Filter", &keep, ImGuiWindowFlags_NoCollapse)) {
        const int64_t L = s1_ - s0_;
        ImGui::Text("Region: %lld samples (%.1f ms)%s", (long long)L,
                    1000.0 * (double)L / src_->sampleRate,
                    (s0_ == 0 && s1_ == src_->frames()) ? " -- whole sample" : " -- selection");
        ImGui::TextWrapped(
            "Resonant high-pass + bell band (boost or cut). The background is "
            "a STATIC analysis of this region's real spectrum (not updated "
            "live); the bright line is the current filter's response.");
        if (ImGui::Checkbox("Preview", &previewOn_)) {
            if (previewOn_) {
                prevDirty_ = true;
                lastEditT_ = 0.0; // recompute right away, skip the debounce
            } else {
                clearPreview(eng);
            }
        }
        ImGui::SetItemTooltip(
            "The waveform and playback use the document's filtered version "
            "(without applying anything): loop markers, selection and "
            "envelopes keep working as usual. Recomputed when a control is "
            "released or paused, not on every pixel of a drag.");

        static const char* kSizeLabels[] = {"512", "1024", "2048", "4096", "8192"};
        ImGui::SetNextItemWidth(120);
        if (ImGui::Combo("FFT size (analysis)", &fftSizeIdx_, kSizeLabels, 5)) bgDirty_ = true;
        ImGui::SameLine(0, 20);
        ImGui::SetNextItemWidth(200);
        ImGui::SliderFloat("Range (dB)", &dbFloor_, -100.f, -20.f, "%.0f dB",
                           ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip(
            "Vertical zoom of the background spectrum -- purely visual, "
            "doesn't change the analysis or the filter.");

        if (bgDirty_) {
            rebuildSpectrum();
            bgDirty_ = false;
        }

        // Only the footer (Apply/Reset/Cancel) has its height reserved
        // strictly (footerH, negative height -- same trick as FileDialog):
        // the content above (graph + band table) goes in its own child WITH
        // a scrollbar if needed, so that if something doesn't fit, a scroll
        // appears instead of covering the buttons (the bug the previous
        // Equalizer had).
        const float footerH = ImGui::GetFrameHeightWithSpacing() + 4.f;
        ImGui::BeginChild("##filtercontent", ImVec2(0, -footerH), false);
        {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            // Rough reservation for the band table below (1 header + 2
            // rows); if it falls short, the outer child scrolls, nothing
            // gets covered.
            const float panelH = std::max(220.f, avail.y - 130.f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1 = ImVec2(p0.x + avail.x, p0.y + panelH);

            dl->AddRectFilled(p0, p1, IM_COL32(10, 12, 14, 255));
            dl->AddRect(p0, p1, IM_COL32(60, 65, 70, 255));

            const double sr = src_->sampleRate;
            const double fMax = std::min(20000.0, sr * 0.5);
            const double logMin = std::log10(20.0), logMax = std::log10(fMax);
            const int cols = std::max(1, (int)(p1.x - p0.x));
            auto logFreqToX = [&](double lf) {
                double t = (lf - logMin) / (logMax - logMin);
                return p0.x + (float)(t * (double)(p1.x - p0.x));
            };
            // Fixed +24 dB headroom above 0: the band can BOOST (and the
            // high-pass's Q can resonate), and that bump has to be visible
            // on the curve, not clipped against the top edge.
            const float dbTop = 24.f;
            auto dbToY = [&](float db) {
                float t = std::clamp((db - dbFloor_) / (dbTop - dbFloor_), 0.f, 1.f);
                return p1.y - t * panelH;
            };

            dl->PushClipRect(p0, p1, true);

            // real spectrum: a SINGLE averaged curve (not a per-frame point
            // cloud like the previous attempt), same style as the
            // oscilloscope's live spectrum -- line + fill.
            if (!curve_.db.empty()) {
                const int nBins = (int)curve_.db.size();
                auto dbAt = [&](double freq) {
                    double bin = std::min((double)(nBins - 1),
                                          std::max(0.0, freq * curve_.nFft / curve_.sr));
                    int b0 = (int)bin, b1 = std::min(nBins - 1, b0 + 1);
                    float frac = (float)(bin - b0);
                    return curve_.db[(size_t)b0] + (curve_.db[(size_t)b1] - curve_.db[(size_t)b0]) * frac;
                };
                float prevX = 0.f, prevY = 0.f;
                bool first = true;
                for (int x = 0; x <= cols; ++x) {
                    double t = (double)x / cols;
                    double freq = std::pow(10.0, logMin + t * (logMax - logMin));
                    float py = dbToY(dbAt(freq));
                    float px = p0.x + (float)x;
                    dl->AddLine(ImVec2(px, py), ImVec2(px, p1.y), IM_COL32(90, 170, 255, 40));
                    if (!first)
                        dl->AddLine(ImVec2(prevX, prevY), ImVec2(px, py), IM_COL32(130, 200, 255, 230), 1.3f);
                    prevX = px; prevY = py; first = false;
                }
            } else {
                const char* msg = "Region too short to analyze";
                ImVec2 ts = ImGui::CalcTextSize(msg);
                dl->AddText(ImVec2(p0.x + (avail.x - ts.x) * 0.5f, p0.y + (panelH - ts.y) * 0.5f),
                            IM_COL32(140, 140, 150, 255), msg);
            }

            // grid: 0 dB line + frequency marks
            dl->AddLine(ImVec2(p0.x, dbToY(0.f)), ImVec2(p1.x, dbToY(0.f)), IM_COL32(70, 80, 90, 160));
            struct { double f; const char* label; } marks[] = {
                {20, "20"}, {50, "50"}, {100, "100"}, {200, "200"}, {500, "500"},
                {1000, "1k"}, {2000, "2k"}, {5000, "5k"}, {10000, "10k"}, {20000, "20k"}};
            for (auto& m : marks) {
                if (m.f < 20.0 || m.f > fMax) continue;
                float x = logFreqToX(std::log10(m.f));
                dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(40, 45, 50, 140));
                dl->AddText(ImVec2(x + 2, p1.y - 14), IM_COL32(120, 130, 140, 255), m.label);
            }

            // combined filter's response curve, overlaid on the real
            // spectrum -- to see where the cutoff lands relative to the
            // real content.
            {
                float prevX = 0.f, prevY = 0.f;
                bool first = true;
                for (int x = 0; x <= cols; ++x) {
                    double t = (double)x / cols;
                    double freq = std::pow(10.0, logMin + t * (logMax - logMin));
                    double gain = filterdsp::responseAt(spec_, freq, sr);
                    float db = (float)(20.0 * std::log10(std::max(1e-6, gain)));
                    float py = dbToY(db);
                    float px = p0.x + (float)x;
                    if (!first)
                        dl->AddLine(ImVec2(prevX, prevY), ImVec2(px, py), IM_COL32(255, 210, 90, 235), 2.f);
                    prevX = px; prevY = py; first = false;
                }
            }

            dl->PopClipRect();
            ImGui::Dummy(ImVec2(avail.x, panelH));

            // ---- band table ----
            ImGui::Separator();
            if (ImGui::BeginTable("##bands", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Band", ImGuiTableColumnFlags_WidthFixed, 80.f);
                ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 55.f);
                // Frequency gets twice the width of gain/Q: it's the control
                // that needs the most drag precision (20-20k log range) --
                // explicit user request.
                ImGui::TableSetupColumn("Frequency", ImGuiTableColumnFlags_WidthStretch, 2.f);
                ImGui::TableSetupColumn("Gain / Slope", ImGuiTableColumnFlags_WidthStretch, 1.f);
                ImGui::TableSetupColumn("Q / Width", ImGuiTableColumnFlags_WidthStretch, 1.f);
                ImGui::TableHeadersRow();

                // Turning it off does NOT reset anything: the on/off flag
                // lives separately from the parameters, which keep their
                // value for when it's turned back on.

                // --- High-Pass ---
                ImGui::PushID("hp");
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("High-Pass");
                ImGui::TableNextColumn();
                specChanged |= ImGui::Checkbox("##on", &spec_.hpOn);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                specChanged |=
                    ImGui::SliderFloat("##freq", &spec_.hpFreq, 20.f, 2000.f, "%.0f Hz",
                                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
                ImGui::TableNextColumn();
                {
                    static const char* kSlopeLabels[] = {"12 dB/oct", "24 dB/oct", "36 dB/oct",
                                                         "48 dB/oct", "60 dB/oct", "72 dB/oct",
                                                         "84 dB/oct", "96 dB/oct"};
                    int slopeIdx = spec_.hpStages - 1;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##slope", &slopeIdx, kSlopeLabels, filterdsp::kMaxStages)) {
                        spec_.hpStages = slopeIdx + 1;
                        specChanged = true;
                    }
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                specChanged |= ImGui::SliderFloat("##q", &spec_.hpQ, 0.3f, 20.f, "%.2f",
                                                  ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetItemTooltip(
                    "0.71 = no resonance (Butterworth); higher = resonant "
                    "peak at the cutoff. Q applies only to the first stage; "
                    "the rest only add slope.");
                ImGui::PopID();

                // --- Band (bell) ---
                ImGui::PushID("band");
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Band");
                ImGui::TableNextColumn();
                specChanged |= ImGui::Checkbox("##on", &spec_.bandOn);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                specChanged |=
                    ImGui::SliderFloat("##freq", &spec_.bandFreq, 20.f, 20000.f, "%.0f Hz",
                                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                specChanged |= ImGui::SliderFloat("##gain", &spec_.bandGainDb, -24.f, 24.f,
                                                  "%+.1f dB", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetItemTooltip(
                    "Positive boosts (e.g. brightness in the highs), negative "
                    "cuts. Middle-click to type the exact value.");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                specChanged |= ImGui::SliderFloat("##q", &spec_.bandQ, 0.3f, 20.f, "%.2f",
                                                  ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetItemTooltip(
                    "Bell width: lower Q = wider (more musical for adding "
                    "brightness), higher Q = more surgical.");
                ImGui::PopID();
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button("Apply", ImVec2(120, 0))) {
            doc.applyFilter(spec_);
            keep = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(120, 0))) {
            spec_ = filterdsp::FilterSpec{};
            specChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) keep = false;

        // Preview debounce: recomputing by filtering the whole region (+
        // waveform peaks) on every pixel of a drag would be too expensive on
        // long samples. It's recomputed once values have been still for a
        // moment or a control is released; a "discrete" change (checkbox,
        // combo, Reset) falls into the "no item active" branch on the next
        // frame.
        if (specChanged) {
            prevDirty_ = true;
            lastEditT_ = ImGui::GetTime();
        }
        if (previewOn_ && prevDirty_ &&
            (ImGui::GetTime() - lastEditT_ > 0.12 || !ImGui::IsAnyItemActive()))
            rebuildPreview(eng);
    }
    ImGui::End();
    if (!keep) close(eng);
}
