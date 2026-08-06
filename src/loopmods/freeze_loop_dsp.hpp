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
    int64_t cyclePeriod = 0;
    std::vector<ChannelResult> ch;
};

// Cheap arithmetic (no FFT), used both by buildFreeze() and for immediate
// feedback in the GUI while dragging a slider. freezePos in [0,1] (0 = start
// of the sample, 1 = end). nLoopSamples here is the REQUESTED duration
// (before tuning); buildFreeze() adjusts it when resampling if tuneCents !=
// 0.
void sizes(int64_t frames, double sampleRate, double freezePos, float durationMs,
          float xfadeMs, int nFft, int64_t& freezeStart, int64_t& xfadeNRequested,
          int64_t& xfadeN, int64_t& nLoopSamples);

// Synthesis (specloop::synthesize, method=Naive) + manual tuning by
// resampling (tuneCents, -99..99) + MANUAL rotation (offsetFrac, no search)
// + entry blend (specloop::rotateAndBlend). false if there isn't enough
// audio around the freeze point for the chosen fft-size.
//
// cycleMode=true -- "exact cycle" mode, port of Infinity's Freeze Looper
// ("quantizes all the sound energy in the loop to a precise harmonic
// series... a waveform that is perfectly repeated in every cycle"): uses
// the phase vocoder (not Naive) with the quantization anchored to the
// note's harmonic series (MIDI rootNote, like Infinity's "MIDI Note
// Number", plus tuneCents as a fine adjustment of the target -- nothing is
// resampled here). The period is rounded to WHOLE samples (cyclePeriod =
// round(sr/f0); the real fundamental becomes sr/cyclePeriod, the classic
// tuning quantization of old samplers) and the body is built by folding the
// synthesis down to a single averaged cycle and tiling it: every cycle is
// bit-identical -- meant for minimal-memory loops (marking/keeping a SINGLE
// cycle is enough). With durationMs~0 the body is exactly one cycle.
// seamError reports the fold's maximum residual (how much non-harmonic
// "impurity" got averaged out), the wrap itself is exact by construction.
bool buildFreeze(const AudioData& src, double freezePos, float durationMs,
                 float offsetFrac, float xfadeMs, float xfadeCurve, float tuneCents,
                 int nFft, bool cycleMode, int rootNote, Result& out);

} // namespace freezedsp
