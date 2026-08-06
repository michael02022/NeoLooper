// Self-contained ImGui file browser (open and save) -- no external toolkit
// dependencies, works embedded in any host.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class FileDialog {
public:
    enum Mode { OpenFile, SaveFile };

    void open(Mode mode, const char* title,
              std::vector<std::string> extensions, // lowercase: ".wav"...
              const std::string& defaultName = "");

    // Optional combo in the dialog's footer (meant for SaveFile: e.g. bit
    // depth on export). Configure AFTER open(); cleared on the next open().
    // The choice ends up in optionIndex() once draw() returns true.
    void setOptions(const char* label, std::vector<std::string> items, int index);
    int optionIndex() const { return optIndex_; }

    // Draws the dialog; returns true if the user accepted, leaving the
    // chosen path in result().
    bool draw();
    const std::string& result() const { return result_; }
    bool isOpen() const { return open_; }

private:
    void refresh();
    bool matchesFilter(const std::string& name) const;

    Mode mode_ = OpenFile;
    bool open_ = false;
    bool needRefresh_ = false;
    std::string title_;
    std::vector<std::string> exts_;
    std::string cwd_;
    char pathEdit_[1024] = {};
    char nameEdit_[512] = {};
    struct Entry {
        std::string name;
        bool isDir;
        uint64_t size;
    };
    std::vector<Entry> entries_;
    std::string result_;
    std::string error_;
    // Overwrite confirmation (SaveFile): if the chosen name already exists on
    // disk, confirmation is requested before accepting. overwriteTrigger_ is
    // a one-shot trigger (consumed on the first draw() after being set) that
    // opens the modal popup; pendingPath_ is the path awaiting that
    // confirmation.
    bool overwriteTrigger_ = false;
    std::string pendingPath_;

    std::string optLabel_;
    std::vector<std::string> optItems_;
    int optIndex_ = 0;
};
