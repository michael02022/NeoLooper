#include "waveform_view.hpp"

#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "imgui_internal.h"

namespace {
const ImU32 kColBg = IM_COL32(24, 26, 30, 255);
const ImU32 kColWave = IM_COL32(120, 200, 120, 255);
const ImU32 kColWaveDim = IM_COL32(80, 140, 80, 255);
const ImU32 kColCenter = IM_COL32(70, 75, 80, 255);
const ImU32 kColSel = IM_COL32(90, 130, 200, 60);
const ImU32 kColSelBorder = IM_COL32(120, 160, 240, 180);
const ImU32 kColLoop = IM_COL32(240, 180, 60, 255);
const ImU32 kColLoopFill = IM_COL32(240, 180, 60, 24);
const ImU32 kColPlayhead = IM_COL32(240, 240, 240, 200);
const ImU32 kColEnv = IM_COL32(240, 120, 200, 255);
const ImU32 kColEnvPt = IM_COL32(255, 255, 255, 255);

Envelope* pickEnv(Envelopes& env, int mode) {
    switch (mode) {
        case 1: return &env.amp;
        case 2: return &env.pan;
        case 3: return &env.sep;
        case 4: return &env.pitch;
        default: return nullptr;
    }
}
} // namespace

void WaveformView::clampView(int64_t n, float width) {
    double minSpp = 0.02; // 50 px per sample at max zoom
    double maxSpp = std::max(minSpp, (double)n / std::max(1.0, (double)width));
    spp_ = std::min(maxSpp, std::max(minSpp, spp_));
    double maxStart = (double)n - spp_ * width;
    viewStart_ = std::min(std::max(0.0, maxStart), std::max(0.0, viewStart_));
}

