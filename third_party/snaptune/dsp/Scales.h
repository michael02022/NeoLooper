#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>

namespace snaptune
{
enum class Scale : int
{
    chromatic,
    major,
    minor,
    harmonicMinor,
    melodicMinor,
    majorPentatonic,
    minorPentatonic,
    blues,
    dorian,
    phrygian,
    lydian,
    mixolydian,
    locrian,
    custom,
    numScales
};

inline constexpr std::array<const char*, (size_t) Scale::numScales> scaleNames {
    "Chromatic", "Major", "Minor", "Harmonic Minor", "Melodic Minor", "Major Pentatonic", "Minor Pentatonic",
    "Blues", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Custom"
};

inline constexpr std::array<const char*, 12> noteNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

constexpr uint16_t degreesToMask (std::initializer_list<int> degrees)
{
    uint16_t mask = 0;

    for (auto degree : degrees)
        mask = (uint16_t) (mask | (1u << degree));

    return mask;
}

// Intervals above the root for each scale (bit n set = n semitones above the root is in the scale).
inline constexpr std::array<uint16_t, (size_t) Scale::numScales> scaleIntervals {
    0x0fff,
    degreesToMask ({ 0, 2, 4, 5, 7, 9, 11 }),
    degreesToMask ({ 0, 2, 3, 5, 7, 8, 10 }),
    degreesToMask ({ 0, 2, 3, 5, 7, 8, 11 }),
    degreesToMask ({ 0, 2, 3, 5, 7, 9, 11 }),
    degreesToMask ({ 0, 2, 4, 7, 9 }),
    degreesToMask ({ 0, 3, 5, 7, 10 }),
    degreesToMask ({ 0, 3, 5, 6, 7, 10 }),
    degreesToMask ({ 0, 2, 3, 5, 7, 9, 10 }),
    degreesToMask ({ 0, 1, 3, 5, 7, 8, 10 }),
    degreesToMask ({ 0, 2, 4, 6, 7, 9, 11 }),
    degreesToMask ({ 0, 2, 4, 5, 7, 9, 10 }),
    degreesToMask ({ 0, 1, 3, 5, 6, 8, 10 }),
    0 // Custom: the mask comes from the per-note parameters instead.
};

inline int pitchClass (int midiNote) noexcept { return ((midiNote % 12) + 12) % 12; }

inline bool maskContains (uint16_t mask, int midiNote) noexcept { return ((mask >> pitchClass (midiNote)) & 1u) != 0; }

/** 12-bit pitch-class mask (bit 0 = C, bit 11 = B) of the notes a voice may be tuned to. */
inline uint16_t pitchClassMask (int key, Scale scale, uint16_t customMask) noexcept
{
    if (scale == Scale::custom)
        return (uint16_t) (customMask & 0x0fff);

    const auto intervals = scaleIntervals[(size_t) scale];
    uint16_t mask = 0;

    for (int degree = 0; degree < 12; ++degree)
        if ((intervals >> degree) & 1u)
            mask = (uint16_t) (mask | (1u << pitchClass (key + degree)));

    return mask;
}

/** Closest MIDI note to a fractional pitch whose pitch class is in the mask, or -1 if the mask is empty. */
inline int nearestAllowedNote (float note, uint16_t mask) noexcept
{
    if ((mask & 0x0fff) == 0)
        return -1;

    const int base = (int) std::lround (note);
    int best = -1;
    float bestDistance = 1.0e9f;

    for (int candidate = base - 6; candidate <= base + 6; ++candidate)
    {
        if (! maskContains (mask, candidate))
            continue;

        const float distance = std::abs (note - (float) candidate);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = candidate;
        }
    }

    return best;
}

/** Picks the note to tune to. The current target is kept until another allowed note is closer
    by more than `hysteresis` semitones, so a voice hovering between two notes does not flicker. */
inline int chooseTargetNote (float note, uint16_t mask, int currentTarget, float hysteresis) noexcept
{
    const int nearest = nearestAllowedNote (note, mask);

    if (nearest < 0 || currentTarget < 0 || nearest == currentTarget || ! maskContains (mask, currentTarget))
        return nearest;

    const float currentDistance = std::abs (note - (float) currentTarget);
    const float nearestDistance = std::abs (note - (float) nearest);
    return currentDistance <= nearestDistance + hysteresis ? currentTarget : nearest;
}

inline const char* noteName (int midiNote) noexcept { return noteNames[(size_t) pitchClass (midiNote)]; }

inline int noteOctave (int midiNote) noexcept { return (midiNote < 0 ? (midiNote - 11) / 12 : midiNote / 12) - 1; }
} // namespace snaptune
