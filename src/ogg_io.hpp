#pragma once

#include <string>

#include "audio_data.hpp"

// Exports Ogg Vorbis (lossy). quality is vorbisenc's own VBR base_quality,
// clamped here to [0,1] (the export dialog's slider range) since Vorbis
// has no bit-depth concept. Loop points are stored as Vorbis comments (in
// samples) -- the de facto convention read by game engines/audio
// middleware (Unreal, Wwise, RPG Maker, ffmpeg, foobar2000...): LOOPSTART
// always, paired with either LOOPLENGTH (useLoopEndTag=false, the more
// common pairing) or LOOPEND (useLoopEndTag=true) -- both conventions
// exist in the wild, so the export dialog lets the user pick whichever
// their target tool expects; readOggLoopTags below understands both
// regardless of which one wrote the file. Unlike exportWavSmpl/exportFlac
// there's no root note/tune fraction or cue chunk: Vorbis comments are
// plain tags, not RIFF-style chunks, and the exact loop sample INDEX
// survives untouched even though the waveform at that position drifts
// slightly from the source (lossy compression).
bool exportOgg(const std::string& path, const AudioData& d, float quality,
               int64_t loopStart, int64_t loopEnd, bool useLoopEndTag,
               std::string& errorOut);

// Reads LOOPSTART plus whichever of LOOPLENGTH/LOOPEND is present (both
// checked regardless of which one exportOgg would have written for a given
// file -- see above). loopStart/loopEnd come back in the project's own
// exclusive-end convention. Returns false if no valid pair was found (or
// the file isn't a readable Ogg/Vorbis stream).
bool readOggLoopTags(const std::string& path, int64_t& loopStart, int64_t& loopEnd);
