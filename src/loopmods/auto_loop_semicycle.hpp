#pragma once

#include "auto_loop_algorithm.hpp"
#include "semicycle_loop_finder.hpp"

// ImGui wrapper for semicycledsp::findLoop (semicycle_loop_finder.hpp). The
// search core has no ImGui dependency (so it's testable from core_test);
// this class owns one parameter set per mode (Short/Long, each with its own
// defaults) and adapts it to the AutoLoopAlgorithm interface.
class AutoLoopSemicycle : public autoloop::AutoLoopAlgorithm {
public:
    const char* name() const override { return "Semicycle match (short/long)"; }
    void drawParams(double sampleRate) override;
    std::vector<autoloop::AlgoResult> run(const AudioData& audio, int64_t s0, int64_t s1) override;

private:
    bool longMode_ = false;
    semicycledsp::Params short_ = semicycledsp::shortDefaults();
    semicycledsp::Params long_ = semicycledsp::longDefaults();
};
