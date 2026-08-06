#pragma once

#include "auto_loop_algorithm.hpp"

// ImGui wrapper for autoloopdsp::findLoopPoints (auto_loop_finder.hpp), the
// port of milselarch/autolooper's coarse-to-fine matching. The search core
// itself has no ImGui dependency (so it's testable from core_test); this
// class just owns the two slider values and adapts it to the
// AutoLoopAlgorithm interface for the Auto Loop popup's algorithm menu.
class AutoLoopMilselarch : public autoloop::AutoLoopAlgorithm {
public:
    const char* name() const override { return "milselarch (autolooper)"; }
    void drawParams(double sampleRate) override;
    std::vector<autoloop::AlgoResult> run(const AudioData& audio, int64_t s0, int64_t s1) override;

private:
    float minMs_ = 30.f;
    float maxMs_ = 2000.f;
};
