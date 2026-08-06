#include "loopauditioneer_finder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace loopauditioneerdsp {
namespace {

// Port of FileHandling::SeparateStrongestChannel: picks the channel with
// the highest RMS (mono files just have channel 0).
int strongestChannel(const AudioData& audio) {
    if (audio.channels() <= 1) return 0;
    int best = 0;
    double bestRms = -1.0;
    for (int c = 0; c < audio.channels(); ++c) {
        double sum = 0.0;
        const auto& v = audio.ch[c];
        for (float s : v) sum += (double)s * (double)s;
        double rms = v.empty() ? 0.0 : std::sqrt(sum / (double)v.size());
        if (rms > bestRms) {
            bestRms = rms;
            best = c;
        }
    }
    return best;
}

// Port of FileHandling::CalculateSustainStartAndEnd, scoped to
// [rangeStart,rangeEnd) instead of the whole file (NeoLooper already lets
// the caller narrow the range via the selection). RMS in 20ms windows
// (samplerate/50, same as the original); heuristically finds where the
// signal settles after an attack and, if present, where a release begins.
std::pair<int64_t, int64_t> detectSustainSection(const std::vector<float>& ch, double sampleRate,
                                                  int64_t rangeStart, int64_t rangeEnd) {
    const int64_t n = rangeEnd - rangeStart;
    int64_t windowSize = (int64_t)(sampleRate / 50.0);
    if (windowSize < 1) windowSize = 1;
    if (windowSize > n - 1) windowSize = n - 1;
    if (windowSize < 1) return {rangeStart, rangeEnd}; // too short to analyze; use as-is

    std::vector<double> rms;
    double rmsSum = 0.0;
    for (int64_t idx = 0; idx + windowSize <= n; idx += windowSize) {
        double total = 0.0;
        for (int64_t j = idx; j < idx + windowSize; ++j) {
            double v = (double)ch[(size_t)(rangeStart + j)];
            total += v * v;
        }
        double r = std::sqrt(total / (double)windowSize);
        rms.push_back(r);
        rmsSum += r;
    }
    if (rms.empty()) return {rangeStart, rangeEnd};
    double rmsAvg = rmsSum / (double)rms.size();

    int64_t sustainStartLocal = 0;
    if (rms[0] > rmsAvg * 1.75) {
        // Loud from sample 0: likely a release-only recording, no attack to skip.
        sustainStartLocal = 0;
    } else {
        size_t firstAbove = 0;
        bool found = false;
        for (size_t i = 0; i < rms.size(); ++i) {
            if (rms[i] > rmsAvg) {
                firstAbove = i;
                found = true;
                break;
            }
        }
        if (!found) firstAbove = 0;
        double startTime = (double)(firstAbove * (size_t)windowSize) / sampleRate;
        if (startTime < 0.300 && firstAbove < rms.size() - 2)
            sustainStartLocal = (int64_t)(firstAbove + 1) * windowSize;
        else
            sustainStartLocal = (int64_t)firstAbove * windowSize;
    }

    int64_t sustainEndLocal;
    size_t lastIdx = rms.size() - 1;
    if (rms[lastIdx] < rmsAvg * 0.75) {
        // The tail is quiet relative to the average: there's some kind of release.
        if (sustainStartLocal != 0) {
            // A real attack was found -- scan backward for the last window still above average.
            size_t lastAbove = lastIdx;
            bool found = false;
            for (size_t i = rms.size(); i-- > 0;) {
                if (rms[i] > rmsAvg) {
                    lastAbove = i;
                    found = true;
                    break;
                }
            }
            sustainEndLocal = found ? (int64_t)lastAbove * windowSize : n - 1;
        } else {
            // No attack detected -- this looks like a release-only file: walk
            // forward while it's still loud and not dropping.
            size_t lastGood = 0;
            bool any = false;
            for (size_t i = 0; i + 2 < rms.size(); ++i) {
                if (rms[i + 1] * 1.2 > rms[i] && rms[i] > rmsAvg * 2.0) {
                    lastGood = i;
                    any = true;
                } else {
                    break;
                }
            }
            if (any)
                sustainEndLocal = (int64_t)lastGood * windowSize;
            else if (rms.size() > 1 && rms[0] > 0.0)
                sustainEndLocal = (int64_t)((rms[1] / rms[0]) * (double)windowSize);
            else
                sustainEndLocal = windowSize;
        }
    } else {
        // No significant drop at the end: treat it as sustain all the way through.
        sustainEndLocal = n - 1;
    }

    int64_t s = rangeStart + sustainStartLocal;
    int64_t e = rangeStart + sustainEndLocal;
    if (e <= s) e = std::min(rangeEnd, s + windowSize);
    return {s, e};
}

// Port of FileHandling::CalculateLoopQuality + GetLoopQuality: compares the
// 5 samples ending at (start-1) against the 5 samples ending at 'end'
// (end-4..end), per channel, and returns the WORST (max) channel's summed
// absolute difference. Lower = more alike.
double loopQuality(const AudioData& audio, int64_t start, int64_t end) {
    if (start < 5 || end <= start) return std::numeric_limits<double>::max();
    double worst = 0.0;
    for (int c = 0; c < audio.channels(); ++c) {
        const auto& v = audio.ch[c];
        double diff = 0.0;
        for (int j = 0; j < 5; ++j) {
            double a = (double)v[(size_t)(start - 5 + j)];
            double b = (double)v[(size_t)(end - 4 + j)];
            diff += std::fabs(a - b);
        }
        worst = std::max(worst, diff);
    }
    return worst;
}

} // namespace

