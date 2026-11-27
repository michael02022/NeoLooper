#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace snaptune
{
void PitchDetector::prepare (int newWindowSize, int newMaxLag)
{
    windowSize = newWindowSize;
    maxLag = newMaxLag;

    const int frameSize = getFrameSize();
    int order = 1;

    while ((1 << order) < frameSize)
        ++order;

    fftSize = 1 << order;
    fft = std::make_unique<juce::dsp::FFT> (order);

    newestFirst.assign ((size_t) frameSize, 0.0f);
    spectrumA.assign ((size_t) fftSize * 2, 0.0f);
    spectrumB.assign ((size_t) fftSize * 2, 0.0f);
    cmnd.assign ((size_t) maxLag + 1, 1.0f);
    difference.assign ((size_t) maxLag + 1, 0.0);
    energyPrefix.assign ((size_t) frameSize + 1, 0.0);

    // FFT engines differ in how they scale the inverse transform, so measure it once:
    // the autocorrelation of a unit impulse at lag 0 must come out as exactly 1.
    std::fill (spectrumA.begin(), spectrumA.end(), 0.0f);
    spectrumA[0] = 1.0f;
    fft->performRealOnlyForwardTransform (spectrumA.data(), true);

    for (int k = 0; k <= fftSize / 2; ++k)
    {
        const float re = spectrumA[(size_t) (2 * k)], im = spectrumA[(size_t) (2 * k + 1)];
        spectrumA[(size_t) (2 * k)] = re * re + im * im;
        spectrumA[(size_t) (2 * k + 1)] = 0.0f;
    }

    fft->performRealOnlyInverseTransform (spectrumA.data());
    inverseScale = std::abs (spectrumA[0]) > 1.0e-9f ? 1.0f / spectrumA[0] : 1.0f;
}

PitchDetector::Result PitchDetector::analyse (const float* frame, float minPeriod, float maxPeriod, float threshold, int window, float previousPeriod)
{
    Result result;
    const int frameSize = getFrameSize();
    const int w = window > 0 ? std::min (window, windowSize) : windowSize;

    // Reversing puts the newest sample at index 0, so the textbook formulation (compare the first
    // W samples with the signal `tau` samples further on) measures the most recent audio.
    for (int i = 0; i < frameSize; ++i)
        newestFirst[(size_t) i] = frame[frameSize - 1 - i];

    energyPrefix[0] = 0.0;

    for (int i = 0; i < frameSize; ++i)
        energyPrefix[(size_t) i + 1] = energyPrefix[(size_t) i] + (double) newestFirst[(size_t) i] * newestFirst[(size_t) i];

    const double windowEnergy = energyPrefix[(size_t) w];
    result.rms = (float) std::sqrt (windowEnergy / w);

    if (windowEnergy < 1.0e-10)
        return result;

    // r(tau) = sum_{j < W} z[j] z[j + tau], via conj(FFT(window)) * FFT(frame).
    std::fill (spectrumA.begin(), spectrumA.end(), 0.0f);
    std::fill (spectrumB.begin(), spectrumB.end(), 0.0f);
    std::copy_n (newestFirst.begin(), w, spectrumA.begin());
    std::copy_n (newestFirst.begin(), frameSize, spectrumB.begin());
    fft->performRealOnlyForwardTransform (spectrumA.data(), true);
    fft->performRealOnlyForwardTransform (spectrumB.data(), true);

    for (int k = 0; k <= fftSize / 2; ++k)
    {
        const auto re = (size_t) (2 * k), im = re + 1;
        const float ar = spectrumA[re], ai = spectrumA[im], br = spectrumB[re], bi = spectrumB[im];
        spectrumB[re] = ar * br + ai * bi;
        spectrumB[im] = ar * bi - ai * br;
    }

    fft->performRealOnlyInverseTransform (spectrumB.data());

    const int lastLag = std::min (maxLag, (int) std::ceil (maxPeriod) + 1);
    double runningSum = 0.0;
    difference[0] = 0.0;
    cmnd[0] = 1.0f;

    for (int tau = 1; tau <= lastLag; ++tau)
    {
        const double lagEnergy = energyPrefix[(size_t) (tau + w)] - energyPrefix[(size_t) tau];
        const double d = std::max (0.0, windowEnergy + lagEnergy - 2.0 * (double) (spectrumB[(size_t) tau] * inverseScale));
        difference[(size_t) tau] = d;
        runningSum += d;
        cmnd[(size_t) tau] = runningSum > 0.0 ? (float) (d * tau / runningSum) : 1.0f;
    }

    const int tauMin = std::max (2, (int) std::floor (minPeriod));
    const int tauMax = std::min (lastLag - 1, (int) std::ceil (maxPeriod));

    if (tauMax <= tauMin)
        return result;

    // Absolute threshold: the first dip below the threshold wins (this is what keeps YIN from
    // choosing a sub-harmonic), then walk down to the bottom of that dip.
    int best = -1;

    for (int tau = tauMin; tau <= tauMax; ++tau)
    {
        if (cmnd[(size_t) tau] < threshold)
        {
            while (tau + 1 <= tauMax && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau])
                ++tau;

            best = tau;
            break;
        }
    }

    if (best < 0)
    {
        best = tauMin;

        for (int tau = tauMin + 1; tau <= tauMax; ++tau)
            if (cmnd[(size_t) tau] < cmnd[(size_t) best])
                best = tau;
    }

    // A periodic signal is also periodic at every multiple of its period, so breath, room sound or
    // other sources often push the pick to 2x-4x the period (the note drops an octave or more). If a
    // whole fraction of the pick still has a clear dip, the shortest such period is the voice. Clear
    // means within 0.2 of the pick, and also relative to it: next to a clean fundamental, a breathy
    // voice's third harmonic can dip within 0.2 of it too. Where the voice was already at that period,
    // the loose test stands: bleed from a lower note makes the pick land on a common multiple of the
    // two periods, much deeper than the voice's own dip.
    auto dipNear = [&] (float period)
    {
        const int from = std::max (tauMin, (int) std::floor (0.94f * period));
        const int to = std::min (tauMax, (int) std::ceil (1.06f * period));
        int dip = -1;

        for (int tau = from; tau <= to; ++tau)
            if (dip < 0 || cmnd[(size_t) tau] < cmnd[(size_t) dip])
                dip = tau;

        const bool tracked = previousPeriod > 0.0f && std::abs ((float) dip - previousPeriod) < 0.06f * previousPeriod;
        const float limit = cmnd[(size_t) best] + (tracked ? 0.2f : std::min (0.2f, cmnd[(size_t) best] + 0.05f));
        const bool clear = dip > tauMin && dip < tauMax && cmnd[(size_t) dip] < limit
                           && cmnd[(size_t) dip] <= cmnd[(size_t) dip - 1] && cmnd[(size_t) dip] <= cmnd[(size_t) dip + 1];
        return clear ? dip : -1;
    };

    const int picked = best;

    for (int k = 4; k >= 2; --k)
    {
        if (const int dip = dipNear ((float) picked / (float) k); dip > 0)
        {
            best = dip;
            break;
        }
    }

    // Parabolic interpolation on the raw difference function for sub-sample accuracy.
    auto refine = [this] (int tau)
    {
        const double a = difference[(size_t) tau - 1], b = difference[(size_t) tau], c = difference[(size_t) tau + 1];
        const double curvature = a - 2.0 * b + c;
        return (float) tau + (curvature > 0.0 ? (float) std::clamp (0.5 * (a - c) / curvature, -0.5, 0.5) : 0.0f);
    };

    result.period = refine (best);
    result.aperiodicity = cmnd[(size_t) best];

    // The caller judges continuity (dropouts, octave jumps) on its own terms, so also report the
    // deepest dip next to the tracked period.
    if (previousPeriod > 0.0f)
    {
        const int from = std::max (tauMin + 1, (int) std::floor (0.94f * previousPeriod));
        const int to = std::min (tauMax - 1, (int) std::ceil (1.06f * previousPeriod));
        int dip = -1;

        for (int tau = from; tau <= to; ++tau)
            if (cmnd[(size_t) tau] <= cmnd[(size_t) tau - 1] && cmnd[(size_t) tau] <= cmnd[(size_t) tau + 1]
                && (dip < 0 || cmnd[(size_t) tau] < cmnd[(size_t) dip]))
                dip = tau;

        if (dip > 0)
        {
            result.trackedPeriod = refine (dip);
            result.trackedAperiodicity = cmnd[(size_t) dip];
        }
    }

    return result;
}
} // namespace snaptune
