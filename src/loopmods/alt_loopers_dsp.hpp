// DSP core of the Ensemble and Random Phase loop modules, and of Freeze's
// Average option (Method::HarmonicFreeze), with no
// ImGui dependency (so it can be tested from alt_loop_test without dragging
// in the GUI).
//
//   * The attack -> loop splice is NeoLooper's own (the Harmonic one): the
//     synthesized loop is circularly rotated to the position that best
//     correlates with the real audio right before ls, and 'xfadeMs' of the
//     real attack is blended into the start of that rotation
//     (as specloop::rotateAndBlend, except that a crossfade longer than the
//     loop tiles it: the cycles under the fade are generated automatically
//     and the loop keeps its own length).
//   * The loop [ls, ls+nLoopSamples) REPLACES [ls, le) and the rest of the
//     document after le is kept (same as Harmonic).
//
// Common pipeline (all methods):
//   1. Period P: from the document's root note + tuneCents.
//   2. Analysis region: the k = floor(L/P) WHOLE cycles that fit in
//      L = le-ls, read with 16-tap sinc interpolation so they span exactly
//      nA = round(k*P) samples -- harmonic h lands exactly on bin h*k. The
//      full region is analysed (dsp::dft handles any length) and the
//      output needs no resampling at all: its length IS nA (pitch error
//      <= 0.5 sample over the whole loop).
//   3. Tukey-windowed FFT, per-method spectral rebuild, inverse FFT.
//   4. Level matched to the RMS of the analysis region (Loop gain on top).
//   5. Splice with the attack (see above), one rotation shared by all
//      channels so the stereo image isn't smeared by per-channel delays.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../audio_data.hpp"
#include "spectral_loop_core.hpp"

