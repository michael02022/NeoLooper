#pragma once

#include "PitchDetector.h"
#include "Scales.h"
#include "VibratoTracker.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace snaptune
{
enum class VoiceType : int
{
    soprano,
    altoTenor,
    lowMale,
    instrument,
    numTypes
};

/** Studio: TD-PSOLA, formant-preserving, ~62 ms latency (compensated by the host).
    Live: pitch-synchronous delay-line resampling (the original Auto-Tune method), a few ms of
    latency for monitoring while recording; formants move with the (usually small) correction. */
enum class LatencyMode : int
{
    studio,
    live
};

struct EngineSettings
{
    int key = 0;
    Scale scale = Scale::chromatic;
    uint16_t customMask = 0x0fff;
    float retuneMs = 20.0f;       // time to reach the target note; 0 = instant ("hard tune")
    float transitionMs = 0.0f;    // time to move to the next note; never shorter than retuneMs (0 = follow it)
    float humanize = 0.0f;        // 0..1, relaxes correction on sustained notes
    float amount = 1.0f;          // 0..1, fraction of the pitch error that is corrected
    float formantSemitones = 0.0f;
    float mix = 1.0f;             // 0..1 dry/wet
    float tracking = 0.5f;        // 0..1, higher accepts breathier / noisier input as pitched
    VoiceType voice = VoiceType::altoTenor;
    bool midiTargets = false;     // held MIDI notes replace the scale
    float referenceHz = 440.0f;
    LatencyMode mode = LatencyMode::studio;

    // Vibrato. Keep: the singer's own vibrato is left on the corrected note (scaled by depth) instead
    // of being corrected away. Add: a synthetic vibrato on every held note, after `vibratoDelayMs`.
    bool keepVibrato = false;
    float vibratoDepth = 1.0f;    // 0..2, scale of the kept vibrato
    float vibratoAddCents = 0.0f; // depth of the added vibrato (each way)
    float vibratoRateHz = 5.5f;
    float vibratoDelayMs = 200.0f;
};

/** One analysis hop of pitch data, published lock-free for the editor. */
struct PitchFrame
{
    float inputNote = 0.0f;  // detected pitch, fractional MIDI note
    float outputNote = 0.0f; // pitch after correction
    int targetNote = -1;
    bool voiced = false;
    double time = 0.0;       // NeoLooper patch: input sample position the frame describes
};

/** Real-time pitch corrector.

    Pipeline per sample:
      1. A YIN detector runs every hop on a high-passed mono sum and yields period + voicing.
      2. The detected note is snapped to the nearest allowed note; the correction (in semitones)
         is smoothed by a one-pole whose time constant is the retune speed. A vibrato tracker finds
         the note's centre under a sung vibrato, so the target is chosen from that centre (and the
         vibrato can be kept). Note changes and onsets glide along an S-curve (a critically damped
         response, as in the Fujisaki model of pitch contours) instead of starting at full speed.
      3. Pitch marks are placed one period apart on the input, each where its cycle best matches
         the previous one, pulled toward the waveform peaks.
      4. TD-PSOLA: two-period Hann grains centred on the marks are overlap-added at the corrected
         spacing. Because the grains are not resampled, the formants stay where they were.
         A formant control optionally resamples the grains to move the formants on purpose.

    In Studio mode the engine runs a fixed 3 periods of the lowest detectable pitch behind the input.
    Live mode skips steps 3-4: the input is read through a delay line at the corrected speed, and
    whenever the read point drifts a whole cycle it jumps back or forward one cycle (located by
    cross-correlation, crossfaded), so the delay stays within about one period of the voice.
    Either latency is reported to the host for delay compensation.
*/
class AutoTuneEngine
{
public:
    AutoTuneEngine();

    void prepare (double sampleRate, int maxChannels);
    void reset();

    /** Latency of the active mode. Changes when settings switch between Studio and Live. */
    int getLatencySamples() const noexcept { return latency; }
    LatencyMode getActiveMode() const noexcept { return activeMode; }
    double getFramesPerSecond() const noexcept { return sampleRate / hopSize; }

    void setSettings (const EngineSettings& newSettings) noexcept { settings = newSettings; }

    void noteOn (int note) noexcept;
    void noteOff (int note) noexcept;
    void allNotesOff() noexcept;

    /** Processes channels[c][startSample .. startSample + numSamples) in place. */
    void process (float* const* channels, int numChannels, int startSample, int numSamples, bool bypassed = false);

    /** Editor side: pops up to maxFrames pending pitch frames. Single consumer only. */
    int popFrames (PitchFrame* destination, int maxFrames);

    /** Pitch classes of the MIDI notes currently held (bit 0 = C). */
    uint16_t getHeldNoteMask() const noexcept { return heldMaskForUi.load (std::memory_order_relaxed); }

#if SNAPTUNE_DIAGNOSTICS
    /** Test builds only: counts of the events that can leave a gap or a click in the output. */
    struct Diagnostics
    {
        int64_t liveReadsAhead = 0; // Live reads that interpolated past the newest sample written
        int64_t grainTailCuts = 0;  // Studio grains cut short because their source hadn't arrived
        int64_t grainHeadCuts = 0;  // Studio grains whose start had already been played
        int64_t earlyReads = 0;     // Studio grains that read a correction not computed yet
        double earlyLead = 0.0;     // furthest such read past the newest computed entry, in samples
        int64_t splices = 0, driftBacks = 0, weakSplices = 0; // weak: the cycles correlate under 0.8
        double spliceScoreSum = 0.0, lastSpliceScore = 0.0;

        struct Splice
        {
            int64_t at;   // input sample index when the splice started
            double offset, fade, score;
        };

        std::vector<Splice> spliceLog;
    };

    mutable Diagnostics diagnostics;
#endif

private:
    struct TrackEntry
    {
        double time = 0.0;
        float period = 0.0f;
        float correction = 0.0f;
        bool voiced = false;
    };

    struct TrackSample
    {
        float period = 0.0f;
        float correction = 0.0f;
        bool voiced = false;
    };

    struct Mark
    {
        double position = 0.0;
        float period = 0.0f;
        bool voiced = false;
    };

    void analyseHop();
    /** Computes the correction of track entry `index` (Studio: a few hops after it was detected, so the
        note choice can look at the hops that follow it). */
    void updateCorrection (int64_t index);
    const TrackEntry& entryAt (int64_t index) const noexcept { return track[(size_t) index & (track.size() - 1)]; }
    TrackSample trackAt (double time) const noexcept;

    /** 0 for a clean voice, 1 for a breathy or noisy one (typical aperiodicity 0.035 and up). */
    float breathiness() const noexcept { return juce::jlimit (0.0f, 1.0f, 40.0f * (typicalAperiodicity - 0.01f)); }

    /** Extends the pitch marks up to newestReadable (the cycle match and the octave check read
        further, up to the newest sample written). */
    void generateMarks (int64_t newestReadable);
    void pushMark (const Mark& m) noexcept;
    double refinePeak (int64_t index) const noexcept;
    double matchCycle (double from, double shortest, double longest, double expected, int before, int after) noexcept;
    bool repeatsAtLonger (int64_t end, double shorter, double longer) const noexcept;
    double toneEnergy (int64_t end, int length, double cyclesPerSample) const noexcept;
    const Mark& markAt (int64_t index) const noexcept { return marks[(size_t) (index & markMask)]; }
    int64_t oldestMark() const noexcept;
    int64_t findSourceMark (double synthesisTime) noexcept;

    void studioSample (float* wet);
    void liveSample (float* wet);
    void maybeSplice (double ratio);
    /** Splice length where the cycles line up best; `correlation` receives how well they do (-1..1). */
    double findSpliceLength (double tap, int direction, double minLength, double maxLength, double period, double& correlation) const;
    void startSplice (double offset, double fadeSamples, double correlation);
    bool driftBackInaudible (double delay) const;
    double periodicityAt (double position, double period) const noexcept;
    void switchMode (LatencyMode mode);
    float readInput (int channel, double position) const noexcept;

    void emitGrain (int64_t now);
    void writeGrain (int64_t now, double centre, double source, double halfLeft, double halfRight, float gain);
    float window (double x) const noexcept;

    /** Mean square of the analysis signal between two input positions (clamped to what has arrived). */
    double meanSquare (double from, double to) const noexcept;

    void pushFrame (const PitchFrame& frame) noexcept;
    void updateHeldMask() noexcept;

    EngineSettings settings;

    double sampleRate = 48000.0;
    int maxChannels = 2, activeChannels = 2;
    int maxPeriod = 0, hopSize = 128, latency = 0, unvoicedPeriod = 240, scheduleAhead = 0;
    int studioLatency = 0, liveLatency = 0;
    LatencyMode activeMode = LatencyMode::studio;
    int switchFadePosition = 0, switchFadeLength = 1;

    int ringSize = 0, ringMask = 0;
    std::array<std::vector<float>, 2> inputRing, outputRing;
    std::vector<float> detectionRing, peakRing;
    int64_t writePos = 0;

    // Analysis-signal filters: high-pass for the detector, extra low-pass for peak picking.
    float hpCoeff = 0.0f, hpLastIn = 0.0f, hpLastOut = 0.0f;
    float lpCoeff = 0.0f, lpState = 0.0f;

    PitchDetector detector;
    std::vector<float> frame;
    std::array<float, 3> recentPeriods {};
    int recentCount = 0;
    bool jumpPending = false;
    double estimateDelay = 0.0; // how far behind the newest sample the latest estimate is centred, minus half a period
    int steadyHops = 0; // consecutive voiced hops whose period changed by less than half a semitone
    int guardedHops = 0; // consecutive hops the octave guard has wanted to hold a jump (it holds the first maxGuardedHops)
    int bridgedHops = 0; // consecutive hops pitch continuity kept voiced
    bool detectedVoiced = false; // the detector's voicing of the last hop (the correction may lag behind it)
    int detectedUnvoicedHops = 0; // since the detector's last voiced hop
    int lookaheadHops = 0; // hops the correction runs behind the detector (Studio)
    int64_t correctedCount = 0; // track entries whose correction has been computed
    float anchorPeriod = 0.0f; // period of the last hop that was voiced on its own
    float typicalAperiodicity = 0.0f; // of the voice, averaged over recent steady hops

    // Correction state (one value per hop). The correction applied is the sum of:
    //   noteCorrection     the in-note correction, a one-pole following the wanted correction at the retune speed
    //   glide              what is left of the last note change or onset, decaying along an S-curve
    //   settle             what is left of a discontinuity in the vibrato split, decaying over ~20 ms
    //   vibrato terms      the kept vibrato's depth change and the added vibrato
    // `correction` is that sum.
    struct Glide
    {
        float offset = 0.0f, velocity = 0.0f; // semitones, semitones per second
    };

    void advanceGlide (Glide& glide, double seconds, double timeMs) const noexcept;
    static float glidePosition (const Glide& glide, double seconds, double timeMs) noexcept;

    bool wasVoiced = false;
    int currentTarget = -1;
    float correction = 0.0f;
    float noteCorrection = 0.0f;
    float lastWanted = 0.0f; // the correction wanted on the last voiced hop
    int leapHops = 0; // Live: hops left in which a note change is still part of a leap already heard
    Glide glide, settle;
    double glideMs = 0.0;
    float keptVibrato = 0.0f, addedVibrato = 0.0f; // their terms in the correction
    float vibratoEnvelope = 0.0f;
    double vibratoPhase = 0.0;
    float noteAge = 0.0f;
    VibratoTracker vibratoTracker;
    int64_t hopCount = 0;
    float onsetCandidate = 0.0f; // period of a lone first voiced hop, waiting for the next to confirm it
    float drivenCorrection = 0.0f; // Live: the input-driven part of the correction (in-note + kept vibrato)
    float choiceNote = 0.0f; // Live: smoothed detected pitch the target note is chosen from
    int rawChoiceHops = 0; // Live: consecutive hops the unsmoothed pitch chose another note
    float lastInputNote = 0.0f; // detected pitch of the last voiced hop
    int unvoicedHops = 0; // since the last voiced hop
    float correctionStep = 0.0f; // Live: change of the correction over the last hop, if within one note
    bool hasCorrectionStep = false;
    float stepPower = 0.0f, stepNoise = 0.0f; // running mean square of that change and of its noise
    std::vector<TrackEntry> track;
    int64_t trackCount = 0;
    double lastTrackTime = 0.0;
    float stampPeriod = 0.0f;

    // Pitch marks on the input timeline.
    std::vector<Mark> marks;
    int64_t markCount = 0;
    int64_t markMask = 0;
    float markPolarity = 1.0f;
    int64_t checkedMark = -1; // the mark whose octave check is cached in checkedSpacing,
    double checkedPeriod = 0.0; // for this track period
    double checkedSpacing = 0.0;
    double longerSince = -1.0; // where marks started following a longer spacing than the track (-1: they don't)
    std::vector<float> cycleScratch;
    std::vector<double> cycleEnergy;

    // Synthesis.
    double synthesisTime = 0.0;
    double lastCentre = 0.0;
    double lastSource = 0.0;
    double lastStep = 0.0;
    int64_t lastSourceMark = -1;
    bool lastGrainVoiced = false;
    double lastVoicedCentre = -1.0e12, lastVoicedPeriod = 240.0; // of the last voiced grain
    double voiceLevel = 0.0; // mean square of recent voiced grains, held and slowly released
    double formantStretch = 1.0;
    std::vector<float> windowTable;
    juce::SmoothedValue<float> mixSmoothed;

    // Live mode: delay-line read tap with pitch-synchronous splices.
    double liveMinDelay = 8.0;
    double liveTap = 0.0, spliceTap = 0.0;
    int fadeLength = 0, fadePosition = 0;
    double fadeCorrelation = 1.0; // of the two copies being crossfaded: 1 for whole cycles, 0 for noise
    double liveCorrectionFrom = 0.0, liveCorrectionTo = 0.0;
    int liveRamp = 0;
    float longPeak = 0.0f, longPeakDecay = 0.0f; // input peak over ~1 s
    int quietRun = 0;                             // samples since the input was last above near silence

    // MIDI target notes.
    std::array<int, 128> heldNotes {};
    uint16_t heldMask = 0;
    std::atomic<uint16_t> heldMaskForUi { 0 };

    // Audio -> editor pitch frames.
    static constexpr int frameFifoSize = 2048;
    juce::AbstractFifo frameFifo { frameFifoSize };
    std::array<PitchFrame, frameFifoSize> frameBuffer {};
};
} // namespace snaptune
