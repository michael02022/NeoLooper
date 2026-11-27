// DSP core of Process > Autotune, with no ImGui dependency (testable from
// core_test). A thin offline driver around Snaptune's AutoTuneEngine
// (third_party/snaptune, AGPL-3.0), always in its Studio mode: YIN pitch
// detection + TD-PSOLA resynthesis that keeps the formants in place. The
// engine is a real-time streaming processor with a fixed latency; offline
// we simply feed it the region (plus some context on each side), flush it
// with that many zeros and drop the first 'latency' output samples, so the
// result lines up sample for sample with the input.
//
// Options (the rest of Snaptune's controls stay at its defaults: Retune
// Speed 20 ms, chromatic scale, no humanize/vibrato/formant shift, 100 %
// wet, the "Instrument" detection range 65..1600 Hz):
//   * useMidi:  the target is the document's root note (its pitch class, as
//               if that MIDI note were held -- Snaptune's "MIDI Target
//               Notes"), instead of the nearest semitone.
//   * amount:   fraction of the pitch error that gets corrected.
//   * lowNotes: "process for low notes" -- the region is processed an
//               octave up and brought back down. Done without resampling:
//               the engine is simply told the sample rate is twice the real
//               one, so it hears every frequency an octave higher (and the
//               output, read back at the real rate, is an octave lower
//               again). That moves notes below the detector's 65 Hz floor
//               (down to ~32 Hz) into range. Time constants are halved to
//               compensate, so the retune speed stays 20 ms of real time.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "../audio_data.hpp"

namespace autotunedsp {

struct Spec {
    bool useMidi = false;
    int rootNote = 60;      // target when useMidi (pitch class)
    float amountPct = 100.f;
    bool lowNotes = false;
};

// One analysis hop (~2.7 ms of the engine's timeline): pitches as
// fractional MIDI notes, already brought back down an octave in lowNotes
// mode. 'pos' is the document sample the hop describes.
struct PitchPoint {
    int64_t pos = 0;
    float inputNote = 0.f;  // detected pitch
    float outputNote = 0.f; // pitch after correction
    int targetNote = -1;
    bool voiced = false;
};

struct Result {
    std::vector<std::vector<float>> audio; // [s0,s1) per channel, same length
    std::vector<PitchPoint> curve;         // hops inside [s0,s1), in order
    int64_t latency = 0;                   // engine latency (samples of its timeline)
};

// Audio before s0 / after s1 fed to the engine so the detector has locked
// on (and the resynthesis is settled) by the time the region starts.
constexpr double kContextSec = 0.25;
// When the region doesn't reach the start/end of the sample, its edges
// crossfade from/to the untouched audio over this long, so a correction
// already active at the boundary doesn't step.
constexpr double kEdgeFadeSec = 0.01;

// false if s1 <= s0, there's no audio, or 'cancel' was raised midway (it's
// polled every block; lets the GUI drop a render made stale by a newer
// setting). 'progress' (optional) goes 0 -> 1.
bool apply(const AudioData& src, int64_t s0, int64_t s1, const Spec& spec, Result& out,
           const std::atomic<bool>* cancel = nullptr, std::atomic<float>* progress = nullptr);

} // namespace autotunedsp