Result findLoops(const AudioData& audio, int64_t searchStart, int64_t searchEnd, const Params& params) {
    Result res;
    if (audio.frames() <= 0 || audio.channels() == 0) return res;
    searchStart = std::max<int64_t>(0, searchStart);
    searchEnd = std::min<int64_t>(audio.frames(), searchEnd);
    if (searchEnd - searchStart < 16) return res;

    const double sr = audio.sampleRate > 0.0 ? audio.sampleRate : 44100.0;
    const int strongest = strongestChannel(audio);
    const auto& ch = audio.ch[strongest];

    int64_t sustainStart, sustainEnd;
    if (params.autoSustain) {
        auto sec = detectSustainSection(ch, sr, searchStart, searchEnd);
        sustainStart = sec.first;
        sustainEnd = sec.second;
    } else {
        sustainStart = searchStart;
        sustainEnd = searchEnd;
    }

    // Safety margin: a real instrument typically needs ~100ms to settle
    // after the attack (port of AutoFindLoops' preamble adjustment).
    const int64_t hundredMs = (int64_t)(sr / 10.0);
    if (sustainStart < searchStart + hundredMs) {
        sustainStart = searchStart + hundredMs;
        if (sustainStart > sustainEnd) {
            if (sustainEnd + hundredMs < searchEnd)
                sustainEnd += hundredMs;
            else
                return res; // no room left to adjust; give up gracefully
        }
    }
    sustainStart = std::max<int64_t>(searchStart, std::min(sustainStart, searchEnd - 2));
    sustainEnd = std::max<int64_t>(sustainStart + 1, std::min(sustainEnd, searchEnd));
    res.sustainStart = sustainStart;
    res.sustainEnd = sustainEnd;
    if (sustainEnd - sustainStart < 16) return res;

    // --- candidate collection: low-derivative points within the sustain section ---
    double maxDerivative = 0.0;
    for (int64_t i = sustainStart; i < sustainEnd - 1; ++i)
        maxDerivative = std::max(maxDerivative, (double)std::fabs(ch[(size_t)(i + 1)] - ch[(size_t)i]));

    const double derivThresh = maxDerivative * params.derivativeThreshold;
    std::vector<int64_t> allCandidates;
    for (int64_t i = sustainStart; i < sustainEnd - 1; ++i) {
        double d = std::fabs((double)ch[(size_t)(i + 1)] - (double)ch[(size_t)i]);
        if (d < derivThresh) allCandidates.push_back(i);
    }
    if (allCandidates.empty()) return res;

    const int hardCap = std::min(std::max(1, params.maxCandidates), kHardCandidateCap);
    std::vector<int64_t> candidates;
    if ((int64_t)allCandidates.size() > hardCap) {
        double increment = (double)allCandidates.size() / (double)hardCap;
        for (int i = 0; i < hardCap; ++i) {
            int64_t idx = (int64_t)((double)i * increment);
            if (idx < (int64_t)allCandidates.size() - 1) candidates.push_back(allCandidates[(size_t)idx]);
        }
    } else {
        candidates = std::move(allCandidates);
    }
    if (candidates.size() < 2) return res;

    // --- matching: compare candidate pairs, longest loops tried first ---
    const int64_t minLoopSamples = (int64_t)std::llround(params.minLoopDurationSec * sr);
    const int64_t minDistSamples = (int64_t)std::llround(params.minDistanceBetweenLoopsSec * sr);
    const size_t poolCap = (size_t)std::max(1, params.loopsToReturn) * (size_t)std::max(1, params.maxLoopsMultiple);

    std::vector<Candidate> found;
    for (size_t i = 0; i + 1 < candidates.size(); ++i) {
        int64_t s = candidates[i];
        if (s < 5) continue;
        if (!found.empty() && !params.useBruteForce && (s - found.back().start) < minDistSamples) continue;

        for (size_t jj = candidates.size(); jj-- > 0;) {
            if (jj <= i + 1) break;
            int64_t e = candidates[jj];
            if (e - s < minLoopSamples) continue;
            double q = loopQuality(audio, s, e);
            if (q <= params.qualityFactor) {
                found.push_back({s, e, q});
                break;
            }
        }
        if (!params.useBruteForce && found.size() > poolCap - 1) break;
    }
    if (found.empty()) return res;

    std::sort(found.begin(), found.end(),
             [](const Candidate& a, const Candidate& b) { return a.quality < b.quality; });

    // --- final selection: every returned loop must overlap at least one other ---
    std::vector<Candidate> loops;
    std::vector<bool> used(found.size(), false);
    loops.push_back(found[0]);
    used[0] = true;
    bool addedLoop = true;
    while ((int)loops.size() < std::max(1, params.loopsToReturn) && addedLoop) {
        addedLoop = false;
        for (size_t i = 0; i < found.size(); ++i) {
            if (used[i]) continue;
            bool overlaps = false;
            bool tooClose = true;
            for (const auto& L : loops) {
                if ((found[i].start > L.start && found[i].start < L.end) ||
                    (found[i].end < L.end && found[i].end > L.start)) {
                    overlaps = true;
                    if (params.useBruteForce) {
                        bool farFromAll = true;
                        for (const auto& K : loops) {
                            if (!(std::llabs(found[i].start - K.start) > minDistSamples &&
                                 std::llabs(found[i].end - K.end) > minDistSamples)) {
                                farFromAll = false;
                                break;
                            }
                        }
                        tooClose = !farFromAll;
                    } else {
                        bool farFromAllEnds = true;
                        for (const auto& K : loops) {
                            if (!(std::llabs(found[i].end - K.end) > minDistSamples)) {
                                farFromAllEnds = false;
                                break;
                            }
                        }
                        tooClose = !farFromAllEnds;
                    }
                }
            }
            if (overlaps && !tooClose) {
                loops.push_back(found[i]);
                used[i] = true;
                addedLoop = true;
                break;
            }
        }
    }

    res.ok = true;
    res.loops = std::move(loops);
    return res;
}

} // namespace loopauditioneerdsp
