// DSP core of the Freeze loop module, with no ImGui dependency (so it can
// be tested from core_test without dragging in the GUI). Port of
// proto/freeze_loop.py, subcommand "freeze" (spectral_freeze), method=
// "naive" (explicit user request: literally holds the analysis frame, with
// no phase advance -- the "buzz" character, instead of the coherent phase
// vocoder Harmonic uses). Also simplified relative to the script: no
// automatic rotation search (the frozen chunk's offset/phase is a manual
// slider) and no exit crossfade -- in fact, no "exit" at all: the frozen
// chunk OVERWRITES to the end of the sample (see freeze_loop.hpp). Shares
// the spectral synthesis (specloop::synthesize) and the entry blend
// (specloop::rotateAndBlend) with Harmonic.
//
// Manual tuning (tuneCents): with method=Naive there's no per-bin frequency
// to shift before quantizing (they all stay at 0 by design, see
// specloop::Method) -- so, unlike a hypothetical "vocoder" tuning, here the
// ALREADY-SYNTHESIZED period (loopBuf) is resampled to the requested pitch
// ratio, same as the pitch-shift the engine already uses for root
// note/finetune. This changes the frozen chunk's real duration (shorter if
// pitched up); nLoopSamples in the result already reflects the FINAL,
// post-tuning duration.
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"
#include "spectral_loop_core.hpp"

namespace freezedsp {

using ChannelResult = specloop::RotatedBlend; // attackTail / loopRegion

struct Result {
    int64_t freezeStart = 0;      // freeze point, in absolute document samples
    int64_t xfadeNRequested = 0;
    int64_t xfadeN = 0;
    int64_t nLoopSamples = 0;     // FINAL duration of the frozen chunk (post-tuning)
    int64_t shift = 0;            // manual rotation actually applied
    double seamError = 0.0;       // diagnostic: continuity of the frozen chunk itself
    // Exact-cycle mode: period in WHOLE samples (0 in normal mode).
    // nLoopSamples is always an exact multiple of cyclePeriod and every
    // cyclePeriod-sample chunk of the body is bit-identical to the rest.
    // With Extras::fractionalCycles and loopCycles > 1, cyclePeriod is the
    // length of the k-cycle loop instead (still the span marked on Apply).
    int64_t cyclePeriod = 0;
    // Cycles inside cyclePeriod: 1 = one whole-sample cycle (original);
    // k > 1 = Extras::fractionalCycles, k cycles in round(k*P) samples.
    int64_t loopCycles = 0;
    // averageMs > 0 (exact-cycle mode): whole cycles of the section that
    // were averaged into the cycle (0 = single-frame analysis).
    int64_t averagedCycles = 0;
    float loopPeak = 0.f; // peak of the frozen chunk (loopRegion), after loopGainDb
    std::vector<ChannelResult> ch;
};

// Optional settings of buildFreeze(); the defaults give the plain
// single-frame freeze.
struct Extras {
    // Exact-cycle mode: rebuild the cycle from the averageMs-long SECTION
    // after the freeze point (altloop's HarmonicFreeze, rebuildLoop())
    // instead of one STFT frame -- the section's average timbre, with the
    // phase the audio has at the freeze point. 0 = single frame.
    float averageMs = 0.f;
    // Add, under the manual offset, the rotation that best correlates with
    // the attack right before the freeze point (shared by every channel).
    bool autoPhase = false;
    // Level of the frozen chunk, dB over the synthesis' own level (the real
    // attack is untouched; the crossfade blends into the new level).
    float loopGainDb = 0.f;
    // Entry crossfade only limited by the audio before the freeze point,
    // not by the chunk: the (periodic) chunk is repeated under the attack as
    // many times as the fade needs and keeps its own length.
    bool extendXfade = false;
    // Exact-cycle mode: instead of whole-sample cycles tiled to Duration,
    // the loop is k = round(Duration/P) cycles in round(k*P) samples -- a
    // fractional period: the rounding error (up to
    // ~866/P cents with one cycle) is spread over the k cycles. The cycle
    // is re-laid harmonic by harmonic (bin h -> h*k of the longer frame, no
    // interpolation). Duration of about one cycle or less -> one
    // whole-sample cycle, as before.
    bool fractionalCycles = false;
    // Exact-cycle mode: number of cycles in the loop, set directly (> 0
    // overrides durationMs, which then only sizes the analysis); 0 = derive
    // it from durationMs.
    int64_t loopCycles = 0;
};

// Cheap arithmetic (no FFT), used both by buildFreeze() and for immediate
// feedback in the GUI while dragging a slider. freezePos in [0,1] (0 = start
// of the sample, 1 = end). nLoopSamples here is the REQUESTED duration
// (before tuning); buildFreeze() adjusts it when resampling if tuneCents !=
// 0.
void sizes(int64_t frames, double sampleRate, double freezePos, float durationMs,
          float xfadeMs, int nFft, int64_t& freezeStart, int64_t& xfadeNRequested,
          int64_t& xfadeN, int64_t& nLoopSamples, bool extendXfade = false);

// Synthesis (specloop::synthesize, method=Naive) + manual tuning by
// resampling (tuneCents, -99..99) + MANUAL rotation (offsetFrac, no search)
// + entry blend (specloop::rotateAndBlend). false if there isn't enough
// audio around the freeze point for the chosen fft-size.
//
// cycleMode=true -- "exact cycle" mode: all the sound energy is quantized
// to a precise harmonic series, a waveform perfectly repeated in every
// cycle. Uses the phase vocoder (not Naive) with the quantization anchored
// to the note's harmonic series (MIDI rootNote, plus tuneCents as a fine
// adjustment of the target -- nothing is resampled here). The period is rounded to WHOLE samples (cyclePeriod =
// round(sr/f0); the real fundamental becomes sr/cyclePeriod, the classic
// tuning quantization of old samplers) and the body is built by folding the
// synthesis down to a single averaged cycle and tiling it: every cycle is
// bit-identical -- meant for minimal-memory loops (marking/keeping a SINGLE
// cycle is enough). With durationMs~0 the body is exactly one cycle.
// seamError reports the fold's maximum residual (how much non-harmonic
// "impurity" got averaged out), the wrap itself is exact by construction.
//
// x: optional settings (see Extras).
bool buildFreeze(const AudioData& src, double freezePos, float durationMs,
                 float offsetFrac, float xfadeMs, float xfadeCurve, float tuneCents,
                 int nFft, bool cycleMode, int rootNote, Result& out,
                 const Extras& x = Extras());

} // namespace freezedsp
