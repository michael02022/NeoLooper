// DSP core of the "Semicycle match" Auto Loop method, with no ImGui
// dependency (so it can be tested from core_test). Implemented from a
// written functional spec (clean room), not from any other program's code.
//
// How it works:
//   1. Every sample is quantized to 16 bits and mapped to "signed dB":
//      20*log10(x) for x>0 (<= 0 dB), -20*log10(-x) for x<0 (>= 0 dB),
//      -100 for x==0 (and for anything outside the sample). Within a
//      half-cycle that's monotonic in x, so low-level detail weighs as much
//      as the peaks when two spots are compared.
//   2. Each frame gets a "slope signature": the signs of the 16 slopes
//      around it of the L/R mid (in signed dB), packed as a base-3 number.
//      Two frames with the same signature have the same local shape; a
//      signature of 0 (all slopes flat) never matches.
//   3. The mid is cut into half-cycles at every strict sign change; loop
//      points are always the first frame of a half-cycle (i.e. right after
//      a zero crossing), except in Long mode's refinement pass.
//   4. A candidate (s,e) is only scored if both signatures match; its error
//      is the triangular-weighted mean |difference| (signed dB, per
//      channel) of a window of W frames on both sides of s against the same
//      offsets around e, with early exit once it can't beat the best so far.
//
//   Short mode: for each half-cycle start e in the region (scanned from the
//     end), the start is fixed exactly 'cycles' periods earlier.
//   Long mode: for each e, every half-cycle start whose loop length falls in
//     [minLen,maxLen] is tried; then the best loop's length is kept fixed
//     and its end slides frame by frame over +-1000 frames to refine it.
//
// The returned end is exclusive (NeoLooper's loopEnd convention): [s,e)
// spans a whole number of half-cycles of the same polarity at both ends.
#pragma once

#include <cstdint>

#include "../audio_data.hpp"

namespace semicycledsp {

enum class Mode { Short, Long };

struct Params {
    Mode mode = Mode::Short;
    // Search region (where the loop END may fall), in % of the whole
    // sample. Ignored when 'useSelection' is set: the selection replaces it.
    double regionStartPct = 25.0;
    double regionEndPct = 100.0;
    // Short mode: number of periods (2 half-cycles each) the loop spans.
    int cycles = 4;
    // Long mode: allowed loop length, in % of the search region. With a
    // selection these are fixed at 40% / 100% of the selection instead.
    double minLengthPct = 2.0;
    double maxLengthPct = 100.0;
    // Width of the comparison window around each point.
    double compareMs = 5.0;
    // [selStart,selEnd) replaces the configured region (and, in Long mode,
    // the length limits -- see above).
    bool useSelection = false;
    int64_t selStart = 0, selEnd = 0;
};

// Defaults per mode, as presented in the UI.
Params shortDefaults();
Params longDefaults();

struct Result {
    bool ok = false;
    int64_t start = 0;
    int64_t end = 0;     // exclusive
    double error = 0.0;  // mean |signed dB difference|, lower = better
    int64_t evaluations = 0;
};

Result findLoop(const AudioData& audio, const Params& params);

} // namespace semicycledsp
