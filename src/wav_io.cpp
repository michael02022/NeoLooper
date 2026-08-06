#include "wav_io.hpp"

#include <sndfile.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "foreign_metadata.hpp"

LoadResult loadAudioFile(const std::string& path) {
    LoadResult r;
    SF_INFO info{};
    SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
    if (!f) {
        r.error = sf_strerror(nullptr);
        return r;
    }
    if (info.channels < 1 || info.frames <= 0) {
        r.error = "File has no audio";
        sf_close(f);
        return r;
    }

    auto d = std::make_shared<AudioData>();
    d->sampleRate = (double)info.samplerate;
    // Edison policy: the editor works in 16 or 32 bit. Files with 16 bits or
    // less come in as 16-bit; everything else (24-bit, float, ogg...) as
    // 32-bit.
    int sub = info.format & SF_FORMAT_SUBMASK;
    bool is16 = (sub == SF_FORMAT_PCM_16 || sub == SF_FORMAT_PCM_S8 ||
                 sub == SF_FORMAT_PCM_U8);
    d->format = is16 ? SampleFormat::Int16 : SampleFormat::Float32;

    int nch = info.channels > 2 ? 2 : info.channels; // mono or stereo only
    std::vector<float> inter((size_t)info.frames * info.channels);
    sf_count_t got = sf_readf_float(f, inter.data(), info.frames);

    d->ch.resize(nch);
    for (int c = 0; c < nch; ++c) d->ch[c].resize((size_t)got);
    for (sf_count_t i = 0; i < got; ++i)
        for (int c = 0; c < nch; ++c)
            d->ch[c][(size_t)i] = inter[(size_t)i * info.channels + c];

    // instrument metadata (loop from the smpl chunk, root note)
    SF_INSTRUMENT inst{};
    if (sf_command(f, SFC_GET_INSTRUMENT, &inst, sizeof(inst)) == SF_TRUE) {
        r.hasInstrument = true;
        r.rootNote = inst.basenote;
        // libsndfile exposes the pitch fraction as detune in cents [0,100);
        // >50 cents is equivalent to the next note minus the complement.
        r.tuneCents = (float)inst.detune;
        if (r.tuneCents > 50.f) {
            r.rootNote += 1;
            r.tuneCents -= 100.f;
        }
        if (inst.loop_count > 0) {
            // libsndfile already reports end as exclusive
            r.loopStart = inst.loops[0].start;
            r.loopEnd = (int64_t)inst.loops[0].end;
        }
    }
    sf_close(f);

    // Fallback/supplement: libsndfile doesn't know about FLAC's
    // `--keep-foreign-metadata` embedding at all (so it always comes up
    // empty there), and some real-world AIFF files name their loop
    // markers by convention ("beg loop"/"LpBeg "/...) without correctly
    // wiring INST's marker-ID cross-reference that libsndfile requires --
    // only fills in what libsndfile didn't already find (see
    // foreign_metadata.hpp).
    if (!r.hasInstrument || r.loopStart < 0) {
        foreignmeta::InstrumentInfo fi;
        if (foreignmeta::scan(path, fi)) {
            if (!r.hasInstrument && fi.hasNote) {
                r.hasInstrument = true;
                r.rootNote = fi.rootNote;
                r.tuneCents = fi.tuneCents;
            }
            if (r.loopStart < 0 && fi.hasLoop) {
                r.hasInstrument = true;
                r.loopStart = fi.loopStart;
                r.loopEnd = fi.loopEnd;
            }
        }
    }

    if (d->format == SampleFormat::Int16)
        for (auto& v : d->ch)
            for (auto& s : v) s = quantize16(s);

    d->buildPeaks();
    r.data = std::move(d);
    return r;
}

// ------------------------------------------------------------------ export

namespace {
void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x & 0xff);
    v.push_back((x >> 8) & 0xff);
    v.push_back((x >> 16) & 0xff);
    v.push_back((x >> 24) & 0xff);
}
void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xff);
    v.push_back((x >> 8) & 0xff);
}
void putTag(std::vector<uint8_t>& v, const char* t) {
    v.insert(v.end(), t, t + 4);
}
} // namespace

