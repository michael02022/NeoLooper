// Real-time engine: sample playback (with pitch relative to root note +
// finetune), input recording and capture for the oscilloscope. The audio
// thread never blocks nor allocates memory.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>

#include "envelope.hpp"
#include "shared.hpp"
#include "sinc_interp.hpp"

using EnvelopesPtr = std::shared_ptr<const Envelopes>;

class Engine {
public:
    ~Engine() { freeChunks(); }

    enum Transport { Stopped = 0, Playing = 1, Recording = 2 };

    SharedParams params;

    // ---- GUI / main thread ----
    void publishDocument(AudioDataPtr d);
    // Loop-module preview; null = remove. loopOffset = sample within 'd'
    // where the region that repeats forever starts; anything before that
    // (the "attack", e.g. Harmonic's entry crossfade) plays only once when
    // entering preview, same as the attack preceding a marked loop during
    // normal playback (the same sampleInterp/wrap mechanism in
    // Engine::process, not a new one). offset=0 (default) loops the whole
    // buffer, the usual behavior.
    void publishPreview(AudioDataPtr d, int64_t loopOffset = 0);
    // DOCUMENT preview (Filter): replaces the document during NORMAL
    // playback -- loop markers, selection, envelopes, transport and playhead
    // keep working as usual, unlike module preview (which forces looping of
    // the published buffer and resets position). Meant for previewing
    // processing that does NOT change the document's duration. null =
    // remove. If a module preview is active, that one takes priority.
    void publishDocPreview(AudioDataPtr d);
    // Envelopes being edited: published every GUI frame so they're AUDIBLE
    // in real time during playback (Edison-style).
    void publishEnvelopes(EnvelopesPtr e);

    void play();                 // resumes from the playhead
    void pause();                // freezes the playhead
    void playFromNote(int note); // piano: triggers from the start at that pitch
    void seek(double frame);

    void startRecord();
    // Returns what was recorded as AudioData (or null if there was nothing)
    // and frees the chunks. Call after pausing/stopping the recording.
    std::shared_ptr<AudioData> stopRecord();
    // GUI poll: reserves recording chunks ahead of the audio thread.
    void pollRecording();

    int transport() const { return transport_.load(); }
    bool isPlaying() const { return transport_.load() == Playing; }
    bool isRecording() const { return transport_.load() == Recording; }
    double playheadFrame() const { return uiPlayhead_.load(); }
    double hostSampleRate() const { return hostSr_.load(); }
    int64_t recordedFrames() const { return recFrames_.load(); }
    bool recordOverrun() const { return recOverrun_.load(); }
    int currentNote() const { return curNote_.load(); }
    bool previewActive() const { return previewOn_.load(); }

    // Oscilloscope: copies the last played samples (mono) aligned to the
    // total counter; returns how many valid samples there are.
    int64_t scopeCounter() const { return scopeWrite_.load(std::memory_order_acquire); }
    float scopeSample(int64_t idx) const {
        return scopeRing_[(size_t)(idx & (kScopeSize - 1))];
    }
    static constexpr int64_t kScopeSize = 1 << 16;

    // ---- audio thread ----
    void activate(double sampleRate);
    void process(const float* inL, const float* inR,
                 float* outL, float* outR, uint32_t nframes);

    // GUI: snapshot retention so nothing dies on the audio thread.
    SnapshotGraveyard graveyard;

private:
    SnapshotSlot<AudioDataPtr> docSlot_, prevSlot_, docPrevSlot_;
    SnapshotSlot<EnvelopesPtr> envSlot_;
    AudioDataPtr rtDoc_, rtPrev_, rtDocPrev_; // audio-thread caches
    EnvelopesPtr rtEnv_;
    // Retains the last envelope snapshots so their last reference is never
    // released on the audio thread.
    std::deque<EnvelopesPtr> envRetain_;

    std::atomic<int> transport_{Stopped};
    std::atomic<int> curNote_{-1}; // -1 = play at the root note
    std::atomic<bool> previewOn_{false};
    std::atomic<bool> docPreviewOn_{false};
    std::atomic<int64_t> previewLoopOffset_{0};
    std::atomic<double> hostSr_{48000.0};
    std::atomic<double> uiPlayhead_{0.0};

    // seek request: the GUI increments seekSeq_ after writing seekPos_
    std::atomic<double> seekPos_{0.0};
    std::atomic<uint32_t> seekSeq_{0};
    uint32_t seekSeen_ = 0;

    double pos_ = 0.0;   // audio thread's playhead (document frames)
    float gain_ = 0.f;   // anti-click ramp
    bool wasPreview_ = false;

    // --- chunked recording (the audio thread only writes into memory that's
    // already allocated; the GUI keeps allocating ahead of it) ---
    static constexpr int kMaxChunks = 512;
    static constexpr int64_t kChunkFrames = 1 << 19; // ~4 MB per chunk (stereo)
    std::atomic<float*> chunks_[kMaxChunks] = {};
    std::atomic<int> chunksReady_{0};
    std::atomic<int64_t> recFrames_{0};
    std::atomic<bool> recOverrun_{false};
    void freeChunks();
    // Processed-block counter: lets code outside the RT path wait for the
    // audio thread to have seen a transport change before freeing memory.
    std::atomic<uint64_t> procGen_{0};
    void waitAudioQuiescent();

    // --- oscilloscope ---
    float scopeRing_[kScopeSize] = {};
    std::atomic<int64_t> scopeWrite_{0};

    // Table built on the main thread (Engine's constructor, never on the
    // audio thread): the RT thread only does lookups.
    SincInterp16 sinc_;
    // The 16-tap kernel spans 8 samples on each side of the position. If
    // those neighboring samples fall outside [ls, le) they wrap around the
    // loop (instead of reading audio outside the loop, or silence if
    // le==frames()): without this you hear a click at the wrap point
    // whenever the playback ratio isn't exactly 1.0 (root note/finetune, or
    // a host sample rate different from the document's).
    float sampleInterp(const AudioData& d, int c, double pos,
                       bool looping, int64_t ls, int64_t le) const;
};
