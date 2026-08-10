#include "ogg_io.hpp"

#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
// Fixed serial number: this writer only ever produces a single logical
// stream per file, so uniqueness only matters within that one file (it
// always does, trivially) -- no need for randomness.
constexpr int kSerialNo = 1;
} // namespace

bool exportOgg(const std::string& path, const AudioData& d, float quality,
               int64_t loopStart, int64_t loopEnd, bool useLoopEndTag,
               std::string& errorOut) {
    if (d.frames() <= 0) {
        errorOut = "No audio to export";
        return false;
    }
    const int nch = d.channels();
    const int64_t n = d.frames();
    const long sr = std::lround(d.sampleRate);
    quality = std::min(1.f, std::max(0.f, quality));

    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, nch, sr, quality) != 0) {
        errorOut = "Vorbis encoder initialization failed";
        vorbis_info_clear(&vi);
        return false;
    }

    vorbis_comment vc;
    vorbis_comment_init(&vc);
    const bool hasLoop = loopStart >= 0 && loopEnd > loopStart;
    if (hasLoop) {
        vorbis_comment_add_tag(&vc, "LOOPSTART", std::to_string(loopStart).c_str());
        if (useLoopEndTag)
            vorbis_comment_add_tag(&vc, "LOOPEND", std::to_string(loopEnd).c_str());
        else
            vorbis_comment_add_tag(&vc, "LOOPLENGTH", std::to_string(loopEnd - loopStart).c_str());
    }

    vorbis_dsp_state vd;
    if (vorbis_analysis_init(&vd, &vi) != 0) {
        errorOut = "Vorbis analysis initialization failed";
        vorbis_comment_clear(&vc);
        vorbis_info_clear(&vi);
        return false;
    }
    vorbis_block vb;
    vorbis_block_init(&vd, &vb);

    ogg_stream_state os;
    ogg_stream_init(&os, kSerialNo);

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        errorOut = "Could not open for writing: " + path;
        ogg_stream_clear(&os);
        vorbis_block_clear(&vb);
        vorbis_dsp_clear(&vd);
        vorbis_comment_clear(&vc);
        vorbis_info_clear(&vi);
        return false;
    }

    {
        ogg_packet header, headerComm, headerCode;
        vorbis_analysis_headerout(&vd, &vc, &header, &headerComm, &headerCode);
        ogg_stream_packetin(&os, &header);
        ogg_stream_packetin(&os, &headerComm);
        ogg_stream_packetin(&os, &headerCode);

        ogg_page og;
        while (ogg_stream_flush(&os, &og)) {
            fwrite(og.header, 1, (size_t)og.header_len, f);
            fwrite(og.body, 1, (size_t)og.body_len, f);
        }
    }

    const int64_t kBlockFrames = 1024;
    int64_t pos = 0;
    bool eos = false;
    bool writeError = false;
    while (!eos) {
        const int64_t thisBlock = std::min(kBlockFrames, n - pos);
        if (thisBlock > 0) {
            float** buffer = vorbis_analysis_buffer(&vd, (int)kBlockFrames);
            for (int64_t i = 0; i < thisBlock; ++i)
                for (int c = 0; c < nch; ++c)
                    buffer[c][i] = d.ch[c][(size_t)(pos + i)];
            vorbis_analysis_wrote(&vd, (int)thisBlock);
            pos += thisBlock;
        } else {
            vorbis_analysis_wrote(&vd, 0); // signals end of stream
        }

        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);

            ogg_packet op;
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);
                ogg_page og;
                while (ogg_stream_pageout(&os, &og)) {
                    size_t wh = fwrite(og.header, 1, (size_t)og.header_len, f);
                    size_t wb = fwrite(og.body, 1, (size_t)og.body_len, f);
                    if (wh != (size_t)og.header_len || wb != (size_t)og.body_len)
                        writeError = true;
                    if (ogg_page_eos(&og)) eos = true;
                }
            }
        }
    }

    fclose(f);
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);

    if (writeError) {
        errorOut = "Disk write error";
        return false;
    }
    return true;
}

bool readOggLoopTags(const std::string& path, int64_t& loopStart, int64_t& loopEnd) {
    OggVorbis_File vf;
    if (ov_fopen(path.c_str(), &vf) != 0) return false;

    vorbis_comment* vc = ov_comment(&vf, -1);
    bool ok = false;
    if (vc) {
        const char* startStr = vorbis_comment_query(vc, "LOOPSTART", 0);
        const char* lengthStr = vorbis_comment_query(vc, "LOOPLENGTH", 0);
        const char* endStr = vorbis_comment_query(vc, "LOOPEND", 0);
        if (startStr && (lengthStr || endStr)) {
            int64_t ls = std::strtoll(startStr, nullptr, 10);
            int64_t le = lengthStr ? ls + std::strtoll(lengthStr, nullptr, 10)
                                    : std::strtoll(endStr, nullptr, 10);
            if (ls >= 0 && le > ls) {
                loopStart = ls;
                loopEnd = le;
                ok = true;
            }
        }
    }
    ov_clear(&vf);
    return ok;
}
