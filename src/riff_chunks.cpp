#include "riff_chunks.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace riffchunks {
namespace {
void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xff);
    v.push_back((x >> 8) & 0xff);
    v.push_back((x >> 16) & 0xff);
    v.push_back((x >> 24) & 0xff);
}
void putTag(std::vector<uint8_t>& v, const char* t) {
    v.insert(v.end(), t, t + 4);
}
} // namespace

std::vector<uint8_t> buildSmpl(double sampleRate, int64_t loopStart, int64_t loopEnd,
                               int rootNote, float tuneCents, int loopType) {
    // MIDIPitchFraction is the fraction of a semitone UPWARD from the unity
    // note, in units of 1/2^32 of a semitone. Negative cents are encoded by
    // lowering the unity note by one semitone.
    int unity = rootNote;
    double cents = (double)tuneCents;
    if (cents < 0) {
        unity -= 1;
        cents += 100.0;
    }
    unity = std::min(127, std::max(0, unity));
    uint32_t pitchFrac = (uint32_t)std::llround((cents / 100.0) * 4294967296.0);
    if (cents >= 100.0) pitchFrac = 0xFFFFFFFFu;

    const bool hasLoop = loopStart >= 0 && loopEnd > loopStart;
    const uint32_t sr = (uint32_t)std::lround(sampleRate);

    std::vector<uint8_t> out;
    putTag(out, "smpl");
    put32(out, 36 + (hasLoop ? 24 : 0));
    put32(out, 0); // manufacturer
    put32(out, 0); // product
    put32(out, sr ? (uint32_t)(1000000000ull / sr) : 0); // sample period ns
    put32(out, (uint32_t)unity);
    put32(out, pitchFrac);
    put32(out, 0); // SMPTE format
    put32(out, 0); // SMPTE offset
    put32(out, hasLoop ? 1 : 0);
    put32(out, 0); // sampler data
    if (hasLoop) {
        put32(out, 0);                       // cue id
        put32(out, (uint32_t)loopType);       // 0 = forward, 1 = ping-pong
        put32(out, (uint32_t)loopStart);
        put32(out, (uint32_t)(loopEnd - 1)); // end inclusive in smpl
        put32(out, 0);                       // fraction
        put32(out, 0);                       // infinite
    }
    return out;
}

CueLoopChunks buildCueLoop(int64_t loopStart, int64_t loopEnd) {
    CueLoopChunks r;
    if (loopStart < 0 || loopEnd <= loopStart) return r;

    // 'cue ' chunk: two points naming the loop boundaries. Position/
    // dwSampleOffset use this project's own exclusive-end convention for
    // point 2 (the wrap-around frame, matching Document::loopEnd() directly
    // rather than the smpl chunk's inclusive-end quirk).
    std::vector<uint8_t> cuePayload;
    put32(cuePayload, 2); // dwCuePoints
    auto point = [&](uint32_t id, uint32_t pos) {
        put32(cuePayload, id);   // dwName
        put32(cuePayload, pos);  // dwPosition
        putTag(cuePayload, "data");
        put32(cuePayload, 0);   // dwChunkStart
        put32(cuePayload, 0);   // dwBlockStart
        put32(cuePayload, pos); // dwSampleOffset
    };
    point(1, (uint32_t)loopStart);
    point(2, (uint32_t)loopEnd);
    putTag(r.cue, "cue ");
    put32(r.cue, (uint32_t)cuePayload.size());
    r.cue.insert(r.cue.end(), cuePayload.begin(), cuePayload.end());
    if (cuePayload.size() & 1) r.cue.push_back(0);

    // 'LIST'/'adtl' with 'labl' names for each cue point above.
    std::vector<uint8_t> listPayload;
    putTag(listPayload, "adtl");
    auto label = [&](uint32_t id, const char* text) {
        std::vector<uint8_t> lp;
        put32(lp, id);
        lp.insert(lp.end(), text, text + strlen(text) + 1); // incl. null terminator
        putTag(listPayload, "labl");
        put32(listPayload, (uint32_t)lp.size());
        listPayload.insert(listPayload.end(), lp.begin(), lp.end());
        if (lp.size() & 1) listPayload.push_back(0);
    };
    label(1, "LoopStart");
    label(2, "LoopEnd");
    putTag(r.list, "LIST");
    put32(r.list, (uint32_t)listPayload.size());
    r.list.insert(r.list.end(), listPayload.begin(), listPayload.end());
    if (listPayload.size() & 1) r.list.push_back(0);

    return r;
}

} // namespace riffchunks
