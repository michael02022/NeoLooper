#include "alt_loopers.hpp"

#include <algorithm>
#include <cmath>

#include "imgui.h"

namespace {
// blended attack + pure loop, in that order: this is the content used by
// both the preview (with previewLoopOffset()==xfadeN) and Apply.
std::vector<std::vector<float>> assemble(const altloop::Result& res) {
    std::vector<std::vector<float>> out(res.ch.size());
    for (size_t c = 0; c < res.ch.size(); ++c) out[c] = specloop::assemble(res.ch[c]);
    return out;
}
} // namespace

// ------------------------------------------------------------------ common

bool AltLoopModule::drawParams() {
    bool changed = false;
    // Freshly opened (dirty_ is only true on the first drawParams() after
    // open()): calibrates tuning to the document's, sign inverted.
    if (dirty_) tuneCents_ = std::clamp(-docTuneCents_, -99.f, 99.f);
    const double sr = src_->sampleRate;
    const auto& r = lastResult_;

    if (usesPitch()) {
        if (docRootNote_ != lastRootNote_) changed = true;
        lastRootNote_ = docRootNote_;
        changed |= ImGui::SliderFloat("Tune", &tuneCents_, -99.f, 99.f, "%.1f cents",
                                      ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip(
            "The loop is built from whole cycles of the document's root note; "
            "this fine-tunes that pitch. Starts calibrated to the document's "
            "tuning (sign inverted: the content is mistuned the opposite way "
            "from the correction that parameter applies).");
    }

    ImGui::Separator();
    changed |= drawMethodParams();
    if (usesSeed()) {
        changed |= ImGui::SliderInt("Seed", &seed_, -1, kMaxSeed, "%d",
                                    ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip(
            "-1 = new random phases on every render; any other value = "
            "reproducible. Middle-click to type the exact value.");
    }

    changed |= ImGui::SliderFloat("Loop gain", &loopGainDb_, -24.f, 24.f, "%+.1f dB",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "Level of the loop, set by you: dB over the automatic level (0 dB = "
        "the loop region's RMS). Only the loop changes -- the real attack "
        "keeps its level and the crossfade blends into the new one. "
        "Middle-click to type the exact value.");

    ImGui::Separator();
    changed |= ImGui::SliderFloat("Crossfade", &xfadeMs_, 0.f, 500.f, "%.1f ms",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Middle-click to type the exact value");
    changed |= ImGui::SliderFloat("Curve", &xfadeCurve_, 0.f, 1.f,
                                  "equal gain < %.2f > equal power",
                                  ImGuiSliderFlags_AlwaysClamp);

    // Info from the last render() (at most one GUI frame out of date).
    if (!lastOk_) {
        if (!r.error.empty())
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "%s", r.error.c_str());
    } else {
        if (usesPitch())
            ImGui::Text("Note %d: %.2f Hz, cycle of %.2f samples", docRootNote_,
                        sr / r.period, r.period);
        if (r.loopCycles > 0)
            ImGui::Text("Loop: %lld cycles, %lld samples (%.1f ms, %.2f Hz actual)",
                        (long long)r.loopCycles, (long long)r.nLoopSamples,
                        1000.0 * (double)r.nLoopSamples / sr,
                        sr * (double)r.loopCycles / (double)r.nLoopSamples);
        else
            ImGui::Text("Loop: %lld samples (%.1f ms), the region's own length",
                        (long long)r.nLoopSamples, 1000.0 * (double)r.nLoopSamples / sr);
        if (r.loopCycles > 0 && r.xfadeN > r.nLoopSamples)
            ImGui::Text("Entry crossfade: %lld samples (%.1f ms, %.1f cycles generated)",
                        (long long)r.xfadeN, 1000.0 * (double)r.xfadeN / sr,
                        (double)r.xfadeN * (double)r.loopCycles / (double)r.nLoopSamples);
        else
            ImGui::Text("Entry crossfade: %lld samples (%.1f ms)", (long long)r.xfadeN,
                        1000.0 * (double)r.xfadeN / sr);
        if (r.xfadeN < r.xfadeNRequested)
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                               "Limited: not enough audio before the loop start");
        if (r.loopPeak > 0.f)
            ImGui::Text("Loop peak: %+.1f dBFS", 20.0 * std::log10((double)r.loopPeak));
        if (r.peak > 1.f)
            ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                               "Peak %+.1f dBFS: lower Loop gain (or normalize) "
                               "before exporting to 16/24-bit",
                               20.0 * std::log10((double)r.peak));
    }
    ImGui::TextDisabled(
        "Replaces the marked loop with the rebuilt one; the crossfade blends "
        "the real attack (before the loop) into its start, already audible "
        "in the preview. Printed into the document when you press Apply.");
    return changed;
}