void WaveformView::draw(Document& doc, Engine& eng, Envelopes& env,
                        int envMode, float height, AudioDataPtr overrideDoc,
                        int64_t ovLoopStart, int64_t ovLoopEnd) {
    // In override mode (module preview) the waveform is display-only: no
    // selection, markers or envelopes; the real document hasn't changed.
    const bool readOnly = overrideDoc != nullptr;
    if (readOnly) envMode = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    const float sbH = 14.f; // own scrollbar
    float waveH = std::max(60.f, height - sbH);

    ImVec2 rectMin = origin;
    ImVec2 rectMax = ImVec2(origin.x + width, origin.y + waveH);

    dl->AddRectFilled(rectMin, rectMax, kColBg);
    dl->AddRect(rectMin, rectMax,
                readOnly ? kColLoop : IM_COL32(60, 60, 66, 255),
                0.f, 0, readOnly ? 2.f : 1.f);
    if (readOnly)
        dl->AddText(ImVec2(rectMin.x + 6, rectMin.y + 4), kColLoop,
                    "PREVIEW (not applied)");

    AudioDataPtr dp = readOnly ? overrideDoc : doc.data();
    if (!dp || dp->frames() == 0) {
        const char* msg = "Drop audio here  (WAV / AIFF / FLAC / OGG / MP3)  or  File -> Open";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(origin.x + (width - ts.x) * 0.5f,
                           origin.y + (waveH - ts.y) * 0.5f),
                    IM_COL32(140, 140, 150, 255), msg);
        ImGui::Dummy(ImVec2(width, height));
        lastDoc_ = nullptr;
        return;
    }

    const AudioData& d = *dp;
    const int64_t n = d.frames();
    if (lastDoc_ != dp.get()) {
        // Freshly imported sample (Open/drag&drop/host state): always
        // reframes to the full waveform, regardless of whatever zoom was
        // active. For an EDIT of the current sample (cut/paste/apply
        // module...), on the other hand, it only reframes if it was already
        // fully visible or the scroll ended up out of range -- keeps the
        // zoom during editing.
        bool forceFull = !readOnly && doc.consumeFullViewRequest();
        if (forceFull || !lastDoc_ || std::abs((double)n - spp_ * width) < spp_ * 4 ||
            viewStart_ >= (double)n) {
            spp_ = (double)n / std::max(1.0, (double)width);
            viewStart_ = 0.0;
        }
        lastDoc_ = dp.get();
    }
    clampView(n, width);
    const int nch = d.channels();

    auto xToFrame = [&](float x) {
        return viewStart_ + (double)(x - origin.x) * spp_;
    };
    auto frameToX = [&](double f) {
        return origin.x + (float)((f - viewStart_) / spp_);
    };

    // ---------------- interaction ----------------
    ImGui::InvisibleButton("##wave", ImVec2(width, waveH),
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;
    ImGuiIO& io = ImGui::GetIO();

    // wheel zoom, centered on the mouse (Polyphone-style)
    if (hovered && io.MouseWheel != 0.f) {
        double mf = xToFrame(mouse.x);
        double factor = std::pow(1.30, (double)io.MouseWheel);
        spp_ /= factor;
        clampView(n, width);
        viewStart_ = mf - (double)(mouse.x - origin.x) * spp_;
        clampView(n, width);
    }
    // horizontal panning with shift+wheel
    if (hovered && io.MouseWheelH != 0.f) {
        viewStart_ += (double)io.MouseWheelH * spp_ * 40.0;
        clampView(n, width);
    }

    int64_t ls = readOnly ? ovLoopStart : doc.loopStart();
    int64_t le = readOnly ? ovLoopEnd : doc.loopEnd();
    const bool hasLoop =
        readOnly ? (ovLoopStart >= 0 && ovLoopEnd > ovLoopStart)
                 : doc.hasLoop();

    Envelope* envSel = pickEnv(env, envMode);

    // hit-test for envelope points (takes priority over everything else)
    int envHit = -1;
    if (envSel && hovered) {
        for (int i = 0; i < (int)envSel->pts.size(); ++i) {
            float px = frameToX(envSel->pts[i].frame);
            float t = (envSel->pts[i].value - envSel->minValue) /
                      (envSel->maxValue - envSel->minValue);
            float py = rectMax.y - t * waveH;
            float dx = mouse.x - px, dy = mouse.y - py;
            if (dx * dx + dy * dy < 36.f) { envHit = i; break; }
        }
    }

    // hit-test for loop markers
    auto nearX = [&](double f) {
        return hasLoop && std::fabs(frameToX(f) - mouse.x) < 5.f;
    };
    bool overLS = hovered && !readOnly && envHit < 0 && nearX((double)ls);
    bool overLE = hovered && !readOnly && envHit < 0 && !overLS && nearX((double)le);
    if (overLS || overLE)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // Envelope magnet, governed by pointMagnet_ (the "Magnet" button): snaps
    // toward the neutral value (1.0 for volume, 0 for pan/sep) and toward
    // the height of any OTHER point already on the same envelope -- makes it
    // easy to create flat segments, at the neutral height or matching
    // another point, without having to hit the value by hand. excludeIdx is
    // the point being moved (so it isn't compared against its own old
    // value); -1 for a new point that isn't in pts yet.
    auto envSnap = [&](float v, int excludeIdx) {
        if (!envSel || !pointMagnet_) return v;
        const float th = 0.04f * (envSel->maxValue - envSel->minValue);
        if (std::fabs(v - envSel->defaultValue) < th) return envSel->defaultValue;
        for (int i = 0; i < (int)envSel->pts.size(); ++i) {
            if (i == excludeIdx) continue;
            if (std::fabs(v - envSel->pts[i].value) < th) return envSel->pts[i].value;
        }
        return v;
    };

    if (hovered && !readOnly && envHit < 0 && !overLS && !overLE &&
        ImGui::IsMouseDoubleClicked(0)) {
        // Double-click: select the "block" the click landed in -- the
        // region bounded by the sample start/end and, if there's a marked
        // loop, whichever of loopStart/loopEnd bracket the click. Lets a
        // double-click grab "before the loop", "the loop", or "after the
        // loop" as one selection, for quick block-wise processing.
        int64_t click = (int64_t)std::llround(xToFrame(mouse.x));
        click = std::min<int64_t>(std::max<int64_t>(0, click), n);
        int64_t zoneLo = 0, zoneHi = n;
        if (hasLoop) {
            if (click < ls) zoneHi = ls;
            else if (click < le) { zoneLo = ls; zoneHi = le; }
            else zoneLo = le;
        }
        if (zoneHi > zoneLo) doc.setSelection(zoneLo, zoneHi);
    } else if (hovered && !readOnly && ImGui::IsMouseClicked(0)) {
        dragStartX_ = mouse.x;
        dragMoved_ = false;
        if (envHit >= 0) {
            drag_ = EnvPoint;
            dragEnvIdx_ = envHit;
            doc.pushEnvUndo("Move envelope point");
        } else if (overLS) {
            drag_ = LoopStart;
            doc.setLoopPoints(ls, le, true); // snapshot for undo
        } else if (overLE) {
            drag_ = LoopEnd;
            doc.setLoopPoints(ls, le, true);
        } else {
            drag_ = Select;
            int64_t click = (int64_t)std::llround(xToFrame(mouse.x));
            click = std::min<int64_t>(std::max<int64_t>(0, click), n);
            // Edison-style selection: with an existing selection, dragging
            // past one end makes THAT end jump to the cursor. If the click
            // falls INSIDE the selection, the closer half determines which
            // end will move (the other stays anchored).
            if (doc.hasSelection()) {
                if (click > doc.selEnd()) {
                    dragAnchor_ = doc.selStart(); // the end jumps to the cursor
                } else if (click < doc.selStart()) {
                    dragAnchor_ = doc.selEnd(); // the start jumps to the cursor
                } else {
                    int64_t mid = doc.selStart() +
                                  (doc.selEnd() - doc.selStart()) / 2;
                    dragAnchor_ = (click < mid) ? doc.selEnd() // 1st half: moves the start
                                                : doc.selStart(); // 2nd half: moves the end
                }
            } else {
                dragAnchor_ = click;
            }
        }
    }

    if (hovered && !readOnly && ImGui::IsMouseClicked(1)) {
        if (envSel) {
            if (envHit >= 0) {
                doc.pushEnvUndo("Delete envelope point");
                envSel->removePoint(envHit);
            } else {
                double f = xToFrame(mouse.x);
                float t = (rectMax.y - mouse.y) / waveH;
                float v = envSel->minValue +
                          t * (envSel->maxValue - envSel->minValue);
                doc.pushEnvUndo("Envelope point");
                // The first point is always born with an anchor at the start
                // of the sample at the neutral value, interpolating toward
                // the new one.
                if (envSel->pts.empty() && f > 0.5)
                    envSel->addPoint(0.0, envSel->defaultValue);
                dragEnvIdx_ = envSel->addPoint(f, envSnap(v, -1));
                drag_ = EnvPoint;
                dragStartX_ = mouse.x;
                dragMoved_ = false;
            }
        }
    }

    if ((drag_ != None && ImGui::IsMouseDown(0)) ||
        (drag_ == EnvPoint && ImGui::IsMouseDown(1))) {
        double mf = xToFrame(mouse.x);
        if (!dragMoved_ && std::fabs(mouse.x - dragStartX_) >= 3.f)
            dragMoved_ = true;
        switch (drag_) {
            case Select: {
                if (!dragMoved_) break; // still a click, not a drag yet
                int64_t cur = (int64_t)std::llround(mf);
                cur = std::min<int64_t>(std::max<int64_t>(0, cur), n);
                doc.setSelection(std::min(dragAnchor_, cur),
                                 std::max(dragAnchor_, cur));
                break;
            }
            case LoopStart: {
                int64_t v = (int64_t)std::llround(mf);
                // start never reaches end: stops 1 sample short
                v = std::min(v, doc.loopEnd() - 1);
                v = std::max<int64_t>(0, v);
                doc.setLoopPoints(v, doc.loopEnd(), false);
                break;
            }
            case LoopEnd: {
                int64_t v = (int64_t)std::llround(mf);
                v = std::max(v, doc.loopStart() + 1);
                v = std::min(v, n);
                doc.setLoopPoints(doc.loopStart(), v, false);
                break;
            }
            case EnvPoint:
                if (envSel && dragEnvIdx_ >= 0) {
                    float t = (rectMax.y - mouse.y) / waveH;
                    float v = envSel->minValue +
                              t * (envSel->maxValue - envSel->minValue);
                    envSel->movePoint(dragEnvIdx_, mf, envSnap(v, dragEnvIdx_), (double)n);
                    if (envMode == 4)
                        ImGui::SetTooltip("%.0f cents", envSel->pts[dragEnvIdx_].value);
                    else
                        ImGui::SetTooltip("%.3f", envSel->pts[dragEnvIdx_].value);
                }
                break;
            default: break;
        }
    }

    if (drag_ != None && !ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1)) {
        // Plain click (no drag): only clears the selection. Doesn't place
        // any playback start point.
        if (drag_ == Select && !readOnly && !dragMoved_)
            doc.clearSelection();
        drag_ = None;
        dragEnvIdx_ = -1;
        dragMoved_ = false;
    }

    // ---------------- drawing ----------------
    dl->PushClipRect(rectMin, rectMax, true);

    // shaded loop region
    if (hasLoop) {
        float x0 = frameToX((double)ls), x1 = frameToX((double)le);
        dl->AddRectFilled(ImVec2(x0, rectMin.y), ImVec2(x1, rectMax.y),
                          kColLoopFill);
    }
    // selection (doesn't apply in preview: it belongs to the real document)
    if (!readOnly && doc.hasSelection()) {
        float x0 = frameToX((double)doc.selStart());
        float x1 = frameToX((double)doc.selEnd());
        dl->AddRectFilled(ImVec2(x0, rectMin.y), ImVec2(x1, rectMax.y), kColSel);
        dl->AddLine(ImVec2(x0, rectMin.y), ImVec2(x0, rectMax.y), kColSelBorder);
        dl->AddLine(ImVec2(x1, rectMin.y), ImVec2(x1, rectMax.y), kColSelBorder);
    }

    // per-channel waveforms
    const float laneH = waveH / (float)nch;
    for (int c = 0; c < nch; ++c) {
        float cy = rectMin.y + laneH * ((float)c + 0.5f);
        float half = laneH * 0.48f;
        dl->AddLine(ImVec2(rectMin.x, cy), ImVec2(rectMax.x, cy), kColCenter);

        if (spp_ <= 1.5) {
            // high zoom: sample-by-sample polyline
            float prevX = 0, prevY = 0;
            bool first = true;
            int64_t f0 = (int64_t)viewStart_;
            int64_t f1 = std::min<int64_t>(n, (int64_t)(viewStart_ + spp_ * width) + 2);
            for (int64_t i = std::max<int64_t>(0, f0 - 1); i < f1; ++i) {
                float x = frameToX((double)i);
                float y = cy - d.ch[c][(size_t)i] * half;
                if (!first) dl->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), kColWave);
                if (spp_ < 0.15f)
                    dl->AddCircleFilled(ImVec2(x, y), 2.5f, kColWave);
                prevX = x; prevY = y; first = false;
            }
        } else {
            // per-pixel min/max columns (using the peak pyramid when it pays off)
            const auto& pk = d.peaks[c];
            for (int px = 0; px < (int)width; ++px) {
                int64_t f0 = (int64_t)(viewStart_ + spp_ * px);
                int64_t f1 = (int64_t)(viewStart_ + spp_ * (px + 1));
                f0 = std::max<int64_t>(0, std::min(f0, n - 1));
                f1 = std::max(f0 + 1, std::min(f1, n));
                float mn = 1e9f, mx = -1e9f;
                if (spp_ >= AudioData::kPeakStep * 2) {
                    int64_t b0 = f0 / AudioData::kPeakStep;
                    int64_t b1 = std::max(b0 + 1,
                                          f1 / AudioData::kPeakStep);
                    b1 = std::min<int64_t>(b1, (int64_t)pk.mn.size());
                    for (int64_t b = b0; b < b1; ++b) {
                        mn = std::min(mn, pk.mn[(size_t)b]);
                        mx = std::max(mx, pk.mx[(size_t)b]);
                    }
                } else {
                    for (int64_t i = f0; i < f1; ++i) {
                        float s = d.ch[c][(size_t)i];
                        mn = std::min(mn, s);
                        mx = std::max(mx, s);
                    }
                }
                float x = rectMin.x + (float)px + 0.5f;
                dl->AddLine(ImVec2(x, cy - mx * half),
                            ImVec2(x, cy - mn * half + 1.f), kColWaveDim);
            }
        }
    }

    // loop markers
    if (hasLoop) {
        float x0 = frameToX((double)ls), x1 = frameToX((double)le);
        dl->AddLine(ImVec2(x0, rectMin.y), ImVec2(x0, rectMax.y), kColLoop, 2.f);
        dl->AddLine(ImVec2(x1, rectMin.y), ImVec2(x1, rectMax.y), kColLoop, 2.f);
        dl->AddRectFilled(ImVec2(x0, rectMin.y), ImVec2(x0 + 14, rectMin.y + 14), kColLoop);
        dl->AddRectFilled(ImVec2(x1 - 14, rectMin.y), ImVec2(x1, rectMin.y + 14), kColLoop);
        dl->AddText(ImVec2(x0 + 2, rectMin.y), IM_COL32(0, 0, 0, 255), "S");
        dl->AddText(ImVec2(x1 - 11, rectMin.y), IM_COL32(0, 0, 0, 255), "E");
        if (drag_ == LoopStart)
            ImGui::SetTooltip("Loop start: %lld", (long long)doc.loopStart());
        if (drag_ == LoopEnd)
            ImGui::SetTooltip("Loop end: %lld", (long long)doc.loopEnd());
    }

    // playhead: in MODULE preview the engine only plays back the published
    // buffer, so its position is remapped by adding the loop start. In
    // document preview (Filter: readOnly without previewActive) the
    // playhead is already in document frames -- drawn as-is.
    {
        float px = -1e9f;
        if (readOnly && eng.previewActive())
            px = frameToX((double)ovLoopStart + eng.playheadFrame());
        else if (!eng.previewActive())
            px = frameToX(eng.playheadFrame());
        if (px >= rectMin.x && px <= rectMax.x)
            dl->AddLine(ImVec2(px, rectMin.y), ImVec2(px, rectMax.y),
                        kColPlayhead, 1.5f);
    }

    // envelope overlay
    if (envSel) {
        const int stepPx = 3;
        float prevX = 0, prevY = 0;
        bool first = true;
        for (int px = 0; px <= (int)width; px += stepPx) {
            double f = viewStart_ + spp_ * px;
            float v = envSel->valueAt(f);
            float t = (v - envSel->minValue) / (envSel->maxValue - envSel->minValue);
            float x = rectMin.x + (float)px;
            float y = rectMax.y - t * waveH;
            if (!first) dl->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), kColEnv, 2.f);
            prevX = x; prevY = y; first = false;
        }
        for (auto& p : envSel->pts) {
            float t = (p.value - envSel->minValue) /
                      (envSel->maxValue - envSel->minValue);
            dl->AddCircleFilled(ImVec2(frameToX(p.frame), rectMax.y - t * waveH),
                                4.f, kColEnvPt);
            dl->AddCircle(ImVec2(frameToX(p.frame), rectMax.y - t * waveH),
                          4.f, kColEnv);
        }
    }

    dl->PopClipRect();

    // ---------------- scrollbar ----------------
    ImVec2 sbMin(origin.x, rectMax.y + 1);
    ImVec2 sbMax(origin.x + width, rectMax.y + sbH);
    dl->AddRectFilled(sbMin, sbMax, IM_COL32(32, 34, 38, 255));
    double total = (double)n;
    double visFrac = std::min(1.0, spp_ * width / std::max(1.0, total));
    double posFrac = viewStart_ / std::max(1.0, total);
    float thumbX0 = sbMin.x + (float)(posFrac * width);
    float thumbW = std::max(24.f, (float)(visFrac * width));
    ImGui::SetCursorScreenPos(sbMin);
    ImGui::InvisibleButton("##wavescroll", ImVec2(width, sbH));
    bool sbHovered = ImGui::IsItemHovered();
    bool sbActive = ImGui::IsItemActive();
    // absolute thumb drag; clicking outside the thumb brings it to the cursor
    if (ImGui::IsItemActivated()) {
        if (mouse.x >= thumbX0 && mouse.x <= thumbX0 + thumbW)
            sbGrab_ = mouse.x - thumbX0;
        else
            sbGrab_ = thumbW * 0.5f;
    }
    if (sbActive && visFrac < 1.0) {
        float newThumbX = mouse.x - sbGrab_ - sbMin.x;
        viewStart_ = (double)newThumbX / (double)width * total;
        clampView(n, width);
        posFrac = viewStart_ / std::max(1.0, total);
        thumbX0 = sbMin.x + (float)(posFrac * width);
    }
    dl->AddRectFilled(ImVec2(thumbX0, sbMin.y + 2),
                      ImVec2(std::min(sbMax.x, thumbX0 + thumbW), sbMax.y - 2),
                      (sbActive || sbHovered) ? IM_COL32(130, 138, 150, 255)
                                              : IM_COL32(90, 95, 105, 255),
                      4.f);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, sbMax.y + 2));
}
