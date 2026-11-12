#include <gui/widgets/file_select.h>
#include <regex>
#include <filesystem>
#include <core.h>
#ifdef __ANDROID__
#include <android_backend.h>
#else
#include <gui/file_dialogs.h>
#endif

FileSelect::FileSelect(std::string defaultPath, std::vector<std::string> filter) {
    _filter = filter;
    root = (std::string)core::args["root"];
    setPath(defaultPath);
}

bool FileSelect::render(std::string id) {
    bool _pathChanged = false;
    float menuColumnWidth = ImGui::GetContentRegionAvail().x;

    ImGuiStyle& style = ImGui::GetStyle();
    float buttonWidth = ImGui::CalcTextSize("...").x + (style.FramePadding.x * 2.0f);
    bool lastPathValid = pathValid;
    if (!lastPathValid) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
    }
    ImGui::SetNextItemWidth(menuColumnWidth - buttonWidth - style.ItemSpacing.x);
    if (ImGui::InputText(id.c_str(), strPath, 2047)) {
        path = std::string(strPath);
        std::string expandedPath = expandString(strPath);
        if (!std::filesystem::is_regular_file(expandedPath)) {
            pathValid = false;
        }
        else {
            pathValid = true;
            _pathChanged = true;
        }
    }
    if (!lastPathValid) {
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (ImGui::Button(("..." + id + "_winselect").c_str(), ImVec2(buttonWidth, 0)) && !dialogOpen) {
        dialogOpen = true;
        if (workerThread.joinable()) { workerThread.join(); }
        workerThread = std::thread(&FileSelect::worker, this);
    }

    _pathChanged |= pathChanged;
    pathChanged = false;
    return _pathChanged;
}

void FileSelect::setPath(std::string path, bool markChanged) {
    this->path = path;
    std::string expandedPath = expandString(path);
    pathValid = std::filesystem::is_regular_file(expandedPath);
    if (markChanged) { pathChanged = true; }
    strcpy(strPath, path.c_str());
}

std::string FileSelect::expandString(std::string input) {
    input = std::regex_replace(input, std::regex("%ROOT%"), root);
    return std::regex_replace(input, std::regex("//"), "/");
}

bool FileSelect::pathIsValid() {
    return pathValid;
}

void FileSelect::worker() {
#ifdef __ANDROID__
    // portable-file-dialogs only knows the desktop helpers (zenity/kdialog/
    // osascript), so on Android the system document picker does the job and
    // hands back a path the native readers can open.
    std::string picked = backend::pickFile();
    if (!picked.empty()) {
        path = picked;
        strcpy(strPath, path.c_str());
        pathChanged = true;
    }
#else
    auto file = pfd::open_file("打开文件", pathValid ? std::filesystem::path(expandString(path)).parent_path().string() : "", _filter);
    std::vector<std::string> res = file.result();

    if (res.size() > 0) {
        path = res[0];
        strcpy(strPath, path.c_str());
        pathChanged = true;
    }

#endif

    pathValid = std::filesystem::is_regular_file(expandString(path));
    dialogOpen = false;
}