// Shared phase-vocoder core used by the Harmonic and Freeze loop modules
// (ported from proto/freeze_loop.py: build_seamless_loop for Harmonic,
// spectral_freeze for Freeze -- both lean on the same synthesis primitive).
// No ImGui dependency, so it can be tested from core_test without dragging
// in the GUI.
//
// synthesize() does the expensive, shared part (STFT of 2 frames + frequency
// quantization + overlap-add synthesis) and returns a pure period WITHOUT
// rotation (loopBuf); each caller decides where the rotation comes from:
// Harmonic searches for it automatically (circularBestShift, correlation
// with the real attack) and Freeze leaves it fully manual (an offset/phase
// slider, no search at all). rotateAndBlend() applies that rotation and
// blends the first xfadeN samples with a real reference, leaving the result
// ready to insert into the document -- identical logic for both modules,
// only where 'shift' comes from changes.
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace specloop {

// PhaseVocoder: estimates each bin's real frequency (phase difference
// between 2 frames) before quantizing -- coherent, no "buzz" (used by
// Harmonic and by build_seamless_loop in the reference script). Naive:
// estimates nothing, each bin's frequency is forced to 0 -- literally holds
// the analysis frame (magnitude and phase) with no advance at all, looping
// with period = hop instead of the source's real pitch (hence the
// characteristic "buzz"; port of method="naive" in spectral_freeze, used by
// Freeze).
enum class Method { PhaseVocoder, Naive };

struct CoreResult {
    std::vector<std::vector<double>> loopBuf; // per channel, length nLoopSamples, UNROTATED
    double seamError = 0.0; // max |loopBuf[i] - "next period"[i]|, diagnostic
};

// Quantizes each STFT bin (taken at anchorSample) to an integer number of
// cycles within nLoopSamples and synthesizes a pure period via overlap-add,
// with no rotation search or blending. false if there isn't enough audio
// around anchorSample for the chosen fft-size.
//
// lockPeriod > 0 (PhaseVocoder only; Freeze's "exact cycle" mode, Infinity's
// Freeze Looper style): instead of the full loop's grid (multiples of
// sr/nLoopSamples), each bin is quantized to the HARMONIC SERIES of a period
// of lockPeriod samples (multiples of sr/lockPeriod) -- all the energy ends
// up on exact harmonics of that fundamental, and the synthesized signal is
// periodic at lockPeriod (every cycle identical), not just over the whole
// loop. With lockPeriod, seamError is NOT meaningful (the synthesis loop
// isn't a multiple of the period; the caller folds/tiles it afterward).
bool synthesize(const AudioData& src, int64_t anchorSample, int64_t nLoopSamples,
                int nFft, Method method, CoreResult& out, int64_t lockPeriod = 0);

// Rotation (via FFT circular correlation) that best aligns loopBuf with
// 'reference' (typically the xfadeN samples right before the entry point).
// Used by Harmonic to automate the entry phase.
int64_t circularBestShift(const std::vector<double>& loopBuf,
                          const std::vector<double>& reference);

struct RotatedBlend {
    std::vector<float> attackTail; // xfadeN samples
    std::vector<float> loopRegion; // nLoopSamples samples, full rotated period
};

// loopBuf rotated by 'shift', with its first xfadeN samples blended with
// 'reference' (-> attackTail) and the rest reordered as a FULL rotation of
// an extra xfadeN samples (loopRegion[i] = rotated[(i+xfadeN)%n]) --
// guarantees a clean wrap at loopRegion's own boundary AND a continuous
// splice between attackTail and loopRegion (see the detailed comment in
// harmonic_loop.hpp: attackTail ends up blending into rotated[xfadeN-1],
// loopRegion starts at rotated[xfadeN], adjacent samples of the same
// periodic waveform).
void rotateAndBlend(const std::vector<double>& loopBuf, const std::vector<double>& reference,
                    int64_t shift, int64_t xfadeN, float xfadeCurve, RotatedBlend& out);

// Concatenates attackTail++loopRegion: this is the content that both the
// preview (looped with the xfadeN offset) and Apply insert.
inline std::vector<float> assemble(const RotatedBlend& rb) {
    std::vector<float> v;
    v.reserve(rb.attackTail.size() + rb.loopRegion.size());
    v.insert(v.end(), rb.attackTail.begin(), rb.attackTail.end());
    v.insert(v.end(), rb.loopRegion.begin(), rb.loopRegion.end());
    return v;
}

} // namespace specloop
