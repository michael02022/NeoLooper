#pragma once

#include "Scales.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace snaptune
{
/** Separates a sung vibrato from the note it oscillates around, one pitch estimate (hop) at a time.

    The detected pitch is smoothed a little and its turning points (peaks and troughs) are found
    with a hysteresis that grows with the swing. Once a cycle and a half of regular swings has been
    seen (3.5-9 Hz, 10-100 cents peak to peak, steady in rate and width, centred on a note rather
    than alternating between two), the tracker locks: the note's centre is then the mean pitch over
    the last full vibrato period, which cancels the oscillation whatever its phase, and the vibrato is
    the pitch minus that centre. It fades in over 80 ms after locking.

    The lock is released as soon as the pitch leaves the band the vibrato has been swinging in (the
    start of a move to another note), when a swing comes too late or too irregular, or when the note
    ends. The caller is told the vibrato value at that moment, so it can keep the output continuous.
*/
class VibratoTracker
{
public:
    struct Output
    {
        float centre = 0.0f;  // pitch without the vibrato (the pitch itself when not locked)
        float vibrato = 0.0f; // pitch minus centre: 0 when not locked
        bool locked = false;
        bool released = false;     // the lock was released on this hop
        float releasedVibrato = 0.0f; // the vibrato this hop would have had if the lock had held
    };

    void prepare (double newHopSeconds)
    {
        hopSeconds = newHopSeconds;
        smoothing = (float) (1.0 - std::exp (-hopSeconds / 0.005));
        fadeStep = (float) (hopSeconds / 0.08);
        reset();
    }

    /** A new note (or a long gap): forget everything. */
    void reset() noexcept
    {
        count = 0;
        smoothed = 0.0f;
        direction = 0;
        extremaCount = 0;
        locked = false;
        fade = 0.0f;
        scatter = 0.0f;
        outlierHops = 0;
        historyHops.fill (-1);
    }

    /** One voiced estimate. `note` is the detected pitch (fractional MIDI note) and `mask` the allowed
        notes. `hop` counts hops since the engine started, so gaps (unvoiced hops) are timed right. */
    Output push (float note, int64_t hop, uint16_t mask) noexcept
    {
        Output out;
        out.centre = note;

        // An octave error of the detector (it reads a breathy voice an octave low now and then, for a
        // few hops): no vibrato drops an octave in a hop. Leave it out of the tracking, so it can't
        // release the lock or pass for a swing; if it lasts it is a real octave leap. Any other jump
        // is taken at once.
        if (count > 0 && std::abs (note - smoothed + 12.0f) < 1.0f)
        {
            if (++outlierHops <= maxOutlierHops)
            {
                if (locked)
                {
                    out.centre = meanOverPeriod (hop - 1);
                    out.locked = true;
                }

                return out;
            }

            if (locked)
                release (out, note, hop);

            const bool released = out.released;
            const float releasedVibrato = out.releasedVibrato;
            reset();
            out = push (note, hop, mask);
            out.released = released;
            out.releasedVibrato = releasedVibrato;
            return out;
        }

        outlierHops = 0;

        if (count == 0)
            smoothed = note;
        else
            smoothed += smoothing * (note - smoothed);

        // The estimates' own scatter, from their second difference: a vibrato barely bends within a
        // hop, so what bends is noise (white noise of deviation s gives a mean |d2| of about 2 s).
        if (count >= 2 && historyHops[(size_t) ((hop - 1) & historyMask)] == hop - 1 && historyHops[(size_t) ((hop - 2) & historyMask)] == hop - 2)
        {
            const float bend = std::abs (note - 2.0f * history[(size_t) ((hop - 1) & historyMask)] + history[(size_t) ((hop - 2) & historyMask)]);
            scatter += 0.05f * (0.5f * std::min (bend, 0.5f) - scatter);
        }

        history[(size_t) (hop & historyMask)] = note;
        historyHops[(size_t) (hop & historyMask)] = hop;
        ++count;

        // While locked, leaving the band the vibrato swings in (by more than a third of its width) is
        // the start of a move to another note, and a swing that doesn't turn around in time means the
        // vibrato has stopped. A turn is confirmed about a third of a swing after it happens, and a
        // vibrato that slows down takes longer: allow twice the last swing. (Holding on to a vibrato
        // that has stopped costs nothing; the pitch then sits at the centre anyway.)
        if (locked)
        {
            const auto& peak = lastOfType (true);
            const auto& trough = lastOfType (false);
            const float width = peak.value - trough.value;
            const float margin = std::max (0.15f, 0.35f * width);
            const float halfPeriod = (float) std::max (extrema[(size_t) ((extremaCount - 1) % maxExtrema)].hop
                                                           - extrema[(size_t) ((extremaCount - 2) % maxExtrema)].hop,
                                                       extrema[(size_t) ((extremaCount - 2) % maxExtrema)].hop
                                                           - extrema[(size_t) ((extremaCount - 3) % maxExtrema)].hop);
            const bool outside = smoothed > peak.value + margin || smoothed < trough.value - margin;
            const bool late = (float) (hop - extrema[(size_t) ((extremaCount - 1) % maxExtrema)].hop) > 2.0f * halfPeriod + 6.0f;

            if (outside || late)
                release (out, note, hop);
        }

        detectTurn (hop, mask, out, note);

        if (locked)
        {
            fade = std::min (1.0f, fade + fadeStep);
            out.centre = note - fade * (note - meanOverPeriod (hop));
            out.vibrato = note - out.centre;
            out.locked = true;
        }

        return out;
    }

    bool isLocked() const noexcept { return locked; }

    /** Most recent full vibrato period, in hops (0 when not locked). */
    int64_t periodHops() const noexcept
    {
        return locked && extremaCount >= 3 ? extrema[(size_t) ((extremaCount - 1) % maxExtrema)].hop - extrema[(size_t) ((extremaCount - 3) % maxExtrema)].hop
                                           : 0;
    }

private:
    struct Extremum
    {
        int64_t hop = 0;
        float value = 0.0f;
        bool peak = false;
    };

    static constexpr int maxExtrema = 8;
    static constexpr int historySize = 512; // hops (~1.4 s): longer than any vibrato period
    static constexpr int64_t historyMask = historySize - 1;

    const Extremum& lastOfType (bool peak) const noexcept
    {
        for (int i = extremaCount - 1; i >= std::max (0, extremaCount - maxExtrema); --i)
            if (extrema[(size_t) (i % maxExtrema)].peak == peak)
                return extrema[(size_t) (i % maxExtrema)];

        return extrema[(size_t) ((extremaCount - 1) % maxExtrema)];
    }

    float meanOverPeriod (int64_t hop) const noexcept
    {
        // Two periods when there are: one spurious turn (noise) then shortens the span only a little.
        const int64_t span = std::max<int64_t> (1, locked && extremaCount >= 5 ? (extrema[(size_t) ((extremaCount - 1) % maxExtrema)].hop
                                                                                     - extrema[(size_t) ((extremaCount - 5) % maxExtrema)].hop)
                                                                                 : periodHops());
        double sum = 0.0;
        int n = 0;

        for (int64_t h = hop - span + 1; h <= hop; ++h)
        {
            if (historyHops[(size_t) (h & historyMask)] == h)
            {
                sum += history[(size_t) (h & historyMask)];
                ++n;
            }
        }

        return n > 0 ? (float) (sum / n) : history[(size_t) (hop & historyMask)];
    }

    void release (Output& out, float note, int64_t hop) noexcept
    {
        out.released = true;
        out.releasedVibrato = fade * (note - meanOverPeriod (hop));
        locked = false;
        fade = 0.0f;

        // Start over from here: what follows is another note, or a vibrato that has to prove itself again.
        extremaCount = 0;
        direction = 0;
        candidateHigh = candidateLow = smoothed;
        candidateHighHop = candidateLowHop = hop;
    }

    void detectTurn (int64_t hop, uint16_t mask, Output& out, float note) noexcept
    {
        if (direction == 0 && extremaCount == 0 && count == 1)
        {
            candidateHigh = candidateLow = smoothed;
            candidateHighHop = candidateLowHop = hop;
            return;
        }

        if (smoothed > candidateHigh)
        {
            candidateHigh = smoothed;
            candidateHighHop = hop;
        }

        if (smoothed < candidateLow)
        {
            candidateLow = smoothed;
            candidateLowHop = hop;
        }

        // The turn has to exceed the (smoothed) estimates' own scatter, and a quarter of the last swing.
        const float lastWidth = extremaCount >= 2 ? std::abs (extrema[(size_t) ((extremaCount - 1) % maxExtrema)].value
                                                              - extrema[(size_t) ((extremaCount - 2) % maxExtrema)].value)
                                                  : 0.0f;
        const float hysteresis = std::max ({ 0.04f, 1.5f * scatter, 0.25f * std::min (lastWidth, 1.0f) });

        if (direction >= 0 && candidateHigh - smoothed > hysteresis && candidateHighHop < hop)
        {
            if (addExtremum ({ candidateHighHop, candidateHigh, true }, mask, out, note, hop))
            {
                direction = -1;
                candidateLow = smoothed;
                candidateLowHop = hop;
            }
        }
        else if (direction <= 0 && smoothed - candidateLow > hysteresis && candidateLowHop < hop)
        {
            if (addExtremum ({ candidateLowHop, candidateLow, false }, mask, out, note, hop))
            {
                direction = 1;
                candidateHigh = smoothed;
                candidateHighHop = hop;
            }
        }
    }

    /** Returns false when the turn was taken for noise and forgotten (the caller then leaves the
        direction alone: the swing in progress before it carries on). */
    bool addExtremum (const Extremum& e, uint16_t mask, Output& out, float note, int64_t hop) noexcept
    {
        extrema[(size_t) (extremaCount % maxExtrema)] = e;
        ++extremaCount;

        if (extremaCount < 3)
            return true;

        const auto& c = extrema[(size_t) ((extremaCount - 1) % maxExtrema)];
        const auto& b = extrema[(size_t) ((extremaCount - 2) % maxExtrema)];
        const auto& a = extrema[(size_t) ((extremaCount - 3) % maxExtrema)];
        const auto halfA = (double) (b.hop - a.hop) * hopSeconds, halfB = (double) (c.hop - b.hop) * hopSeconds;
        const float widthA = std::abs (b.value - a.value), widthB = std::abs (c.value - b.value);

        if (locked)
        {
            // Looser while locked: a real vibrato drifts in rate and width from cycle to cycle.
            const bool keeps = halfB > 0.045 && halfB < 0.18 && widthB > 0.05f && widthB < 1.3f && widthB < 3.0f * widthA
                               && widthB > widthA / 3.0f;

            if (keeps)
                return true;

            // A swing too quick or too small for a vibrato, inside one that is going on, is a wobble
            // of breath noise on the way (a dip just after a turn, say): forget it and the turn it
            // started from, and carry on with the swing they interrupted.
            const bool wobble = halfB < 0.045 || widthB < 0.05f || widthB < widthA / 3.0f;

            if (wobble && extremaCount >= 5)
            {
                const Extremum start = b;
                extremaCount -= 2;

                if (start.peak)
                {
                    direction = 1;
                    candidateHigh = std::max (start.value, smoothed);
                    candidateHighHop = start.value >= smoothed ? start.hop : hop;
                }
                else
                {
                    direction = -1;
                    candidateLow = std::min (start.value, smoothed);
                    candidateLowHop = start.value <= smoothed ? start.hop : hop;
                }

                return false;
            }

            release (out, note, hop);
            return true;
        }

        // A vibrato is 3.5-9 Hz (half periods of 55-143 ms), 10 to 100 cents peak to peak, and steady.
        // A trill alternates between two notes instead: its extremes sit on notes and its centre
        // between them, so the centre has to be about as close to a note as the extremes are.
        const bool rate = halfA > 0.055 && halfA < 0.143 && halfB > 0.055 && halfB < 0.143 && halfB < 1.67 * halfA && halfB > 0.6 * halfA;
        const bool width = widthA > 0.1f && widthA < 1.0f && widthB > 0.1f && widthB < 1.0f && widthB < 2.0f * widthA && widthB > 0.5f * widthA;

        if (! (rate && width))
            return true;

        const float centre = 0.25f * (a.value + 2.0f * b.value + c.value);
        auto distance = [mask] (float n)
        {
            const int nearest = nearestAllowedNote (n, mask);
            return nearest < 0 ? 0.0f : std::abs (n - (float) nearest);
        };

        if (distance (centre) > 0.5f * (distance (b.value) + distance (c.value)) + 0.25f)
            return true;

        locked = true;
        fade = 0.0f;
        return true;
    }

    double hopSeconds = 128.0 / 48000.0;
    float smoothing = 0.4f, fadeStep = 0.03f;
    std::array<float, historySize> history {};
    std::array<int64_t, historySize> historyHops {};
    int64_t count = 0;
    float smoothed = 0.0f;
    int direction = 0;
    float candidateHigh = 0.0f, candidateLow = 0.0f;
    int64_t candidateHighHop = 0, candidateLowHop = 0;
    std::array<Extremum, maxExtrema> extrema {};
    int extremaCount = 0;
    bool locked = false;
    float fade = 0.0f;
    float scatter = 0.0f; // typical deviation of one estimate, semitones
    int outlierHops = 0;
    static constexpr int maxOutlierHops = 12;    // ~32 ms (hops are ~2.7 ms): longer is a real leap
};
} // namespace snaptune
