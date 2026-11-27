// State shared between the GUI thread and the audio thread.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include "audio_data.hpp"

// How the engine reads between samples when the playback ratio isn't 1.
enum class PlaybackInterp {
    None = 0,   // drop-sample: the previous sample is held (old hardware samplers)
    Linear = 1, // 2-point linear
    Cubic = 2,  // 4-point cubic Hermite spline (Catmull-Rom)
    Sinc16 = 3, // 16-tap windowed sinc (default)
};
constexpr int kPlaybackInterpCount = 4;

// "Live" parameters: the audio thread reads them per block, the GUI writes
// them. That way loop/selection/tune changes are heard instantly.
struct SharedParams {
    std::atomic<int64_t> loopStart{-1};  // -1 = no marker
    std::atomic<int64_t> loopEnd{-1};    // exclusive
    std::atomic<int64_t> selStart{-1};
    std::atomic<int64_t> selEnd{-1};
    std::atomic<int>     rootNote{60};
    std::atomic<float>   tuneCents{0.f}; // -99 .. +99
    std::atomic<bool>    loopEnabled{false};
    // Playback rate multiplier for auditioning (0.25x .. 4x), independent of
    // the piano/root-note pitch ratio -- e.g. checking a loop point in a
    // 3-minute file without waiting through it at 1x. Not a time-stretch:
    // pitch moves with speed, same as scrubbing tape. Never touches the
    // document, only how fast the playhead advances (see Engine::process).
    std::atomic<float>  playbackSpeed{1.f};
    // Playback interpolation (PlaybackInterp), auditioning only -- never
    // touches the document or any offline processing.
    std::atomic<int>    playbackInterp{(int)PlaybackInterp::Sinc16};
    // Piano preview envelope (see key_envelope.hpp): applied only to notes
    // triggered from the on-screen piano, never to Play or the document.
    std::atomic<bool>  keyEnvEnabled{false};
    std::atomic<int>   keyEnvCurve{0};        // keyenv::Curve
    std::atomic<float> keyEnvHoldMs{0.f};
    std::atomic<float> keyEnvDecayMs{1000.f};
};

// Publishing immutable snapshots to the audio thread. The GUI does set()
// (blocking, brief); the audio thread does tryGet() and if the lock is busy
// it keeps using its cached copy -- it never blocks or allocates memory.
template <typename P>
class SnapshotSlot {
public:
    void set(P p) {
        std::lock_guard<std::mutex> l(m_);
        v_ = std::move(p);
    }
    bool tryGet(P& out) {
        if (!m_.try_lock()) return false;
        out = v_;
        m_.unlock();
        return true;
    }

private:
    std::mutex m_;
    P v_;
};

// Keeps published snapshots alive for a few seconds so the last reference
// never dies on the audio thread (freeing memory there isn't RT-safe).
class SnapshotGraveyard {
public:
    void keep(AudioDataPtr p) {
        if (!p) return;
        items_.push_back({std::move(p), std::chrono::steady_clock::now()});
    }
    void prune() {
        auto now = std::chrono::steady_clock::now();
        size_t i = 0;
        while (i < items_.size()) {
            if (now - items_[i].when > std::chrono::seconds(8))
                items_.erase(items_.begin() + i);
            else
                ++i;
        }
    }

private:
    struct Item {
        AudioDataPtr p;
        std::chrono::steady_clock::time_point when;
    };
    std::vector<Item> items_;
};