namespace altloop {

enum class Method {
    // Each harmonic band's energy (minus the sample's own noise) is re-spread
    // around its harmonic -- a Gaussian or flat band (width interpolated
    // between a low and a high frequency, in thousandths of the harmonic
    // spacing) or N discrete detuned voices -- optionally stretched
    // inharmonically, plus the sample's own noise floor, with random phases:
    // a "chorused", ensemble-like sustain.
    Ensemble,
    // Each harmonic band's energy is collapsed onto the exact harmonic bin,
    // keeping the original phase: a strictly periodic waveform (every cycle
    // identical), optionally shortened to fewer cycles than the region. No
    // module of its own: it's Freeze's Average analysis (through
    // rebuildLoop(), see freeze_loop_dsp.hpp).
    HarmonicFreeze,
    // Spectral phase randomization: the region's magnitude spectrum with
    // random phases.
    RandomPhase,
};


// Shape of Ensemble's crossfade between its low and high band: how the
// value travels from the low band's to the high band's as t goes 0 -> 1
// across [lowFreq, highFreq] (t is log frequency). bend in [-1, 1], 0 =
// linear (a straight line on an octave axis) for both.
enum class BandCurve {
    // Exponential bend: > 0 convex (most of the change happens near the
    // low band), < 0 concave (it stays near the low band's value and
    // changes late, near the high band).
    Curve,
    // Bipolar: > 0 an S (gentle at both bands, steep in the middle), < 0 an
    // inverted S (steep at both bands, a plateau halfway between them).
    Bipolar,
};
// 0 -> 0, 1 -> 1, monotonic; Bipolar is point-symmetric (f(t) + f(1-t) = 1).
double bandShape(double t, BandCurve mode, double bend);

// Ensemble: how each harmonic's energy is spread around it.
enum class HarmonicShape {
    Gaussian, // normal curve, std. deviation = width
    Flat,     // even band with the same std. deviation (+-width*sqrt(3)): denser, flatter chorus
    Voices,   // N discrete voices detuned in cents (true unison, supersaw-like)
};

struct Params {
    Method method = Method::HarmonicFreeze;
    int rootNote = 60;
    float tuneCents = 0.f;     // fine adjustment of the root note's pitch
    float windowPct = 100.f;   // Tukey taper, 0 = rectangular, 100 = Hann (Ensemble always uses 100)
    // Random Phase: true = analyse/rebuild an integer number of cycles of P
    // (loop length = round(k*P)); false = the exact region length, no
    // pitch needed (the pitch is kept as-is).
    bool integerCycles = true;
    // Random Phase: how much of each bin's phase is randomized, 0..100 %
    // (the random offset is drawn in [-amount*pi, amount*pi)). 0 = the
    // region's own phases, i.e. the region as-is (its wrap may click); 100 =
    // fully random, a stationary loop. The Tukey taper is scaled by it too
    // (windowPct * amount): a window only makes sense once the time
    // structure is randomized away -- at low amounts it would leave its
    // fade-in/out envelope audible in the loop.
    float randomPct = 100.f;
    // Random Phase: independent random phases per channel (true) instead of
    // one shared offset (false, keeps the stereo image). Widens a stereo
    // source and makes a mono one (copied to both channels) pseudo-stereo.
    bool decorrelate = false;
    // Random Phase: Daniell smoothing of the power spectrum, a Hann kernel
    // this wide in Hz (0 = off; equivalent to averaging shorter FFTs, as
    // Welch). Tames the spiky, "metallic" magnitudes a single long FFT gives
    // noise; keep it well under the fundamental to preserve harmonics.
    float smoothHz = 0.f;
    // Ensemble
    HarmonicShape shape = HarmonicShape::Gaussian;
    float lowFreq = 100.f;     // Hz            (Gaussian / Flat)
    float lowWidth = 0.f;      // per mille of the harmonic spacing (0 = pure harmonics)
    float highFreq = 8000.f;   // Hz
    float highWidth = 30.f;    // per mille of the harmonic spacing
    // Shape of the crossfade between the two bands (widths and Voices'
    // detune alike), see BandCurve; bend 0 = linear in log frequency.
    BandCurve bandCurve = BandCurve::Curve;
    float bandBend = 0.f;
    // Voices: count (2..16) and spread -- evenly from -detune to +detune
    // cents, so each voice's offset grows with the harmonic number like real
    // detuned oscillators. The detune follows the same two bands as the
    // widths (lowDetuneCents up to lowFreq, highDetuneCents from highFreq,
    // crossfaded in log frequency between them, see bandValue). Each voice
    // sits between the two nearest FFT bins (power split linearly): detune
    // finer than one bin (f0 / cycles) can't beat within the loop and
    // blends into the center.
    int voices = 7;
    float lowDetuneCents = 10.f;
    float highDetuneCents = 15.f;
    // Stiff-string inharmonicity B: harmonic h moves to
    // h*f0*sqrt(1+B*h^2)/sqrt(1+B) (fundamental kept in tune), rounded to a
    // bin so the loop stays seamless. 0 = harmonic; ~1e-4..1e-3 piano-like,
    // more = bell/metallic.
    float inharmonicity = 0.f;
    // Noise floor with the sample's REAL color: the residual between the
    // harmonics (the spectrum with every harmonic masked out, gaps
    // interpolated, smoothed) at floorPct % of its own level (100 = as in
    // the sample). The harmonics' energy excludes that residual.
    float floorPct = 0.f;
    // HarmonicFreeze: cycles in the output loop, 0 = all k cycles of the region.
    int loopCycles = 0;
    // Ensemble / Random Phase: -1 = fresh entropy on every call, else reproducible.
    int64_t seed = -1;
    float xfadeMs = 30.f;
    float xfadeCurve = 0.5f;   // 0 = equal gain, 1 = equal power
    // User level of the loop, dB over the automatic one (the region's RMS);
    // the real attack is untouched, the crossfade blends into the new level.
    float loopGainDb = 0.f;
};

struct Result {
    std::string error;          // empty on success; human-readable reason otherwise
    double period = 0.0;        // samples per cycle of rootNote+tuneCents
    int64_t cycles = 0;         // whole cycles analysed (0 = Random Phase without integer cycles)
    int64_t loopCycles = 0;     // cycles in the output loop (0 = free length)
    int64_t nLoopSamples = 0;
    int64_t xfadeNRequested = 0;
    int64_t xfadeN = 0;
    int64_t shift = 0;          // rotation chosen by the correlation search
    float peak = 0.f;           // peak of the inserted content (attack blend + loop)
    float loopPeak = 0.f;       // peak of the loop alone (loopRegion), after loopGainDb
    std::vector<specloop::RotatedBlend> ch; // attackTail / loopRegion per channel
};

// Ensemble's two-band value at 'freq' -- the harmonic width (per mille of
// the harmonic spacing) for Gaussian/Flat, the detune (cents) for Voices:
// v1 up to f1 (low band), v2 from f2 up (high band), and between them a
// crossfade over LOG frequency shaped by (curve, bend) -- with bend 0 a
// straight line on an octave axis (linear in Hz would cram the whole
// transition into the top octaves). f2 <= f1: a step at f2. Shared by the
// DSP and the window's band curve.
double bandValue(double freq, double f1, double v1, double f2, double v2,
                 BandCurve curve = BandCurve::Curve, double bend = 0.0);

// Samples per cycle of a MIDI note + cents (A4 = 440 Hz).
double notePeriod(double sampleRate, int rootNote, float tuneCents);

// false (with out.error set) if the region is too short for the method, the
// region is silent, etc.
bool build(const AudioData& src, int64_t ls, int64_t le, const Params& p, Result& out);

// Steps 1-4 only: the rebuilt loop per channel (nLoopSamples long,
// level-matched, UNROTATED -- for HarmonicFreeze, with the phase the audio
// has at ls) and out's analysis fields; no splice (out.ch stays empty). Used
// by Freeze's "Average" option to rebuild its cycle from a section.
bool rebuildLoop(const AudioData& src, int64_t ls, int64_t le, const Params& p, Result& out,
                 std::vector<std::vector<double>>& loop);

// specloop::rotateAndBlend that also accepts a crossfade LONGER than the
// loop: the rotated loop is tiled under the attack (the cycles the fade
// needs are generated on the fly) while loopRegion stays one loop long.
// Identical to rotateAndBlend when xfadeN <= loop.size().
void tiledBlend(const std::vector<double>& loop, const std::vector<double>& ref, int64_t shift,
                int64_t xfadeN, float xfadeCurve, specloop::RotatedBlend& out);

// Rotation shared by every channel that best aligns 'loop' (tiled if 'ref'
// is longer) with 'ref' -- the automatic phase of all these modules.
int64_t bestShift(const std::vector<std::vector<double>>& loop,
                  const std::vector<std::vector<double>>& ref);

} // namespace altloop
