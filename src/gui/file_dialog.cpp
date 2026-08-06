#include "file_dialog.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "imgui.h"

namespace fs = std::filesystem;

void FileDialog::open(Mode mode, const char* title,
                      std::vector<std::string> extensions,
                      const std::string& defaultName) {
    mode_ = mode;
    title_ = title;
    exts_ = std::move(extensions);
    open_ = true;
    needRefresh_ = true;
    error_.clear();
    if (cwd_.empty()) {
        const char* home = getenv("HOME");
        cwd_ = home ? home : "/";
    }
    snprintf(nameEdit_, sizeof(nameEdit_), "%s", defaultName.c_str());
    optLabel_.clear();
    optItems_.clear();
    optIndex_ = 0;
}

void FileDialog::setOptions(const char* label, std::vector<std::string> items, int index) {
    optLabel_ = label;
    optItems_ = std::move(items);
    optIndex_ = std::min((int)optItems_.size() - 1, std::max(0, index));
}

bool FileDialog::matchesFilter(const std::string& name) const {
    if (exts_.empty()) return true;
    std::string low = name;
    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
    for (auto& e : exts_)
        if (low.size() >= e.size() &&
            low.compare(low.size() - e.size(), e.size(), e) == 0)
            return true;
    return false;
}

void FileDialog::refresh() {
    entries_.clear();
    std::error_code ec;
    for (auto& de : fs::directory_iterator(cwd_, ec)) {
        Entry e;
        e.name = de.path().filename().string();
        if (e.name.empty() || e.name[0] == '.') continue;
        e.isDir = de.is_directory(ec);
        e.size = e.isDir ? 0 : (uint64_t)de.file_size(ec);
        if (e.isDir || matchesFilter(e.name)) entries_.push_back(std::move(e));
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    snprintf(pathEdit_, sizeof(pathEdit_), "%s", cwd_.c_str());
    needRefresh_ = false;
}

bool FileDialog::draw() {
    if (!open_) return false;
    if (needRefresh_) refresh();

    bool accepted = false;
    ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_FirstUseEver);
    bool keep = true;
    if (ImGui::Begin(title_.c_str(), &keep,
                     ImGuiWindowFlags_NoCollapse)) {
        // path bar
        if (ImGui::Button("Up")) {
            fs::path p(cwd_);
            if (p.has_parent_path() && p != p.root_path()) {
                cwd_ = p.parent_path().string();
                needRefresh_ = true;
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##path", pathEdit_, sizeof(pathEdit_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::error_code ec;
            if (fs::is_directory(pathEdit_, ec)) {
                cwd_ = pathEdit_;
                needRefresh_ = true;
            }
        }

        // listing
        float footer = ((mode_ == SaveFile ? 2.f : 1.f) +
                        (optItems_.empty() ? 0.f : 1.f)) *
                           (ImGui::GetFrameHeightWithSpacing()) +
                       8.f;
        if (ImGui::BeginChild("##files", ImVec2(0, -footer), true)) {
            for (auto& e : entries_) {
                std::string label =
                    (e.isDir ? "[DIR] " : "      ") + e.name;
                bool selected = !e.isDir && e.name == nameEdit_;
                if (ImGui::Selectable(label.c_str(), selected,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (e.isDir) {
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            cwd_ = (fs::path(cwd_) / e.name).string();
                            needRefresh_ = true;
                        }
                    } else {
                        snprintf(nameEdit_, sizeof(nameEdit_), "%s",
                                 e.name.c_str());
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            result_ = (fs::path(cwd_) / e.name).string();
                            accepted = true;
                        }
                    }
                }
                if (!e.isDir && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%.1f KB", e.size / 1024.0);
            }
        }
        ImGui::EndChild();

        // name + buttons
        auto tryAccept = [&]() {
            if (!nameEdit_[0]) return;
            std::string name = nameEdit_;
            if (mode_ == SaveFile && !matchesFilter(name) && !exts_.empty())
                name += exts_[0];
            fs::path p = fs::path(cwd_) / name;
            std::error_code ec;
            if (mode_ == OpenFile) {
                if (fs::is_regular_file(p, ec)) {
                    result_ = p.string();
                    accepted = true;
                } else {
                    error_ = "File does not exist";
                }
            } else if (fs::exists(p, ec)) {
                // a file with that name already exists: ask for confirmation
                // instead of accepting directly (see popup below).
                pendingPath_ = p.string();
                overwriteTrigger_ = true;
            } else {
                result_ = p.string();
                accepted = true;
            }
        };

        if (mode_ == SaveFile) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##name", nameEdit_, sizeof(nameEdit_),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
                tryAccept(); // Enter in the name field = Save
        }
        if (!optItems_.empty()) {
            ImGui::SetNextItemWidth(180);
            if (ImGui::BeginCombo(optLabel_.c_str(),
                                  optItems_[(size_t)optIndex_].c_str())) {
                for (int i = 0; i < (int)optItems_.size(); ++i)
                    if (ImGui::Selectable(optItems_[(size_t)i].c_str(), i == optIndex_))
                        optIndex_ = i;
                ImGui::EndCombo();
            }
        }
        if (!error_.empty())
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());
        const char* okLabel = mode_ == OpenFile ? "Open" : "Save";
        if (ImGui::Button(okLabel, ImVec2(120, 0))) tryAccept();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) keep = false;
    }
    ImGui::End();

    // Overwrite confirmation: a separate modal, always drawn (not only
    // inside the Begin above) so it stays open even though the rest of the
    // window ends up behind it, blocked by the modal.
    if (overwriteTrigger_) {
        ImGui::OpenPopup("Replace file");
        overwriteTrigger_ = false;
    }
    if (ImGui::BeginPopupModal("Replace file", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        fs::path pp(pendingPath_);
        ImGui::Text("A file named \"%s\" already exists.",
                    pp.filename().string().c_str());
        ImGui::Text("Its contents will be replaced.");
        if (ImGui::Button("Replace", ImVec2(120, 0))) {
            result_ = pendingPath_;
            accepted = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (accepted || !keep) open_ = false;
    return accepted;
}
