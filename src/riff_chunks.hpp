// Byte-exact WAV chunk builders (tag + 4-byte LE size + payload), shared by
// exportWavSmpl (wav_io.cpp, embedded straight into the RIFF chunk list) and
// exportFlac (flac_io.cpp, each chunk wrapped in a FLAC APPLICATION block
// tagged "riff" -- the same convention `flac --keep-foreign-metadata` uses
// and foreign_metadata.cpp's scanFlacMetadata already understands).
#pragma once

#include <cstdint>
#include <vector>

namespace riffchunks {

// 'smpl' chunk: loop points (if any), unity note, and pitch fraction. Same
// layout this project's own importer reads back (wav_io.cpp's
// SFC_GET_INSTRUMENT path / foreign_metadata.cpp's parseWavSmpl) and what
// most samplers understand. loopStart<0 or loopEnd<=loopStart => no loop
// entry (root note/tune only, same as exportWavSmpl's prior inline version).
// loopType is the smpl spec's own loop-type field (only meaningful when
// there IS a loop): 0 = forward, 1 = alternating/ping-pong, 2 = backward --
// this project's export dialogs only expose 0/1 (see exportWavSmpl).
std::vector<uint8_t> buildSmpl(double sampleRate, int64_t loopStart, int64_t loopEnd,
                               int rootNote, float tuneCents, int loopType = 0);

// 'cue ' chunk (two points, IDs 1/2: loop start/end in the project's own
// exclusive-end convention) plus the matching 'LIST'/'adtl' chunk naming
// them "LoopStart"/"LoopEnd" via 'labl' sub-chunks. Broader compatibility
// than smpl alone for software that reads cue markers instead (e.g. Unreal
// Engine's Sound Wave cue points). Both vectors are empty when there's no
// loop.
struct CueLoopChunks {
    std::vector<uint8_t> cue, list;
};
CueLoopChunks buildCueLoop(int64_t loopStart, int64_t loopEnd);

} // namespace riffchunks
