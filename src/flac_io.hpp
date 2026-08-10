#pragma once

#include <string>

#include "audio_data.hpp"

// Exports FLAC (lossless). Audio is written via libsndfile (same as
// wav_io.cpp's read path); loop points + unity note + pitch fraction and
// the cue-point equivalent then get embedded as FLAC APPLICATION metadata
// blocks (application ID "riff", each wrapping one WAV chunk byte-exact --
// 'smpl', 'cue ', 'LIST') via libFLAC's metadata-chain API. Same convention
// `flac --keep-foreign-metadata` uses and this project's own importer
// already understands (foreign_metadata.cpp's scanFlacMetadata). FLAC has
// no float subtype, so bitDepth is 16 or 24 only -- same peak-scaling
// clip-avoidance as exportWavSmpl's integer paths. includeCue: also embed
// the 'cue '/'LIST'/'labl' loop markers alongside 'smpl' -- see
// exportWavSmpl's includeCue for the rationale.
bool exportFlac(const std::string& path, const AudioData& d, int bitDepth,
                int64_t loopStart, int64_t loopEnd, int rootNote,
                float tuneCents, std::string& errorOut, bool* scaledDown = nullptr,
                bool includeCue = true);
