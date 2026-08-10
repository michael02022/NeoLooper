#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "audio_data.hpp"

// Result of loading a file (WAV/AIFF/FLAC/OGG via libsndfile, MP3 via
// dr_mp3 -- see loadAudioFile's dispatch in wav_io.cpp).
struct LoadResult {
    std::shared_ptr<AudioData> data; // null on failure
    std::string error;
    // instrument metadata if the file carried it (smpl / inst chunk)
    bool hasInstrument = false;
    int rootNote = 60;
    float tuneCents = 0.f;
    int64_t loopStart = -1, loopEnd = -1;
};

LoadResult loadAudioFile(const std::string& path);

// Exports WAV writing the smpl chunk with loop, unity note and pitch
// fraction. bitDepth chooses the FILE's format (independent of the
// document's internal format): 16 or 24 = integer PCM, 32 = IEEE float.
// Heads up: not all sampler software supports 32-bit float WAV (e.g.
// Polyphone silently fails to import it) -- that's why depth is chosen by
// the user on save instead of being inherited from the document.
// If the document's peak amplitude exceeds what bitDepth can represent
// (e.g. exporting a hot 32-bit float document to 16-bit PCM), the WHOLE
// file is scaled down so the peak lands exactly on that format's ceiling
// instead of clipping every sample above it (same principle as
// Document::toggleBits). scaledDown, if non-null, reports whether that
// happened.
// loopStart/loopEnd in frames (end exclusive); loopStart<0 = no loop.
// tuneCents in [-99, 99].
bool exportWavSmpl(const std::string& path, const AudioData& d, int bitDepth,
                   int64_t loopStart, int64_t loopEnd, int rootNote,
                   float tuneCents, std::string& errorOut, bool* scaledDown = nullptr);
