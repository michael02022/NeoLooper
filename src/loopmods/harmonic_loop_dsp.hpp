// DSP core of the Harmonic loop module, with no ImGui dependency (so it can
// be tested from core_test without dragging in the GUI). Port of
// proto/freeze_loop.py, subcommand "loop" (build_seamless_loop): see
// harmonic_loop.hpp for the technique's explanation. The spectral synthesis
// (phase vocoder) and the rotation+blend live in spectral_loop_core.hpp,
// shared with freeze_loop_dsp; this file only adds the automatic rotation
// search (circularBestShift) that's specific to Harmonic -- Freeze uses a
// manual offset instead.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "../audio_data.hpp"
#include "spectral_loop_core.hpp"

namespace harmonicdsp {

using ChannelResult = specloop::RotatedBlend; // attackTail / loopRegion

struct Result {
    int64_t xfadeNRequested = 0;
    int64_t xfadeN = 0;
    int64_t nLoopSamples = 0;
    double seamError = 0.0; // max |loopRegion[i] - "next period"[i]|, diagnostic
    int64_t notePeriod = 0; // 0 if noteMode==false; otherwise, samples per cycle of the note
    std::vector<ChannelResult> ch;
};

// Frequency (Hz) of a MIDI note + fine tuning in cents, standard A4=440Hz
// formula -- shared with Freeze's exact-cycle mode.
inline double noteFreq(int rootNote, float tuneCents) {
    return 440.0 * std::pow(2.0, ((double)rootNote - 69.0 + (double)tuneCents / 100.0) / 12.0);
}

// Cheap arithmetic (no FFT), used both by buildLoop() and for immediate
// feedback in the GUI while dragging a slider.
//
// noteMode=false (default): nLoopSamples is quantized to the hop grid
// (multiples of nFft/4), as always -- periodic by construction, unrelated
// to any musical note.
//
// noteMode=true: nLoopSamples is instead quantized to an integer number of
// cycles of the note (rootNote+tuneCents) -- period = round(sr/f0), and
// nLoopSamples = k*period with k = round((le-ls)/period). Unlike Freeze's
// exact-cycle mode, this does NOT fold down to a single cycle: the k full
// cycles are kept (see harmonic_loop.hpp), each with its natural
// overlap-add variation -- only the phase-quantization anchor changes, not
// the module's multi-cycle philosophy.
void sizes(int64_t ls, int64_t le, double sampleRate, int nFft, float xfadeMs,
          bool noteMode, int rootNote, float tuneCents, int64_t& xfadeNRequested,
          int64_t& xfadeN, int64_t& nLoopSamples, int64_t& notePeriod);

// Full phase vocoder (specloop::synthesize, with lockPeriod=notePeriod if
// noteMode) + rotation search via circular correlation
// (specloop::circularBestShift) + blend (specloop::rotateAndBlend). false if
// there isn't enough audio around ls for the chosen fft-size (nFft doesn't
// fit in src, or ls falls in the first/last frame).
bool buildLoop(const AudioData& src, int64_t ls, int64_t le, int nFft, float xfadeMs,
              float xfadeCurve, bool noteMode, int rootNote, float tuneCents, Result& out);

} // namespace harmonicdsp
