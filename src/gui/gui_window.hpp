// X11 + GLX + ImGui window with its own GUI thread per plugin instance. The
// window is created as a child (embeddable via clap.gui/set_parent) and all
// of the instance's X11 traffic happens on its own Display, on its own
// thread. Implements XDND as a drag & drop target for files.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class GuiWindow {
public:
    // drawUI: builds the ImGui interface (called every frame, GUI thread).
    // onFileDrop: path dropped onto the window (GUI thread).
    GuiWindow(std::function<void()> drawUI,
              std::function<void(const std::string&)> onFileDrop);
    ~GuiWindow();

    bool create(uint32_t w, uint32_t h);
    void destroy();

    // Callable from the host's main thread (queued to the GUI thread):
    // nativeParent: X11 Window on Linux, HWND on Windows.
    void setParent(intptr_t nativeParent);
    void setVisible(bool visible);
    void setSize(uint32_t w, uint32_t h);

    void getSize(uint32_t& w, uint32_t& h) const {
        w = width_.load();
        h = height_.load();
    }

private:
    void threadMain();

    std::function<void()> drawUI_;
    std::function<void(const std::string&)> onFileDrop_;

    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> width_{1150}, height_{720};

    // command mailbox, host -> GUI thread
    std::mutex cmdMutex_;
    intptr_t pendingParent_ = 0;
    int pendingVisible_ = -1; // -1 nothing, 0 hide, 1 show
    uint32_t pendingW_ = 0, pendingH_ = 0;
    bool hasPendingSize_ = false;
};
