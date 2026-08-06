// DSP core of the PingPong Harmonic loop module, with no ImGui dependency
// (so it can be tested from core_test without dragging in the GUI). Port of
// proto/mirror_loop.py: unlike Harmonic (spectral_loop_core.hpp), which
// quantizes a SINGLE frequency per bin taken from one analysis frame, here
// EVERY STFT frame of the [ls, le) region is analyzed separately (magnitude
// + real frequency per bin, phase vocoder) and synthesis walks those frames
// in the order [0..M-1, M-2..1] -- forward then backward, each end visited
// once per lap -- so what gets reversed is the ORDER of the frames (which
// harmonic was loud when, the envelope/formant's position in time), never
// the synthesis phase, which always advances forward. Without that last
// part the result would be literally reversed audio; with it, only the
// envelope walks backward while the fine pitch/texture sounds normal.
//
// Identity phase-locking (Laroche & Dolson): spectral peaks are detected
// (averaging the magnitude across all of the region's frames) and only
// those bins accumulate phase freely via their own real frequency; the rest
// of the bins stay "locked" to their nearest peak's phase plus the phase
// offset that bin had relative to its peak IN THE FRAME CURRENTLY BEING
// PLAYED at each step -- without this, each harmonic's fine shape blurs as
// soon as the frame order stops being strictly forward (see the docstring
// in proto/mirror_loop.py for why).
//
// Each peak frequency is quantized (same as Harmonic) to an integer number
// of cycles per LAP (one full pass of the mirrored sequence), so a lap
// closes at an exact phase and the result is genuinely periodic lap after
// lap -- it can repeat forever with no drift or beating, same as
// build_mirror_loop in the script.
//
// Unlike the prototype (which operates in mono), here each channel is
// analyzed and synthesized separately, same as the rest of this project's
// spectral modules (Harmonic/Freeze/Rotation), to preserve the sample's
// real stereo image.
#pragma once

#include <cstdint>
#include <vector>

#include "../audio_data.hpp"

namespace pingpongdsp {

struct Result {
    int64_t xfadeNRequested = 0;
    int64_t xfadeN = 0;
    // Duration of ONE lap (there-and-back pass of frames), in samples --
    // this is what gets marked as the loop [ls, ls+oneLapSamples) on Apply,
    // regardless of how many laps were rendered into 'body' (see laps in
    // buildLoop).
    int64_t oneLapSamples = 0;
    double seamError = 0.0; // lap->lap continuity, diagnostic

    // Per channel, in the order they're inserted into the document/preview:
    // attackTail (xfadeN, real attack blended into the start of the first
    // lap) ++ body (laps*oneLapSamples, 'laps' synthesized laps
    // concatenated -- only the first is the loop that gets marked; the rest
    // is there so you can audition several cycles without depending on a
    // sampler, same as --laps in the reference script).
    struct Channel {
        std::vector<float> attackTail;
        std::vector<float> body;
    };
    std::vector<Channel> ch;
};

// Cheap arithmetic (no FFT), used both by buildLoop() and for immediate
// feedback in the GUI while dragging a slider. oneLapSamples comes out as 0
// if the loop region doesn't reach at least 3 STFT frames (needed: 1 of
// phase context + 2 real ones, same check as mirror_loop.py).
void sizes(int64_t ls, int64_t le, double sampleRate, int nFft, float xfadeMs,
          int64_t& xfadeNRequested, int64_t& xfadeN, int64_t& oneLapSamples);

// false if the [ls, le) region doesn't reach at least 3 STFT frames at this
// fft-size, or if there isn't even one hop of audio before ls (phase
// context needed to estimate the first real frame's true frequency). laps <
// 1 also fails. Internally it always synthesizes at least one extra lap
// beyond 'laps' (so it can measure seamError by comparing two full laps and
// have margin when discarding the first xfadeN samples, already absorbed
// by attackTail) and trims the result.
bool buildLoop(const AudioData& src, int64_t ls, int64_t le, int nFft, int laps,
              float xfadeMs, float xfadeCurve, Result& out);

} // namespace pingpongdsp
