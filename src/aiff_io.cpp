#include "aiff_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

void put16be(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x & 0xff));
}
void put32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x & 0xff));
}
void putTag(std::vector<uint8_t>& v, const char* t) {
    v.insert(v.end(), t, t + 4);
}
// AIFF pascal string: 1-byte length + chars, padded to even total (1+len)
// -- see foreign_metadata.cpp's parsePstring, which this mirrors exactly.
void putPString(std::vector<uint8_t>& v, const char* s) {
    size_t n = strlen(s);
    v.push_back((uint8_t)n);
    v.insert(v.end(), s, s + n);
    if ((1 + n) % 2 != 0) v.push_back(0);
}

// Classic public-domain 80-bit IEEE extended float encoder (as used by
// libsndfile/sox and the original Apple/SGI AIFF sample code) -- the only
// way AIFF's COMM chunk can express a sample rate; no standard C type
// covers this format.
void putIeeeExtended80(std::vector<uint8_t>& v, double num) {
    int sign = 0;
    int expon = 0;
    uint32_t hiMant = 0, loMant = 0;

    if (num < 0) {
        sign = 0x8000;
        num = -num;
    }
    if (num != 0) {
        double fMant = std::frexp(num, &expon);
        if (expon > 16384 || !(fMant < 1.0)) {
            expon = sign | 0x7FFF; // infinity/NaN
        } else {
            expon += 16382;
            if (expon < 0) {
                fMant = std::ldexp(fMant, expon);
                expon = 0;
            }
            expon |= sign;
            fMant = std::ldexp(fMant, 32);
            double fsMant = std::floor(fMant);
            hiMant = (uint32_t)fsMant;
            fMant = std::ldexp(fMant - fsMant, 32);
            fsMant = std::floor(fMant);
            loMant = (uint32_t)fsMant;
        }
    }
    put16be(v, (uint16_t)expon);
    put32be(v, hiMant);
    put32be(v, loMant);
}

} // namespace

