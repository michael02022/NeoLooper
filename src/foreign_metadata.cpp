#include "foreign_metadata.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace foreignmeta {
namespace {

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
uint16_t be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

bool tagIs(const uint8_t* p, const char* tag) { return memcmp(p, tag, 4) == 0; }

// WAV 'smpl' chunk (little-endian): manufacturer, product, samplePeriod,
// midiUnityNote, midiPitchFraction, smpteFormat, smpteOffset, numSampleLoops,
// samplerData, then per loop: cuePointId, type, start, end(inclusive),
// fraction, playCount.
void parseWavSmpl(const std::vector<uint8_t>& p, InstrumentInfo& out) {
    if (p.size() < 36) return;
    out.hasNote = true;
    out.rootNote = std::clamp((int)le32(&p[12]), 0, 127);
    // midiPitchFraction: fraction of a semitone upward, in units of 1/2^32
    // of a semitone -- inverse of exportWavSmpl's pitchFrac computation.
    out.tuneCents = (float)((double)le32(&p[16]) / 4294967296.0 * 100.0);

    const uint32_t numLoops = le32(&p[28]);
    if (numLoops > 0 && p.size() >= 36 + 16) {
        const uint32_t start = le32(&p[36 + 8]);
        const uint32_t end = le32(&p[36 + 12]); // inclusive per spec
        out.hasLoop = true;
        out.loopStart = (int64_t)start;
        out.loopEnd = (int64_t)end + 1; // exclusive, project convention
    }
}

// AIFF pascal string: 1-byte length + that many chars, then a pad byte if
// the total (1+length) is odd. Advances 'off' past it.
bool parsePstring(const std::vector<uint8_t>& p, size_t& off, std::string& outStr) {
    if (off >= p.size()) return false;
    const uint8_t len = p[off];
    off += 1;
    if (off + len > p.size()) return false;
    outStr.assign((const char*)&p[off], len);
    off += len;
    if ((1 + (size_t)len) % 2 != 0) off += 1;
    return true;
}

// AIFF 'MARK' (marker table, matched by conventional loop-marker names,
// not by INST's marker-ID cross-reference -- see foreign_metadata.hpp) and
// 'INST' (root note + fine tune) chunks, big-endian.
void parseAiffMarkInst(const std::vector<uint8_t>& mark, const std::vector<uint8_t>& inst,
                       InstrumentInfo& out) {
    if (inst.size() >= 2) {
        out.hasNote = true;
        out.rootNote = std::clamp((int)inst[0], 0, 127);
        out.tuneCents = (float)(int8_t)inst[1]; // signed byte, cents
    }
    if (mark.size() < 2) return;

    const uint16_t numMarkers = be16(&mark[0]);
    size_t off = 2;
    int64_t start = -1, end = -1;
    for (uint16_t i = 0; i < numMarkers; ++i) {
        if (off + 6 > mark.size()) break;
        off += 2; // marker id, unused: matched by name below
        const uint32_t pos = be32(&mark[off]);
        off += 4;
        std::string name;
        if (!parsePstring(mark, off, name)) break;
        if (name == "beg loop" || name == "LpBeg ") start = (int64_t)pos;
        else if (name == "end loop" || name == "LpEnd ") end = (int64_t)pos;
    }
    if (start >= 0 && end > start) {
        out.hasLoop = true;
        out.loopStart = start;
        // Same inclusive-end assumption as the WAV smpl chunk (the most
        // common sampler convention for AIFF loop markers too): +1 to
        // match this project's exclusive-end convention. Unverified
        // against libsndfile's own AIFF convention (it wouldn't have
        // found this loop at all -- that's why this fallback runs).
        out.loopEnd = end + 1;
    }
}

// Scans a flat top-level chunk list (WAV right after the 12-byte RIFF/WAVE
// header, or AIFF right after the 12-byte FORM/AIFF header) for the chunks
// we care about. 'f' is positioned right after that outer header.
bool scanTopLevelChunks(FILE* f, bool bigEndian, InstrumentInfo& out) {
    std::vector<uint8_t> markPayload, instPayload;
    bool haveMark = false, haveInst = false;

    // Bounded so a malformed/truncated file can't loop forever.
    for (int guard = 0; guard < 100000; ++guard) {
        uint8_t tag[4], szb[4];
        if (fread(tag, 1, 4, f) != 4) break;
        if (fread(szb, 1, 4, f) != 4) break;
        const uint32_t size = bigEndian ? be32(szb) : le32(szb);

        const bool wantSmpl = !bigEndian && tagIs(tag, "smpl");
        const bool wantMark = bigEndian && tagIs(tag, "MARK");
        const bool wantInst = bigEndian && tagIs(tag, "INST");
        if (wantSmpl || wantMark || wantInst) {
            std::vector<uint8_t> payload((size_t)size);
            if (size > 0 && fread(payload.data(), 1, size, f) != size) break;
            if (wantSmpl) parseWavSmpl(payload, out);
            else if (wantMark) { markPayload = std::move(payload); haveMark = true; }
            else { instPayload = std::move(payload); haveInst = true; }
        } else {
            if (fseek(f, (long)size, SEEK_CUR) != 0) break;
        }
        if (size & 1) fseek(f, 1, SEEK_CUR); // chunk padding byte
    }
    if (haveMark && haveInst) parseAiffMarkInst(markPayload, instPayload, out);
    return out.hasNote || out.hasLoop;
}

// Scans a FLAC file's metadata blocks for APPLICATION blocks written by
// `flac --keep-foreign-metadata`: each one is a byte-exact copy of a
// single original WAV/AIFF chunk (tag + size + payload, endianness
// matching the origin format), tagged with application ID "riff" (WAV
// origin) or "aiff" (AIFF origin). 'f' is positioned right after the
// 4-byte "fLaC" magic.
bool scanFlacMetadata(FILE* f, InstrumentInfo& out) {
    std::vector<uint8_t> markPayload, instPayload;
    bool haveMark = false, haveInst = false;

    for (int guard = 0; guard < 100000; ++guard) {
        uint8_t hdr[4];
        if (fread(hdr, 1, 4, f) != 4) break;
        const bool last = (hdr[0] & 0x80) != 0;
        const int btype = hdr[0] & 0x7f;
        const uint32_t len = ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];

        if (btype == 2 /* APPLICATION */ && len >= 8) {
            uint8_t appid[4];
            if (fread(appid, 1, 4, f) != 4) break;
            const bool isRiff = tagIs(appid, "riff");
            const bool isAiff = tagIs(appid, "aiff");
            uint32_t remain = len - 4;
            if ((isRiff || isAiff) && remain >= 8) {
                uint8_t tag[4], szb[4];
                if (fread(tag, 1, 4, f) != 4) break;
                if (fread(szb, 1, 4, f) != 4) break;
                remain -= 8;
                const uint32_t csize = isRiff ? le32(szb) : be32(szb);
                if (csize <= remain) {
                    const bool wantSmpl = isRiff && tagIs(tag, "smpl");
                    const bool wantMark = isAiff && tagIs(tag, "MARK");
                    const bool wantInst = isAiff && tagIs(tag, "INST");
                    if (wantSmpl || wantMark || wantInst) {
                        std::vector<uint8_t> payload((size_t)csize);
                        if (csize > 0 && fread(payload.data(), 1, csize, f) != csize) break;
                        remain -= csize;
                        if (wantSmpl) parseWavSmpl(payload, out);
                        else if (wantMark) { markPayload = std::move(payload); haveMark = true; }
                        else { instPayload = std::move(payload); haveInst = true; }
                    }
                }
            }
            if (remain > 0 && fseek(f, (long)remain, SEEK_CUR) != 0) break;
        } else {
            if (fseek(f, (long)len, SEEK_CUR) != 0) break;
        }
        if (last) break;
    }
    if (haveMark && haveInst) parseAiffMarkInst(markPayload, instPayload, out);
    return out.hasNote || out.hasLoop;
}

} // namespace

bool scan(const std::string& path, InstrumentInfo& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    bool ok = false;
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) == 4) {
        if (tagIs(magic, "RIFF")) {
            fseek(f, 4, SEEK_CUR); // RIFF size
            uint8_t form[4];
            if (fread(form, 1, 4, f) == 4 && tagIs(form, "WAVE"))
                ok = scanTopLevelChunks(f, /*bigEndian=*/false, out);
        } else if (tagIs(magic, "FORM")) {
            fseek(f, 4, SEEK_CUR); // FORM size
            uint8_t form[4];
            if (fread(form, 1, 4, f) == 4 && (tagIs(form, "AIFF") || tagIs(form, "AIFC")))
                ok = scanTopLevelChunks(f, /*bigEndian=*/true, out);
        } else if (tagIs(magic, "fLaC")) {
            ok = scanFlacMetadata(f, out);
        }
    }
    fclose(f);
    return ok;
}

} // namespace foreignmeta
