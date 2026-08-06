// Fallback instrument-chunk scanner, independent of libsndfile.
//
// Covers three cases libsndfile's SFC_GET_INSTRUMENT doesn't (fully) get:
//  - WAV 'smpl' chunk (root note, fine tune, loop) -- libsndfile already
//    reads this reliably; this is mostly here to share code with the FLAC
//    case below, but doubles as a cheap safety net.
//  - AIFF 'MARK'+'INST' chunks, but matched by the loop markers'
//    conventional NAMES ("beg loop"/"end loop"/"LpBeg "/"LpEnd ", the
//    convention several samplers/DAWs use) instead of INST's sustain-loop
//    marker-ID cross-reference, which libsndfile requires and some
//    real-world files don't wire up correctly even though the marker names
//    make the intent obvious.
//  - FLAC files produced with `flac --keep-foreign-metadata`: the encoder
//    embeds byte-exact copies of the source WAV/AIFF file's non-audio
//    chunks inside APPLICATION metadata blocks (application ID "riff" for
//    a WAV origin, "aiff" for an AIFF origin) so the file can be decoded
//    back losslessly. libsndfile has no knowledge of this FLAC-specific
//    convention at all.
//
// Used by loadAudioFile() (wav_io.cpp) as a fallback/supplement when
// libsndfile's own instrument reading comes up empty or incomplete.
#pragma once

#include <cstdint>
#include <string>

namespace foreignmeta {

struct InstrumentInfo {
    bool hasNote = false;
    int rootNote = 60;
    float tuneCents = 0.f;
    bool hasLoop = false;
    int64_t loopStart = -1, loopEnd = -1; // frames, end EXCLUSIVE (project convention)
};

// Returns true if anything was found (hasNote and/or hasLoop may each be
// independently true/false). Never throws; false on any I/O/parse issue or
// if the file isn't a WAV/AIFF/FLAC this scanner recognizes.
bool scan(const std::string& path, InstrumentInfo& out);

} // namespace foreignmeta
