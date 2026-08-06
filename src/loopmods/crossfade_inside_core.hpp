// Blend core for Classic Crossfade (Inside): blends a loop's TAIL with its
// own HEAD, used only as a read reference (never modified) -- see
// classic_crossfade_inside.hpp for the full explanation of the technique.
// Pulled out into a shared core (same reason as spectral_loop_core.hpp for
// Harmonic/Freeze) because Rotation reuses this exact same processing as the
// last step over its own loop synthesized from rotated layers, instead of
// over the document's marked loop -- see rotation_loop_dsp.cpp.
#pragma once

#include <cstdint>
#include <vector>

namespace ccinside {

// Blend duration, in samples: pct (0-100) of the MAXIMUM possible blend
// (half of loopLen -- head and tail can't overlap each other).
int64_t fadeLen(int64_t loopLen, float pct);

// head and tail: 'avail' samples each, from the SAME loop (head = its first
// 'avail' samples, tail = its last 'avail' samples). Returns the tail
// already blended toward the head (shape: 0=equal gain, 1=equal power),
// ready to replace 'tail' -- the head itself isn't modified or included in
// the result; it's up to the caller to decide what to do with it (Classic
// Crossfade Inside and Rotation exclude it from the repeating loop and let
// it play once, as an attack).
std::vector<float> blendTail(const std::vector<float>& head, const std::vector<float>& tail,
                             float shape);

} // namespace ccinside
