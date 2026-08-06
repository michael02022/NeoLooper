// DSP core of the FreezeStretch loop module, with no ImGui dependency (so
// it can be tested from core_test without dragging in the GUI).
//
// Discovered by the user while experimenting with Signalsmith Stretch
// (procmods/signalsmith_stretch_dsp.hpp): pushing the stretch ratio far
// beyond the library's own "clean stretch" threshold (it starts
// randomizing phase for spectrally-distant grain matches past that point,
// by design -- see maxCleanStretch in the vendored signalsmith-stretch.h)
// turns a short grain of audio into an evolving, ambience-like texture
// instead of a clean time-stretch -- conceptually a stretch pinned at its
// most extreme ("0%"/frozen) setting. Two controls drive the freeze
// itself: Position (where the grain is taken from) and Duration (how much
// output to keep) -- NOT a stretch ratio: the grain (kGrainSec below) is a
// small FIXED constant, independent of Duration, so the "how frozen"
// character stays the same regardless of how long you render; Duration
// only cuts the render short, same idea as a bounded generator rather
// than an infinite one.
//
// Unlike Freeze (freeze_loop_dsp.hpp), which keeps the real attack before
// its freeze point and appends the frozen chunk after it, FreezeStretch
// discards everything before the chosen position (explicit user request:
// the point is a self-contained ambiance texture, not "the sample, then a
// tail") -- the whole document gets replaced by the generated chunk. As
// the last step, Classic Crossfade (Inside)'s own core (see
// crossfade_inside_core.hpp) blends the chunk's tail into its own head --
// same technique ClassicCrossfadeInside/RotationLoop already use, reused
// here so the result is loop-ready on its own, with no material before it
// to draw a blend from (same reasoning ClassicCrossfadeInside's own
// header gives for when to prefer it over the regular Classic
// Crossfade). Apply marks the loop at [loopStart, loopEnd) directly, same
// convention as those two.
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace freezestretchdsp {

struct Params {
    double positionFrac = 0.5; // 0 = start of the document, 1 = end
    double durationSec = 5.0;  // how much output to keep (finite, not infinite)
    float ccPct = 25.f;        // Classic Crossfade (Inside), final step: % of the max blend
    float ccShape = 1.f;       // 0 = equal gain, 1 = equal power
};

struct Result {
    bool ok = false;
    // Full generated chunk (already Classic Crossfade (Inside)-blended),
    // meant to become the WHOLE document (the original attack is
    // discarded, not prepended).
    std::vector<std::vector<float>> ambiance;
    // [loopStart, loopEnd) within 'ambiance' is the seamless, repeatable
    // part; loopEnd == ambiance[0].size(). [0, loopStart) is the
    // crossfade's own reference head -- plays once before the loop,
    // same convention as ClassicCrossfadeInside/RotationLoop.
    int64_t loopStart = 0;
    int64_t loopEnd = 0;
};

// Grain length fed into Signalsmith's engine as the "frozen" source
// material. Deliberately FIXED (not derived from durationSec): the point
// is that Duration only limits how much gets rendered, it doesn't change
// how "frozen" the result sounds. Kept as small as the library's own
// minimum viable input allows (its offline exact() needs at least
// roughly one analysis block's worth of input regardless of ratio) so
// the effective stretch ratio (durationSec/kGrainSec) stays as extreme
// as possible -- comfortably past maxCleanStretch even at the shortest
// Duration the UI allows.
constexpr double kGrainSec = 0.2;

// ok=false if there isn't enough audio left after 'positionFrac' for even
// one grain, or durationSec<=0.
Result generate(const AudioData& audio, const Params& params);

} // namespace freezestretchdsp