bool exportWavSmpl(const std::string& path, const AudioData& d, int bitDepth,
                   int64_t loopStart, int64_t loopEnd, int rootNote,
                   float tuneCents, std::string& errorOut, bool* scaledDown) {
    if (d.frames() <= 0) {
        errorOut = "No audio to export";
        return false;
    }
    if (bitDepth != 16 && bitDepth != 24 && bitDepth != 32) {
        errorOut = "Unsupported bit depth (16/24/32)";
        return false;
    }
    const int nch = d.channels();
    const int64_t n = d.frames();
    const bool f32 = bitDepth == 32;
    const uint16_t bits = (uint16_t)bitDepth;

    // If the document's peak exceeds what the target integer format can
    // hold, scale the WHOLE file down so the peak lands exactly on that
    // format's ceiling -- clipping would distort every sample above it,
    // scaling only makes the file quieter. 32-bit float has no practical
    // ceiling here, so it's exempt.
    float scale = 1.0f;
    if (!f32) {
        const float ceiling = bits == 24 ? kInt24Max : kInt16Max;
        float peak = d.peakAbs();
        if (peak > ceiling) scale = ceiling / peak;
    }
    if (scaledDown) *scaledDown = (scale != 1.0f);
    const uint16_t fmt = f32 ? 3 /*IEEE float*/ : 1 /*PCM*/;
    const uint32_t sr = (uint32_t)std::lround(d.sampleRate);
    const uint32_t blockAlign = nch * bits / 8;
    const uint64_t dataBytes64 = (uint64_t)n * blockAlign;
    if (dataBytes64 > 0xFFFFFFFFull - 1024) {
        errorOut = "Audio too long for the WAV 32-bit size field";
        return false;
    }
    const uint32_t dataBytes = (uint32_t)dataBytes64;

    std::vector<uint8_t> out;
    out.reserve(dataBytes + 256);

    putTag(out, "RIFF");
    put32(out, 0); // filled in at the end
    putTag(out, "WAVE");

    putTag(out, "fmt ");
    put32(out, 16);
    put16(out, fmt);
    put16(out, (uint16_t)nch);
    put32(out, sr);
    put32(out, sr * blockAlign);
    put16(out, (uint16_t)blockAlign);
    put16(out, bits);

    // ---- smpl chunk: loop points + unity note + pitch fraction ----
    {
        // MIDIPitchFraction is the fraction of a semitone UPWARD from the
        // unity note, in units of 1/2^32 of a semitone. Negative cents are
        // encoded by lowering the unity note by one semitone.
        int unity = rootNote;
        double cents = (double)tuneCents;
        // The sample's own tuning is the opposite of the correction applied:
        // if the user raises tune by +20 cents to tune it up, the sample was
        // 20 cents flat; the chunk describes the sample as exported, already
        // played at the unity note, so we store the tune value as-is.
        if (cents < 0) {
            unity -= 1;
            cents += 100.0;
        }
        unity = std::min(127, std::max(0, unity));
        uint32_t pitchFrac =
            (uint32_t)std::llround((cents / 100.0) * 4294967296.0 / 1.0);
        if (cents >= 100.0) pitchFrac = 0xFFFFFFFFu;

        const bool hasLoop = loopStart >= 0 && loopEnd > loopStart;
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
            put32(out, 0);                       // type 0 = forward
            put32(out, (uint32_t)loopStart);
            put32(out, (uint32_t)(loopEnd - 1)); // end inclusive in smpl
            put32(out, 0);                       // fraction
            put32(out, 0);                       // infinite
        }
    }

    putTag(out, "data");
    put32(out, dataBytes);
    size_t dataPos = out.size();
    out.resize(out.size() + dataBytes);
    uint8_t* p = out.data() + dataPos;
    if (f32) {
        for (int64_t i = 0; i < n; ++i)
            for (int c = 0; c < nch; ++c) {
                float s = d.ch[c][(size_t)i];
                memcpy(p, &s, 4);
                p += 4;
            }
    } else if (bits == 24) {
        for (int64_t i = 0; i < n; ++i)
            for (int c = 0; c < nch; ++c) {
                float s = d.ch[c][(size_t)i] * scale;
                long v = std::lround((double)s * 8388608.0);
                v = std::min(8388607l, std::max(-8388608l, v));
                *p++ = (uint8_t)(v & 0xff);
                *p++ = (uint8_t)((v >> 8) & 0xff);
                *p++ = (uint8_t)((v >> 16) & 0xff);
            }
    } else {
        for (int64_t i = 0; i < n; ++i)
            for (int c = 0; c < nch; ++c) {
                float s = d.ch[c][(size_t)i] * scale;
                int v = (int)std::lround(s * 32768.f);
                v = std::min(32767, std::max(-32768, v));
                *p++ = (uint8_t)(v & 0xff);
                *p++ = (uint8_t)((v >> 8) & 0xff);
            }
    }
    if (dataBytes & 1) out.push_back(0); // RIFF padding

    // RIFF size
    uint32_t riffSize = (uint32_t)(out.size() - 8);
    out[4] = riffSize & 0xff;
    out[5] = (riffSize >> 8) & 0xff;
    out[6] = (riffSize >> 16) & 0xff;
    out[7] = (riffSize >> 24) & 0xff;

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
