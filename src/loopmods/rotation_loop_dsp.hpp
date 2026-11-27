// DSP core of the Rotation loop module, with no ImGui dependency (so it can
// be tested from core_test without dragging in the GUI). Port of
// proto/rotation_loop.py, reworked so the result is
// seamless and its level predictable. Rotation doesn't synthesize anything:
// it sums nLayers circularly rotated copies of the marked loop [ls, le)
// (offsets[0]==0 = unrotated reference layer, the rest random according to
// 'seed': -1 = fresh entropy on every call, any other value = reproducible;
// the same offsets on every channel). In order:
//
//   1. Classic Crossfade (Inside) FIRST (see crossfade_inside_core.hpp): the
//      region's tail is blended toward its head and the head excluded, so
//      the loop being rotated -- nLoopSamples - ccAvail samples -- wraps
//      seamlessly. A rotation of a seamless loop is seamless and so is any
//      sum of them: no further blend is needed at the end. (Rotating a loop
//      that clicks at its wrap would instead spread that click into one
//      smaller click per layer, inside the loop; with ccPct = 0 the marked
//      loop must already be seamless.)
//   2. The layers are summed without their DC; the sum is scaled so each
//      channel has EXACTLY the energy of its own (seamless) loop -- the
//      level and the stereo balance don't depend on the seed or the layer
//      count -- and the loop's original DC is added back (summing would
//      otherwise multiply it).
//   3. Splice with the real attack (audio before ls): the rotation of the
//      combined loop that best correlates with it (one shared by all
//      channels, or one per channel with alignPerChannel), and 'xfadeMs' of
//      the attack blended into it; the combined loop repeats under that
//      blend and under the excluded head as many times as needed.
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"
#include "spectral_loop_core.hpp"

namespace rotationdsp {

struct Result {
    int64_t xfadeNRequested = 0;
    int64_t xfadeN = 0;       // real attack -> rotated loop crossfade (harmonic-style)
    int64_t nLoopSamples = 0; // period = le - ls (Rotation doesn't change the loop's duration)
    int64_t ccAvail = 0;      // Classic Crossfade Inside blend that makes the loop seamless
    std::vector<int64_t> offsets;    // rotation offset of each layer (offsets[0]==0)
    float peak = 0.f;                // peak of the final loop (loopBlended), all channels

    // Per channel, in the order they're inserted into the document/preview:
    // attackTail (xfadeN) ++ ccHead (ccAvail, plays once as attack) ++
    // loopBlended (nLoopSamples - ccAvail, the final loop that repeats,
    // seamless by construction).
    struct Channel {
        std::vector<float> attackTail;
        std::vector<float> ccHead;
        std::vector<float> loopBlended;
    };
    std::vector<Channel> ch;
};

// Cheap arithmetic (no FFT or RNG), used both by buildRotationLoop() and for
// immediate feedback in the GUI while dragging a slider.
void sizes(int64_t ls, int64_t le, double sampleRate, float xfadeMs, float ccPct,
          int64_t& xfadeNRequested, int64_t& xfadeN, int64_t& nLoopSamples, int64_t& ccAvail);

// fails gracefully (returns false) if le<=ls or nLayers isn't in [2, 100].
// Unlike Harmonic/Freeze there's no minimum audio requirement around ls: the
// entry crossfade is simply trimmed (sizes()) if it doesn't reach, never
// fails the build. alignPerChannel: the attack alignment picks a rotation
// per channel (each channel follows its own attack; stereo material may end
// up with an offset between L and R) instead of one for all channels.
bool buildRotationLoop(const AudioData& src, int64_t ls, int64_t le, int nLayers, int64_t seed,
                       float xfadeMs, float xfadeCurve, float ccPct, float ccShape, Result& out,
                       bool alignPerChannel = false);

} // namespace rotationdsp
