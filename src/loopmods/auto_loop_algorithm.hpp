// Interface for one entry in the Auto Loop toolbar button's algorithm menu.
// Each author's method lives in its own auto_loop_<author>.hpp/.cpp
// implementing this interface -- mirrors the one-file-per-type convention
// already used for LoopModule (classic_crossfade.*, freeze_loop.*, ...),
// just keyed by author instead of by technique since that's how the Auto
// Loop menu presents them. Registered in EditorUI's autoLoopAlgos_ (see
// the constructor in editor_ui.cpp), the same way modules_ registers
// LoopModules.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../audio_data.hpp"

namespace autoloop {

struct AlgoResult {
    int64_t start = 0;
    int64_t end = 0;
    // Lower = better. Algorithm-defined scale (not comparable across
    // algorithms) -- shown to the user alongside each candidate, not used
    // to rank across a run() call (run() already returns them best-first).
    double quality = 0.0;
    std::string info; // optional short label, e.g. "score 0.0031"
};

class AutoLoopAlgorithm {
public:
    virtual ~AutoLoopAlgorithm() = default;
    // Shown in the Auto Loop popup's algorithm dropdown, e.g.
    // "milselarch (autolooper)" -- credit the author, since that's how
    // the menu distinguishes entries.
    virtual const char* name() const = 0;
    // Draws this algorithm's own parameter widgets inside the popup.
    // sampleRate is the document's, for algorithms that want to show
    // ms/sample conversions.
    virtual void drawParams(double sampleRate) = 0;
    // Runs the algorithm over [s0,s1) of 'audio' using the widgets'
    // current values. Returns 0 or more candidate loop points, best
    // (most likely to sound good) first. Most algorithms return a single
    // candidate; some (e.g. LoopAuditioneer) are deliberately designed to
    // return several ranked alternatives for the user to audition instead
    // of committing to just one guess.
    virtual std::vector<AlgoResult> run(const AudioData& audio, int64_t s0, int64_t s1) = 0;
};

} // namespace autoloop