bool exportAiff(const std::string& path, const AudioData& d, int bitDepth,
                int64_t loopStart, int64_t loopEnd, int rootNote,
                float tuneCents, std::string& errorOut, bool* scaledDown,
                bool lpMarkerNames, int loopType) {
    if (d.frames() <= 0) {
        errorOut = "No audio to export";
        return false;
    }
    if (bitDepth != 16 && bitDepth != 24) {
        errorOut = "Unsupported bit depth for AIFF (16/24)";
        return false;
    }
    const int nch = d.channels();
    const int64_t n = d.frames();
    const uint16_t bits = (uint16_t)bitDepth;

    // Same peak-scaling clip-avoidance as exportWavSmpl's integer paths:
    // scale the WHOLE file down instead of clipping individual samples.
    const float ceiling = bits == 24 ? kInt24Max : kInt16Max;
    float scale = 1.0f;
    float peak = d.peakAbs();
    if (peak > ceiling) scale = ceiling / peak;
    if (scaledDown) *scaledDown = (scale != 1.0f);

    const uint32_t blockAlign = (uint32_t)nch * bits / 8;
    const uint64_t dataBytes64 = (uint64_t)n * blockAlign;
    if (dataBytes64 > 0xFFFFFFFFull - 1024) {
        errorOut = "Audio too long for the AIFF 32-bit size field";
        return false;
    }
    const uint32_t dataBytes = (uint32_t)dataBytes64;

    std::vector<uint8_t> out;
    out.reserve(dataBytes + 256);

    putTag(out, "FORM");
    put32be(out, 0); // filled in at the end
    putTag(out, "AIFF");

    // ---- COMM: channels, frames, bit depth, sample rate ----
    putTag(out, "COMM");
    put32be(out, 18);
    put16be(out, (uint16_t)nch);
    put32be(out, (uint32_t)n);
    put16be(out, bits);
    putIeeeExtended80(out, d.sampleRate);

    const bool hasLoop = loopStart >= 0 && loopEnd > loopStart;

    // ---- INST: root note + fine tune, plus the sustain loop
    // cross-referencing MARK's marker IDs below (the AIFF spec's own loop
    // mechanism -- both id 1 and 2 only exist when hasLoop, matching the
    // MARK chunk emitted further down) ----
    {
        int unity = std::min(127, std::max(0, rootNote));
        // AIFF's detune is a single signed byte (integer cents) -- no
        // fractional pitch encoding like WAV/FLAC's smpl, so this rounds
        // instead of reconstructing a fraction.
        int detune = std::min(99, std::max(-99, (int)std::lround(tuneCents)));

        putTag(out, "INST");
        put32be(out, 20);
        out.push_back((uint8_t)unity);
        out.push_back((uint8_t)(int8_t)detune);
        out.push_back(0);   // lowNote
        out.push_back(127); // highNote
        out.push_back(0);   // lowVelocity
        out.push_back(127); // highVelocity
        put16be(out, 0);    // gain
        // sustainLoop cross-references MARK's marker IDs below (1/2) --
        // the spec-compliant mechanism real-world software (Kontakt, Awave
        // Studio, ExtremeSampleConverter, libsndfile's own
        // SFC_GET_INSTRUMENT...) actually relies on to detect an AIFF
        // loop; foreign_metadata.cpp's NAME-based scan is only a fallback
        // for malformed files that DON'T have this cross-reference, not
        // the primary mechanism. releaseLoop mirrors sustainLoop (same
        // marker pair) -- matches how reference files exported by other
        // tools set both to the same loop (see aiff_io.hpp). playMode per
        // the AIFF spec: 1 = forward, 2 = forward/backward (ping-pong) --
        // see loopType.
        const uint16_t playMode = !hasLoop ? 0 : (loopType == 1 ? 2 : 1);
        put16be(out, playMode); // sustainLoop.playMode
        put16be(out, hasLoop ? 1 : 0); // sustainLoop.beginLoop (marker id)
        put16be(out, hasLoop ? 2 : 0); // sustainLoop.endLoop (marker id)
        put16be(out, playMode); // releaseLoop.playMode
        put16be(out, hasLoop ? 1 : 0); // releaseLoop.beginLoop
        put16be(out, hasLoop ? 2 : 0); // releaseLoop.endLoop
    }

    // ---- MARK: named loop markers, cross-referenced from INST above by
    // ID (the mechanism real-world AIFF software actually reads) and also
    // matched by NAME by foreign_metadata.cpp's own fallback scan. End
    // position EXCLUSIVE here -- unlike the WAV smpl chunk's inclusive
    // convention, AIFF's marker-referenced loop end is exclusive (verified
    // against a real multi-program-compatible reference file and against
    // libsndfile's own AIFF reader, which passes the position through with
    // no +1 adjustment; see aiff_io.hpp). Name pair picked by
    // lpMarkerNames -- see this function's doc comment. ----
    if (hasLoop) {
        const char* begName = lpMarkerNames ? "LpBeg " : "beg loop";
        const char* endName = lpMarkerNames ? "LpEnd " : "end loop";

        std::vector<uint8_t> payload;
        put16be(payload, 2); // numMarkers
        put16be(payload, 1);
        put32be(payload, (uint32_t)loopStart);
        putPString(payload, begName);
        put16be(payload, 2);
        put32be(payload, (uint32_t)loopEnd);
        putPString(payload, endName);

        putTag(out, "MARK");
        put32be(out, (uint32_t)payload.size());
        out.insert(out.end(), payload.begin(), payload.end());
        if (payload.size() & 1) out.push_back(0);
    }

    // ---- SSND: big-endian signed PCM ----
    putTag(out, "SSND");
    put32be(out, 8 + dataBytes);
    put32be(out, 0); // offset
    put32be(out, 0); // blockSize
    size_t dataPos = out.size();
    out.resize(out.size() + dataBytes);
    uint8_t* p = out.data() + dataPos;
    if (bits == 24) {
        for (int64_t i = 0; i < n; ++i)
            for (int c = 0; c < nch; ++c) {
                float s = d.ch[c][(size_t)i] * scale;
                long v = std::lround((double)s * 8388608.0);
                v = std::min(8388607l, std::max(-8388608l, v));
                *p++ = (uint8_t)((v >> 16) & 0xff);
                *p++ = (uint8_t)((v >> 8) & 0xff);
                *p++ = (uint8_t)(v & 0xff);
            }
    } else {
        for (int64_t i = 0; i < n; ++i)
            for (int c = 0; c < nch; ++c) {
                float s = d.ch[c][(size_t)i] * scale;
                int v = (int)std::lround(s * 32768.f);
                v = std::min(32767, std::max(-32768, v));
                *p++ = (uint8_t)((v >> 8) & 0xff);
                *p++ = (uint8_t)(v & 0xff);
            }
    }
    if (dataBytes & 1) out.push_back(0); // chunk padding

    // FORM size
    uint32_t formSize = (uint32_t)(out.size() - 8);
    out[4] = (uint8_t)(formSize >> 24);
    out[5] = (uint8_t)(formSize >> 16);
    out[6] = (uint8_t)(formSize >> 8);
    out[7] = (uint8_t)(formSize & 0xff);

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        errorOut = "Could not open for writing: " + path;
        return false;
    }
    size_t wrote = fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    if (wrote != out.size()) {
        errorOut = "Disk write error";
        return false;
    }
    return true;
}
