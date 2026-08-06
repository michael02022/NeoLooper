#pragma once

#include "auto_loop_algorithm.hpp"

// ImGui wrapper for loopauditioneerdsp::findLoops (loopauditioneer_finder.hpp),
// a port of Lars Palo's LoopAuditioneer (github.com/GrandOrgue/LoopAuditioneer).
// The search core itself has no ImGui dependency (so it's testable from
// core_test); this class just owns the slider values and adapts it to the
// AutoLoopAlgorithm interface for the Auto Loop popup's algorithm menu.
class AutoLoopLoopAuditioneer : public autoloop::AutoLoopAlgorithm {
public:
    const char* name() const override { return "Lars Palo (LoopAuditioneer)"; }
    void drawParams(double sampleRate) override;
    std::vector<autoloop::AlgoResult> run(const AudioData& audio, int64_t s0, int64_t s1) override;

private:
    bool autoSustain_ = true;
    float derivativeThresholdPct_ = 3.f;    // %
    float minLoopDurationSec_ = 1.f;
    float minDistanceSec_ = 0.3f;
    float qualityFactor_ = 0.01f;
    int maxCandidates_ = 2000;
    int loopsToReturn_ = 6;
    int maxLoopsMultiple_ = 10;
    bool bruteForce_ = false;
};
