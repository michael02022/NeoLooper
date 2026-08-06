// State shared between the GUI thread and the audio thread.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include "audio_data.hpp"

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
