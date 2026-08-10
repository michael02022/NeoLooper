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

    // Adds one combo to the dialog's footer (meant for SaveFile: e.g. bit
    // depth, loop-marker naming, loop type...). Call once per combo, AFTER
    // open() (all are cleared on the next open()); each call is drawn in
    // the order added. Returns a handle to pass to comboValue() once
    // draw() returns true.
    int addCombo(const char* label, std::vector<std::string> items, int index);
    int comboValue(int handle) const { return combos_[(size_t)handle].index; }

    // Optional checkbox in the dialog's footer (meant for SaveFile: e.g.
    // whether to embed cue markers on export). Configure AFTER open();
    // cleared on the next open(). The choice ends up in checkboxValue()
    // once draw() returns true.
    void setCheckbox(const char* label, bool defaultValue);
    bool checkboxValue() const { return checkboxValue_; }

    // Optional float slider in the dialog's footer (meant for SaveFile:
    // e.g. OGG VBR quality 0.0-1.0). Configure AFTER open(); cleared on the
    // next open(). The choice ends up in sliderValue() once draw() returns
    // true.
    void setSlider(const char* label, float defaultValue, float min, float max);
    float sliderValue() const { return sliderValue_; }

    // Optional informational line in the dialog's footer (meant for
    // SaveFile: e.g. noting a format's export limitations, dimmed text).
    // Configure AFTER open(); cleared on the next open().
    void setInfoText(const char* text);

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

    struct Combo {
        std::string label;
        std::vector<std::string> items;
        int index = 0;
    };
    std::vector<Combo> combos_;

    std::string checkboxLabel_;
    bool checkboxValue_ = false;

    std::string sliderLabel_;
    float sliderValue_ = 0.f;
    float sliderMin_ = 0.f, sliderMax_ = 1.f;

    std::string infoText_;
};
