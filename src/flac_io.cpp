#include "flac_io.hpp"

#include <sndfile.h>
#include <FLAC/metadata.h>

#include <cmath>
#include <cstring>

#include "riff_chunks.hpp"

namespace {

FLAC__StreamMetadata* makeApplicationBlock(const std::vector<uint8_t>& raw) {
    FLAC__StreamMetadata* app = FLAC__metadata_object_new(FLAC__METADATA_TYPE_APPLICATION);
    memcpy(app->data.application.id, "riff", 4);
    FLAC__metadata_object_application_set_data(
        app, const_cast<FLAC__byte*>(raw.data()), (uint32_t)raw.size(), /*copy=*/true);
    return app;
}

} // namespace

bool exportFlac(const std::string& path, const AudioData& d, int bitDepth,
                int64_t loopStart, int64_t loopEnd, int rootNote,
                float tuneCents, std::string& errorOut, bool* scaledDown,
                bool includeCue) {
    if (d.frames() <= 0) {
        errorOut = "No audio to export";
        return false;
    }
    if (bitDepth != 16 && bitDepth != 24) {
        errorOut = "Unsupported bit depth for FLAC (16/24)";
        return false;
    }
    const int nch = d.channels();
    const int64_t n = d.frames();

    // Same peak-scaling clip-avoidance as exportWavSmpl's integer paths:
    // scale the WHOLE file down instead of clipping individual samples.
    const float ceiling = bitDepth == 24 ? kInt24Max : kInt16Max;
    float scale = 1.0f;
    float peak = d.peakAbs();
    if (peak > ceiling) scale = ceiling / peak;
    if (scaledDown) *scaledDown = (scale != 1.0f);

    SF_INFO info{};
    info.samplerate = (int)std::lround(d.sampleRate);
    info.channels = nch;
    info.format = SF_FORMAT_FLAC | (bitDepth == 24 ? SF_FORMAT_PCM_24 : SF_FORMAT_PCM_16);

    SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!f) {
        errorOut = sf_strerror(nullptr);
        return false;
    }

    std::vector<float> inter((size_t)n * nch);
    for (int64_t i = 0; i < n; ++i)
        for (int c = 0; c < nch; ++c)
            inter[(size_t)i * nch + c] = d.ch[c][(size_t)i] * scale;
    sf_count_t wrote = sf_writef_float(f, inter.data(), n);
    sf_close(f);
    if (wrote != n) {
        errorOut = "Disk write error";
        return false;
    }

    // ---- embed smpl + cue/labl as FLAC APPLICATION blocks (appid "riff"),
    // the same convention `flac --keep-foreign-metadata` uses and this
    // project's own importer already understands (foreign_metadata.cpp) ----
    std::vector<uint8_t> smpl =
        riffchunks::buildSmpl(d.sampleRate, loopStart, loopEnd, rootNote, tuneCents);
    riffchunks::CueLoopChunks cue =
        includeCue ? riffchunks::buildCueLoop(loopStart, loopEnd) : riffchunks::CueLoopChunks{};

    FLAC__Metadata_Chain* chain = FLAC__metadata_chain_new();
    if (!chain || !FLAC__metadata_chain_read(chain, path.c_str())) {
        errorOut = "Audio exported, but could not reopen the FLAC file to embed loop metadata";
        if (chain) FLAC__metadata_chain_delete(chain);
        return false;
    }
    FLAC__Metadata_Iterator* it = FLAC__metadata_iterator_new();
    FLAC__metadata_iterator_init(it, chain);
    while (FLAC__metadata_iterator_next(it)) {} // seek to the last existing block

    bool inserted = FLAC__metadata_iterator_insert_block_after(it, makeApplicationBlock(smpl));
    if (inserted && !cue.cue.empty()) {
        inserted = FLAC__metadata_iterator_insert_block_after(it, makeApplicationBlock(cue.cue));
        if (inserted)
            inserted = FLAC__metadata_iterator_insert_block_after(it, makeApplicationBlock(cue.list));
    }
    FLAC__metadata_iterator_delete(it);

    bool metaOk = inserted &&
                  FLAC__metadata_chain_write(chain, /*use_padding=*/true, /*preserve_file_stats=*/false);
    FLAC__metadata_chain_delete(chain);
    if (!metaOk) {
        errorOut = "Audio exported, but embedding loop metadata failed";
        return false;
    }
    return true;
}