std::shared_ptr<AudioData> AltLoopModule::render() {
    auto d = std::make_shared<AudioData>();
    d->sampleRate = src_->sampleRate;
    d->format = SampleFormat::Float32;

    altloop::Params p;
    p.method = method();
    p.rootNote = docRootNote_;
    p.tuneCents = tuneCents_;
    p.seed = seed_;
    p.xfadeMs = xfadeMs_;
    p.xfadeCurve = xfadeCurve_;
    p.loopGainDb = loopGainDb_;
    fillParams(p);

    lastOk_ = altloop::build(*src_, ls_, le_, p, lastResult_);
    if (!lastOk_) {
        lastXfadeN_ = 0;
        d->ch.resize(src_->channels());
        return d;
    }
    lastXfadeN_ = lastResult_.xfadeN;
    d->ch = assemble(lastResult_);
    return d;
}

void AltLoopModule::apply(Document& doc) {
    if (!lastOk_) return;
    doc.replaceRange(ls_ - lastResult_.xfadeN, le_, assemble(lastResult_), ls_,
                     ls_ + lastResult_.nLoopSamples, name());
}

// --------------------------------------------------------- Ensemble

bool EnsembleLoop::drawMethodParams() {
    bool changed = false;
    static const char* kShapes[] = {"Gaussian", "Flat", "Voices"};
    changed |= ImGui::Combo("Shape", &shape_, kShapes, 3);
    ImGui::SetItemTooltip(
        "How each harmonic's energy is spread around it. Gaussian: a soft "
        "bell (the original Synthesis). Flat: an even band of the same "
        "width, a denser, flatter chorus. Voices: N discrete copies "
        "detuned in cents, a true unison (supersaw-like).");
    // Same two bands (Low/High frequency, crossfaded in log frequency) for
    // every shape: the width for Gaussian/Flat, the detune for Voices --
    // or, in the 1-band variant, one value for the whole spectrum.
    const bool voices = shape_ == (int)altloop::HarmonicShape::Voices;
    if (voices)
        changed |= ImGui::SliderInt("Voices", &voices_, 2, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
    if (oneBand_) {
        if (voices) {
            changed |= ImGui::SliderFloat("Detune", &detune_, 0.f, 100.f, "+-%.1f cents",
                                          ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip(
                "Spread of the voices over the whole spectrum, evenly from "
                "-detune to +detune cents (like real detuned oscillators, the "
                "offset in Hz grows with the harmonic). Detune finer than the "
                "loop can resolve (one FFT bin = pitch / cycles in the loop) "
                "blends into the center: longer loops allow subtler detune. "
                "Middle-click to type the exact value.");
        } else {
            changed |= ImGui::SliderFloat("Width", &width_, 0.f, 1000.f, "%.1f",
                                          ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip(
                "Bandwidth of every harmonic, in thousandths of the harmonic "
                "spacing: 0 = pure harmonics, higher = a wider, chorused/ensemble "
                "sustain. The same over the whole spectrum (Ensemble has a low "
                "and a high band). Middle-click to type the exact value.");
        }
    } else {
        changed |= ImGui::SliderFloat("Low frequency", &lowFreq_, 20.f, 20000.f, "%.0f Hz",
                                      ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        if (voices) {
            changed |= ImGui::SliderFloat("Low detune", &lowDetune_, 0.f, 100.f, "+-%.1f cents",
                                          ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip(
                "Spread of the voices at and below Low frequency, evenly from "
                "-detune to +detune cents (like real detuned oscillators, the "
                "offset in Hz grows with the harmonic). Crossfaded (log frequency) "
                "up to High detune. Detune finer than the loop can resolve (one "
                "FFT bin = pitch / cycles in the loop) blends into the center: "
                "longer loops allow subtler detune. Middle-click to type the exact "
                "value.");
        } else {
            changed |= ImGui::SliderFloat("Low width", &lowWidth_, 0.f, 1000.f, "%.1f",
                                          ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip(
                "Bandwidth of each harmonic at and below Low frequency, in "
                "thousandths of the harmonic spacing: 0 = pure harmonics, higher = "
                "a wider, chorused/ensemble sustain. Interpolated (log frequency) "
                "up to High width.");
        }
        changed |= ImGui::SliderFloat("High frequency", &highFreq_, 20.f, 20000.f, "%.0f Hz",
                                      ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        if (voices) {
            changed |= ImGui::SliderFloat("High detune", &highDetune_, 0.f, 100.f, "+-%.1f cents",
                                          ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Spread of the voices at and above High frequency.");
        } else {
            changed |= ImGui::SliderFloat("High width", &highWidth_, 0.f, 1000.f, "%.1f",
                                          ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip("Bandwidth of each harmonic at and above High frequency.");
        }
        static const char* kBandCurves[] = {"Curve", "Bipolar"};
        changed |= ImGui::Combo("Band crossfade", &bandCurve_, kBandCurves, 2);
        ImGui::SetItemTooltip(
            "Shape of the crossfade between the low and the high band (see the "
            "curve below). Curve: one bend -- right = convex (it changes soon "
            "after Low frequency), left = concave/exponential (it changes late, "
            "near High frequency). Bipolar: an S -- right = gentle near both "
            "bands and steep in the middle, left = steep near both bands with a "
            "plateau halfway. Bend 0 = linear (a straight line on the octave axis).");
        changed |= ImGui::SliderFloat("Bend", &bandBend_, -1.f, 1.f, "%+.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Curvature of the band crossfade, 0 = linear. Middle-click to type "
                              "the exact value.");
        drawBandCurve();
    }
    changed |= ImGui::SliderFloat("Inharmonicity", &inharmonicity_, 0.f, 0.01f,
                                  inharmonicity_ > 0.f ? "%.5f" : "off",
                                  ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip(
        "Stretches the upper harmonics like a stiff string: harmonic h moves "
        "to h*f0*sqrt(1+B*h^2) (the fundamental stays in tune). ~0.0001-0.001 "
        "= piano-like, more = bell/metallic. Middle-click to type the exact "
        "value.");
    changed |= ImGui::SliderFloat("Noise floor", &floorPct_, 0.f, 200.f, "%.0f%%",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "The sample's own noise between the harmonics (breath, bow, hiss), "
        "with its real color: 100%% = as loud as in the sample, 0%% = pure "
        "harmonics. The harmonics never include it, so it's only heard "
        "through this control.");
    ImGui::TextDisabled("Needs at least 10 cycles in the loop.");
    return changed;
}

void EnsembleLoop::drawBandCurve() const {
    const double sr = src_ ? src_->sampleRate : 44100.0;
    const double fMin = 20.0, fMax = std::min(20000.0, sr / 2.0);
    const bool voices = shape_ == (int)altloop::HarmonicShape::Voices;
    const float w1 = voices ? lowDetune_ : lowWidth_, w2 = voices ? highDetune_ : highWidth_;
    // Vertical scale: a round ceiling over the larger value (at least 10).
    const double peak = std::max({(double)w1, (double)w2, 10.0}) * 1.15;
    const double mag = std::pow(10.0, std::floor(std::log10(peak)));
    const double yMax = std::ceil(peak / (mag / 2.0)) * (mag / 2.0);

    const float lineH = ImGui::GetTextLineHeight();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 7.f * lineH);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##ensemble_width_curve", size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float left = origin.x + 3.2f * lineH, right = origin.x + size.x - 4.f;
    const float top = origin.y + 1.3f * lineH, bottom = origin.y + size.y - 1.3f * lineH;
    auto xOf = [&](double f) {
        const double t = std::log(std::clamp(f, fMin, fMax) / fMin) / std::log(fMax / fMin);
        return left + (float)t * (right - left);
    };
    auto yOf = [&](double w) { return bottom - (float)(w / yMax) * (bottom - top); };
    auto fOf = [&](float x) {
        return fMin * std::pow(fMax / fMin, (double)std::clamp((x - left) / (right - left), 0.f, 1.f));
    };
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 gridCol = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 lowCol = ImGui::GetColorU32(ImVec4(0.30f, 0.55f, 0.95f, 0.22f));
    const ImU32 highCol = ImGui::GetColorU32(ImVec4(0.95f, 0.60f, 0.25f, 0.22f));
    const ImU32 curveCol = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    const ImU32 tickCol = ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.55f));

    dl->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), ImGui::GetColorU32(ImGuiCol_FrameBg));

    // Bands: low (up to Low frequency), crossfade (a gradient between the
    // two colors), high (from High frequency up). High <= Low: a step.
    const float x1 = xOf(lowFreq_), x2 = xOf(std::max(lowFreq_, highFreq_));
    const float xStep = xOf(highFreq_);
    if (highFreq_ > lowFreq_) {
        dl->AddRectFilled(ImVec2(left, top), ImVec2(x1, bottom), lowCol);
        dl->AddRectFilledMultiColor(ImVec2(x1, top), ImVec2(x2, bottom), lowCol, highCol, highCol, lowCol);
        dl->AddRectFilled(ImVec2(x2, top), ImVec2(right, bottom), highCol);
    } else {
        dl->AddRectFilled(ImVec2(left, top), ImVec2(xStep, bottom), lowCol);
        dl->AddRectFilled(ImVec2(xStep, top), ImVec2(right, bottom), highCol);
    }
    auto centered = [&](float a, float b, float y, ImU32 col, const char* label) {
        const float tw = ImGui::CalcTextSize(label).x;
        if (b - a > tw + 6.f) dl->AddText(ImVec2(0.5f * (a + b) - 0.5f * tw, y), col, label);
    };
    const float labelY = top + 0.2f * lineH;
    if (highFreq_ > lowFreq_) {
        centered(left, x1, labelY, textCol, "Low band");
        centered(x1, x2, labelY, textCol, "Crossfade");
        centered(x2, right, labelY, textCol, "High band");
    } else {
        centered(left, xStep, labelY, textCol, "Low band");
        centered(xStep, right, labelY, textCol, "High band");
    }

    // Octave grid with decade labels.
    for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        if (f >= fMax) break;
        const float x = xOf(f);
        dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), (f == 100.0 || f == 1000.0 || f == 10000.0)
                                                           ? gridCol
                                                           : (gridCol & 0x60FFFFFF));
        if (f == 100.0 || f == 1000.0 || f == 10000.0) {
            const char* lbl = f == 100.0 ? "100" : f == 1000.0 ? "1k" : "10k";
            dl->AddText(ImVec2(x - 0.5f * ImGui::CalcTextSize(lbl).x, bottom + 0.15f * lineH), dimCol, lbl);
        }
    }
    dl->AddText(ImVec2(right - ImGui::CalcTextSize("Hz").x, bottom + 0.15f * lineH), dimCol, "Hz");

    // Low/High frequency markers with their values above the plot.
    char buf[48];
    auto marker = [&](float x, double f, const char* name, bool alignRight) {
        dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), textCol, 1.5f);
        if (f >= 1000.0) snprintf(buf, sizeof buf, "%s %.1fk", name, f / 1000.0);
        else snprintf(buf, sizeof buf, "%s %.0f", name, f);
        const float tw = ImGui::CalcTextSize(buf).x;
        const float tx = std::clamp(alignRight ? x - tw - 2.f : x + 2.f, left, right - tw);
        dl->AddText(ImVec2(tx, origin.y), textCol, buf);
    };
    // The lower of the two gets its label on the left of its line.
    marker(xOf(lowFreq_), lowFreq_, "Low", lowFreq_ <= highFreq_);
    marker(xOf(highFreq_), highFreq_, "High", highFreq_ < lowFreq_);

    // Width axis.
    snprintf(buf, sizeof buf, "%.0f", yMax);
    dl->AddText(ImVec2(left - ImGui::CalcTextSize(buf).x - 4.f, top - 0.5f * lineH), dimCol, buf);
    dl->AddText(ImVec2(left - ImGui::CalcTextSize("0").x - 4.f, bottom - 0.5f * lineH), dimCol, "0");
    dl->AddText(ImVec2(origin.x, 0.5f * (top + bottom) - 0.5f * lineH), dimCol,
                voices ? "cents" : "width");

    // The loop's harmonics (with inharmonicity) as ticks along the bottom.
    const double period = (lastOk_ && lastResult_.period > 0.0)
                              ? lastResult_.period
                              : altloop::notePeriod(sr, docRootNote_, tuneCents_);
    const double f0 = sr / period, B = std::max(0.0, (double)inharmonicity_);
    for (int h = 1; h <= 512; ++h) {
        const double f = h * f0 * std::sqrt((1.0 + B * h * h) / (1.0 + B));
        if (f >= fMax) break;
        if (f < fMin) continue;
        const float x = xOf(f);
        dl->AddLine(ImVec2(x, bottom), ImVec2(x, bottom - 0.7f * lineH), tickCol);
    }

    // The band curve itself, sampled from the DSP's function.
    constexpr int kPts = 256;
    ImVec2 pts[kPts];
    for (int i = 0; i < kPts; ++i) {
        const float x = left + (right - left) * (float)i / (float)(kPts - 1);
        pts[i] = ImVec2(x, yOf(altloop::bandValue(fOf(x), lowFreq_, w1, highFreq_, w2,
                                                  (altloop::BandCurve)bandCurve_, bandBend_)));
    }
    dl->AddPolyline(pts, kPts, curveCol, 2.f);

    if (hovered) {
        const float mx = ImGui::GetIO().MousePos.x;
        if (mx >= left && mx <= right) {
            const double f = fOf(mx);
            const double w = altloop::bandValue(f, lowFreq_, w1, highFreq_, w2,
                                                (altloop::BandCurve)bandCurve_, bandBend_);
            dl->AddLine(ImVec2(mx, top), ImVec2(mx, bottom), dimCol);
            dl->AddCircleFilled(ImVec2(mx, yOf(w)), 3.5f, curveCol);
            ImGui::SetTooltip(voices ? "%.0f Hz: detune +-%.1f cents (%s)" : "%.0f Hz: width %.1f (%s)",
                              f, w,
                              f <= lowFreq_ || highFreq_ <= lowFreq_
                                  ? (f >= highFreq_ && highFreq_ <= lowFreq_ ? "high band" : "low band")
                              : f >= highFreq_ ? "high band"
                                               : "crossfade");
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(voices ? "Detune of each harmonic's voices (ticks = the loop's "
                                "harmonics), on an octave axis. Hover to read it."
                              : "Width of each harmonic (ticks = the loop's harmonics), on an "
                                "octave axis. Hover to read it.");
    ImGui::PopStyleColor();
}

void EnsembleLoop::fillParams(altloop::Params& p) const {
    p.shape = (altloop::HarmonicShape)shape_;
    p.voices = voices_;
    if (oneBand_) {
        // Both bands equal: bandValue is that value at every frequency.
        p.lowWidth = p.highWidth = width_;
        p.lowDetuneCents = p.highDetuneCents = detune_;
    } else {
        p.lowFreq = lowFreq_;
        p.lowWidth = lowWidth_;
        p.highFreq = highFreq_;
        p.highWidth = highWidth_;
        p.bandCurve = (altloop::BandCurve)bandCurve_;
        p.bandBend = bandBend_;
        p.lowDetuneCents = lowDetune_;
        p.highDetuneCents = highDetune_;
    }
    p.inharmonicity = inharmonicity_;
    p.floorPct = floorPct_;
}

// ------------------------------------------------------------ Random Phase

bool RandomPhaseLoop::drawMethodParams() {
    bool changed = false;
    changed |= ImGui::SliderFloat("Window", &windowPct_, 0.f, 100.f, "%.0f%%",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "Analysis window taper (Tukey): 0%% = rectangular, 100%% = Hann. "
        "Scaled by Amount (at low amounts the region's time structure "
        "survives, and a taper would be heard as a fade in the loop).");
    changed |= ImGui::Checkbox("Integer cycles", &integerCycles_);
    ImGui::SetItemTooltip(
        "On: the loop is a whole number of cycles of the pitch (cleaner "
        "harmonics). Off: the loop keeps the marked region's exact length "
        "and no pitch is needed (noise, ambiences, inharmonic sounds).");
    changed |= ImGui::SliderFloat("Amount", &randomPct_, 0.f, 100.f, "%.0f%%",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "How much of each frequency's phase is randomized. 100%% = fully "
        "random: a stationary, seamless texture. Lower keeps part of the "
        "region's own movement (attacks, vibrato) and smears it less; 0%% = "
        "the region as-is (its loop point may click).");
    const bool stereo = src_ && src_->channels() >= 2;
    ImGui::BeginDisabled(!stereo);
    changed |= ImGui::Checkbox("Stereo decorrelation", &decorrelate_);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        stereo ? "Independent random phases per channel instead of one shared "
                 "offset: wider stereo, and a mono source copied to both "
                 "channels becomes pseudo-stereo. Off keeps the original "
                 "stereo image."
               : "Needs a stereo sample.");
    changed |= ImGui::SliderFloat("Smoothing", &smoothHz_, 0.f, 500.f,
                                  smoothHz_ > 0.f ? "%.1f Hz" : "off",
                                  ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip(
        "Smooths the frozen spectrum over this bandwidth (like averaging "
        "several shorter FFTs): noise and ambiences lose the spiky, "
        "\"metallic\" color a single long analysis gives them. On pitched "
        "sounds keep it well below the fundamental, or the harmonics blur "
        "into noise. Middle-click to type the exact value.");
    return changed;
}

void RandomPhaseLoop::fillParams(altloop::Params& p) const {
    p.windowPct = windowPct_;
    p.integerCycles = integerCycles_;
    p.randomPct = randomPct_;
    p.decorrelate = decorrelate_ && src_ && src_->channels() >= 2;
    p.smoothHz = smoothHz_;
}
