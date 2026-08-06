// DSP core of the Rotation loop module, with no ImGui dependency (so it can
// be tested from core_test without dragging in the GUI). Port of
// proto/rotation_loop.py: unlike Harmonic/Freeze, Rotation doesn't do any
// spectral synthesis of its own or invent a loop -- it takes the ALREADY
// MARKED loop [ls, le) as-is (assumed seamless, a precondition of the
// method) and builds nLayers circularly rotated copies of that same region
// (offsets[0]==0 = unrotated reference layer; the rest, random offsets
// according to 'seed': -1 = fresh entropy on every call, any other value =
// reproducible -- see layerOffsets in the .cpp). The layers are summed with
// equal-power gain (1/sqrt(nLayers), same as compute_layer_gains
// (falloff_db=None) in the script) and the result is normalized only if it
// exceeds the original peak (never amplified). Since rotation and summation
// commute, the combined buffer stays exactly periodic -- there's no need to
// fix its own wrap, only to splice it with the real attack.
//
// That splice reuses the same core as Harmonic (specloop::
// circularBestShift + rotateAndBlend, see spectral_loop_core.hpp): it
// searches for the rotation of the combined buffer that best aligns with
// the real attack right before ls, and blends 'xfadeMs' of that rotation
// with the attack.
//
// The final step -- explicitly requested for this module -- reuses the
// Classic Crossfade (Inside) core (see crossfade_inside_core.hpp) over the
// loopRegion already spliced with the attack: it blends its own tail into
// its own head, which then gets excluded from the loop (added to the
// attack, right after the entry crossfade) exactly like
// ClassicCrossfadeInside does over the document's marked loop.
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
    int64_t ccAvail = 0;      // final blend (Classic Crossfade Inside) over the loop itself
    std::vector<int64_t> offsets;    // rotation offset of each layer (offsets[0]==0)
    bool safetyNormalized = false;   // true if any channel was attenuated for exceeding the original peak

    // Per channel, in the order they're inserted into the document/preview:
    // attackTail (xfadeN) ++ ccHead (ccAvail, head excluded from the loop,
    // plays as attack) ++ loopBlended (nLoopSamples - ccAvail, the final
    // loop that repeats, already with its tail blended toward ccHead).
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

// fails gracefully (returns false) if le<=ls or nLayers isn't in [2, 100]
// (same as build_rotation_layers in the script). Unlike Harmonic/Freeze
// there's no minimum audio requirement around ls: the entry crossfade is
// simply trimmed (sizes()) if it doesn't reach, never fails the build.
bool buildRotationLoop(const AudioData& src, int64_t ls, int64_t le, int nLayers, int64_t seed,
                       float xfadeMs, float xfadeCurve, float ccPct, float ccShape, Result& out);

} // namespace rotationdsp
