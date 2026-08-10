#pragma once

#include <string>

#include "audio_data.hpp"

// Exports AIFF (lossless, big-endian PCM), writing 'COMM'/'SSND' audio plus
// 'MARK'+'INST' loop/root-note/tune metadata. 'INST's sustainLoop (and
// releaseLoop, mirrored to the same pair) cross-reference two 'MARK'
// marker IDs -- the spec-compliant mechanism real-world software (Kontakt,
// Awave Studio, ExtremeSampleConverter, libsndfile's own
// SFC_GET_INSTRUMENT...) actually relies on to detect the loop; the
// markers are ALSO named ("beg loop"/"end loop" or "LpBeg "/"LpEnd ", per
// lpMarkerNames) matching foreign_metadata.cpp's own NAME-based fallback
// scan, exercised only for AIFF files that lack a proper ID
// cross-reference. The end marker position is EXCLUSIVE (unlike the WAV
// smpl chunk's inclusive convention) -- verified against a real,
// multi-program-compatible reference AIFF file and against libsndfile's
// own AIFF reader, which passes the position straight through with no +1
// adjustment. lpMarkerNames picks the marker name pair: false = "beg
// loop"/"end loop" (more common in modern tools), true = "LpBeg "/"LpEnd "
// (padded to 6 chars, the classic SampleCell/older-sampler convention).
// AIFF's INST detune is a single signed byte (integer cents only, official
// range -50..+50 -- no fractional pitch encoding like WAV/FLAC's smpl
// pitch fraction), so tuneCents is rounded to the nearest whole cent.
// Plain (uncompressed) AIFF has no float subtype, so bitDepth is 16 or 24
// only -- same peak-scaling clip-avoidance as exportWavSmpl's integer
// paths. loopType selects sustainLoop/releaseLoop's playMode per the AIFF
// spec: 0 = forward (playMode 1), 1 = alternating/ping-pong (playMode 2) --
// lets a sampler that supports native ping-pong playback loop the file
// that way without NeoLooper having baked the alternation into the audio
// itself (same idea as exportWavSmpl's loopType).
bool exportAiff(const std::string& path, const AudioData& d, int bitDepth,
                int64_t loopStart, int64_t loopEnd, int rootNote,
                float tuneCents, std::string& errorOut, bool* scaledDown = nullptr,
                bool lpMarkerNames = false, int loopType = 0);
