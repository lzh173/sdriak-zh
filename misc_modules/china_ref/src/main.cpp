#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <gui/widgets/waterfall.h>
#include <core.h>
#include <config.h>
#include <utils/flog.h>
#include <utils/event.h>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

SDRPP_MOD_INFO{
    /* Name:            */ "china_ref",
    /* Description:     */ "中文频率注释、频道速查与中继换算",
    /* Author:          */ "lzh173",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

namespace {

    std::string formatFreq(uint64_t hz) {
        char buf[64];
        if (hz >= 1000000ULL) {
            double mhz = (double)hz / 1e6;
            std::snprintf(buf, sizeof(buf), "%.6f MHz", mhz);
        }
        else if (hz >= 1000ULL) {
            std::snprintf(buf, sizeof(buf), "%.3f kHz", (double)hz / 1e3);
        }
        else {
            std::snprintf(buf, sizeof(buf), "%llu Hz", (unsigned long long)hz);
        }
        std::string out(buf);
        // Trim trailing zeros of the fractional part for a compact label.
        if (out.find(" MHz") != std::string::npos) {
            size_t dot = out.find('.');
            size_t space = out.find(" MHz");
            if (dot != std::string::npos && space != std::string::npos && space > dot) {
                size_t last = space;
                while (last > dot + 1 && out[last - 1] == '0') { last--; }
                if (last > dot + 1 || (last == dot + 1 && out[dot + 1] == '0')) {
                    out.erase(last, space - last);
                }
            }
        }
        return out;
    }

    bool parseMHz(const char* text, uint64_t& outHz) {
        if (!text || !*text) { return false; }
        char* end = nullptr;
        double value = std::strtod(text, &end);
        if (!end || *end != '\0' || value <= 0.0) { return false; }
        outHz = (uint64_t)std::llround(value * 1e6);
        return outHz > 0;
    }

    double parseMHzf(const char* text, bool& ok) {
        ok = false;
        if (!text || !*text) { return 0.0; }
        char* end = nullptr;
        double value = std::strtod(text, &end);
        if (!end || *end != '\0') { return 0.0; }
        ok = true;
        return value;
    }

    // Standard CTCSS tones, Hz. Kept here so the relay calculator can show the
    // usual list without pulling in another resource.
    const double CTCSS_TONES[] = {
        67.0, 69.3, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4, 88.5, 91.5,
        94.8, 97.4, 100.0, 103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3,
        131.8, 136.5, 141.3, 146.2, 151.4, 156.7, 159.8, 162.2, 165.5, 167.9,
        171.3, 173.8, 177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6, 199.5,
        203.5, 206.5, 210.7, 218.1, 225.7, 229.1, 233.6, 241.8, 250.3, 254.1
    };
    const int CTCSS_COUNT = (int)(sizeof(CTCSS_TONES) / sizeof(CTCSS_TONES[0]));
}

class ChinaRefModule : public ModuleManager::Instance {
public:
    ChinaRefModule(std::string name) {
        this->name = name;
        showOnWaterfall = core::configManager.read().value("chinaRefShowOnWaterfall", true);
        loadBuiltin();
        loadUser();
        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~ChinaRefModule() {
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        gui::menu.removeEntry(name);
    }

    void postInit() {}

    void enable() { enabled = true; }

    void disable() { enabled = false; }

    bool isEnabled() { return enabled; }

private:
    struct Annotation {
        std::string name;
        std::string cat;
        std::string mode;
        std::string note;
        uint64_t freq = 0;
        float bw = 0.0f;
        bool user = false;
    };

    struct ChannelItem {
        std::string name;
        uint64_t freq = 0;
    };

    struct ChannelGroup {
        std::string name;
        std::vector<ChannelItem> items;
    };

    // ---- data ----------------------------------------------------------

    void loadBuiltin() {
        annotations.clear();
        groups.clear();

        const std::string path = core::getResourcesDirectory() + "/china_ref.json";
        std::ifstream file(path);
        if (!file.is_open()) {
            flog::warn("china_ref: cannot open {}", path);
            return;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        json doc = json::parse(buffer.str(), nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) {
            flog::error("china_ref: {} is not valid JSON", path);
            return;
        }

        if (doc.contains("annotations") && doc["annotations"].is_array()) {
            for (const auto& item : doc["annotations"]) {
                Annotation a;
                a.name = item.value("name", std::string());
                a.cat = item.value("cat", std::string("其它"));
                a.mode = item.value("mode", std::string());
                a.note = item.value("note", std::string());
                a.freq = item.value("freq", (uint64_t)0);
                a.bw = item.value("bw", 0.0f);
                if (a.name.empty() || a.freq == 0) { continue; }
                annotations.push_back(std::move(a));
            }
        }

        if (doc.contains("groups") && doc["groups"].is_array()) {
            for (const auto& group : doc["groups"]) {
                ChannelGroup g;
                g.name = group.value("name", std::string());
                if (group.contains("items") && group["items"].is_array()) {
                    for (const auto& item : group["items"]) {
                        ChannelItem c;
                        c.name = item.value("name", std::string());
                        c.freq = item.value("freq", (uint64_t)0);
                        if (c.name.empty() || c.freq == 0) { continue; }
                        g.items.push_back(std::move(c));
                    }
                }
                if (!g.name.empty() && !g.items.empty()) { groups.push_back(std::move(g)); }
            }
        }

        flog::info("china_ref: loaded {} annotations, {} channel groups from {}",
                   (int)annotations.size(), (int)groups.size(), path);
    }

    void loadUser() {
        json arr = core::configManager.read().value("chinaRefAnnotations", json::array());
        if (!arr.is_array()) { return; }
        for (const auto& item : arr) {
            Annotation a;
            a.name = item.value("name", std::string());
            a.cat = item.value("cat", std::string("其它"));
            a.mode = item.value("mode", std::string());
            a.note = item.value("note", std::string());
            a.freq = item.value("freq", (uint64_t)0);
            a.bw = item.value("bw", 0.0f);
            a.user = true;
            if (a.name.empty() || a.freq == 0) { continue; }
            annotations.push_back(std::move(a));
        }
    }

    void saveUser() {
        json arr = json::array();
        for (const auto& a : annotations) {
            if (!a.user) { continue; }
            json item;
            item["name"] = a.name;
            item["cat"] = a.cat;
            item["freq"] = a.freq;
            item["mode"] = a.mode;
            item["bw"] = a.bw;
            item["note"] = a.note;
            arr.push_back(std::move(item));
        }
        core::configManager.edit().set("chinaRefAnnotations", arr);
    }

    // ---- helpers -------------------------------------------------------

    double currentFreq() {
        if (gui::waterfall.selectedVFO.empty()) { return 0.0; }
        auto it = gui::waterfall.vfos.find(gui::waterfall.selectedVFO);
        if (it == gui::waterfall.vfos.end() || !it->second) { return 0.0; }
        return gui::waterfall.getCenterFrequency() + it->second->generalOffset;
    }

    const Annotation* matchFor(double freq) {
        if (freq <= 0.0) { return nullptr; }
        const Annotation* best = nullptr;
        double bestMargin = 0.0;
        for (const auto& a : annotations) {
            const double tolerance = std::max<double>(a.bw > 0 ? a.bw * 0.5 : 3000.0, 3000.0);
            const double margin = std::fabs(freq - (double)a.freq);
            if (margin > tolerance) { continue; }
            // Prefer the narrowest entry so a channel annotation wins over a
            // wide band-style one.
            const double width = a.bw > 0 ? a.bw : 6000.0;
            if (!best || width < bestMargin) {
                best = &a;
                bestMargin = width;
            }
        }
        return best;
    }

    void tune(uint64_t hz) {
        if (gui::waterfall.selectedVFO.empty()) {
            flog::warn("china_ref: no VFO selected");
            return;
        }
        tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, (double)hz);
    }

    // ---- UI ------------------------------------------------------------

    static void menuHandler(void* ctx) {
        ChinaRefModule* _this = (ChinaRefModule*)ctx;
        if (ImGui::BeginTabBar(("##_china_ref_tabs_" + _this->name).c_str())) {
            if (ImGui::BeginTabItem("注释库")) {
                _this->drawAnnotationsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("频道表")) {
                _this->drawChannelsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("中继换算")) {
                _this->drawRelayTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    void drawAnnotationsTab() {
        const double now = currentFreq();
        const Annotation* match = matchFor(now);
        if (now > 0.0) {
            ImGui::Text("当前：%s", formatFreq((uint64_t)std::llround(now)).c_str());
            if (match) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "→ %s", match->name.c_str());
            }
        }
        else {
            ImGui::TextDisabled("当前：未选择 VFO");
        }

        if (ImGui::Checkbox("在瀑布图上标注", &showOnWaterfall)) {
            core::configManager.edit().set("chinaRefShowOnWaterfall", showOnWaterfall);
        }

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
        ImGui::InputTextWithHint(("##_china_ref_search_" + name).c_str(), "搜索名称/备注…", searchBuf, sizeof(searchBuf));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo(("##_china_ref_cat_" + name).c_str(), &catFilter, categoriesTxt);

        const std::string query = lower(searchBuf);
        ImGui::BeginChild(("##_china_ref_list_" + name).c_str(), ImVec2(0, 220.0f), false);
        for (size_t i = 0; i < annotations.size(); i++) {
            Annotation& a = annotations[i];
            if (catFilter > 0 && a.cat != catList[catFilter - 1]) { continue; }
            if (!query.empty()) {
                if (lower(a.name).find(query) == std::string::npos &&
                    lower(a.note).find(query) == std::string::npos) { continue; }
            }
            ImGui::PushID((int)i);
            const std::string label = a.name + "  ·  " + formatFreq(a.freq);
            if (ImGui::Selectable(label.c_str())) { tune(a.freq); }
            if (!a.note.empty() && ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", a.note.c_str()); }
            if (a.user) {
                ImGui::SameLine();
                if (ImGui::SmallButton("删除")) {
                    annotations.erase(annotations.begin() + (std::ptrdiff_t)i);
                    saveUser();
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        std::string addPopupId = "添加注释###_china_ref_add_" + name;
        if (ImGui::Button("添加注释…")) { ImGui::OpenPopup(addPopupId.c_str()); }
        ImGui::SameLine();
        ImGui::TextDisabled("共 %d 条（%d 条自定义）",
                            (int)annotations.size(),
                            (int)std::count_if(annotations.begin(), annotations.end(),
                                               [](const Annotation& a) { return a.user; }));

        if (ImGui::BeginPopupModal(addPopupId.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(200.0f);
            ImGui::InputText(("频率(MHz)##_china_ref_new_freq_" + name).c_str(), newFreqBuf, sizeof(newFreqBuf));
            ImGui::SetNextItemWidth(280.0f);
            ImGui::InputText(("名称##_china_ref_new_name_" + name).c_str(), newNameBuf, sizeof(newNameBuf));
            ImGui::SetNextItemWidth(280.0f);
            ImGui::InputText(("备注##_china_ref_new_note_" + name).c_str(), newNoteBuf, sizeof(newNoteBuf));
            if (ImGui::Button("保存")) {
                uint64_t hz = 0;
                if (parseMHz(newFreqBuf, hz) && newNameBuf[0]) {
                    Annotation a;
                    a.name = newNameBuf;
                    a.cat = "自定义";
                    a.note = newNoteBuf;
                    a.freq = hz;
                    a.bw = 0.0f;
                    a.user = true;
                    annotations.push_back(std::move(a));
                    saveUser();
                    newNameBuf[0] = '\0';
                    newNoteBuf[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("取消")) { ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }

    void drawChannelsTab() {
        for (auto& group : groups) {
            if (!ImGui::CollapsingHeader(group.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) { continue; }
            ImGui::Columns(3, ("##_china_ref_cols_" + group.name).c_str(), false);
            for (auto& item : group.items) {
                if (ImGui::Button((item.name + "##" + group.name).c_str(), ImVec2(-1, 0))) {
                    tune(item.freq);
                }
                ImGui::NextColumn();
            }
            ImGui::Columns(1);
        }
    }

    void drawRelayTab() {
        ImGui::TextWrapped("输入接收频率与偏移，得到发射频率。亚音（CTCSS）用于打开中继。");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputText(("接收频率(MHz)##_china_ref_rx_" + name).c_str(), rxFreqBuf, sizeof(rxFreqBuf));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputText(("偏移(MHz)##_china_ref_shift_" + name).c_str(), shiftBuf, sizeof(shiftBuf));

        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::BeginCombo(("亚音##_china_ref_tone_" + name).c_str(), toneIdx == 0 ? "无" : toneLabels[toneIdx].c_str())) {
            for (int i = 0; i < (int)toneLabels.size(); i++) {
                if (ImGui::Selectable(toneLabels[i].c_str(), toneIdx == i)) { toneIdx = i; }
            }
            ImGui::EndCombo();
        }

        bool okRx = false;
        bool okShift = false;
        const double rx = parseMHzf(rxFreqBuf, okRx);
        const double shift = parseMHzf(shiftBuf, okShift);

        if (okRx && okShift) {
            const double tx = rx + shift;
            ImGui::Text("发射：%.6f MHz", tx);
            if (std::fabs(shift) > 20.0) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "偏移超过 20 MHz，请确认是否符合当地规定");
            }
        }
        else {
            ImGui::TextDisabled("请输入有效的数字");
        }

        if (ImGui::Button("调谐到接收频率") && okRx) {
            tune((uint64_t)std::llround(rx * 1e6));
        }
        ImGui::SameLine();
        if (ImGui::Button("调谐到发射频率") && okRx && okShift) {
            tune((uint64_t)std::llround((rx + shift) * 1e6));
        }
        ImGui::TextDisabled("提示：国内常见中继为上行 430–440 MHz，具体频率与偏移以当地公布为准。");
    }

    // ---- waterfall overlay ---------------------------------------------

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        ChinaRefModule* _this = (ChinaRefModule*)ctx;
        if (!_this->showOnWaterfall || _this->annotations.empty()) { return; }

        ImDrawList* drawList = args.window->DrawList;
        const float rowHeight = ImGui::GetTextLineHeight() + 2.0f;
        const int maxRows = 3;
        std::vector<std::pair<float, float>> used[maxRows];
        int drawn = 0;

        for (const auto& a : _this->annotations) {
            if (drawn >= 64) { break; }
            const double freq = (double)a.freq;
            if (freq < args.lowFreq || freq > args.highFreq) { continue; }

            const float x = (float)(args.min.x + (freq - args.lowFreq) * args.freqToPixelRatio);
            const ImVec2 textSize = ImGui::CalcTextSize(a.name.c_str());
            const float left = x + 3.0f;
            const float right = left + textSize.x;

            int row = 0;
            for (; row < maxRows; row++) {
                bool hit = false;
                for (const auto& span : used[row]) {
                    if (right > span.first && left < span.second) { hit = true; break; }
                }
                if (!hit) { break; }
            }
            if (row >= maxRows) { continue; }
            used[row].push_back({ left, right });

            const float y = args.min.y + 3.0f + row * rowHeight;
            drawList->AddLine(ImVec2(x, args.min.y), ImVec2(x, args.max.y), IM_COL32(255, 170, 40, 70), 1.0f);
            drawList->AddText(ImVec2(left, y), IM_COL32(255, 200, 90, 230), a.name.c_str());
            drawn++;
        }
    }

    std::string lower(std::string in) {
        for (auto& c : in) { c = (char)std::tolower((unsigned char)c); }
        return in;
    }

    std::string name;
    bool enabled = true;
    bool showOnWaterfall = true;

    std::vector<Annotation> annotations;
    std::vector<ChannelGroup> groups;

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;

    char searchBuf[128] = "";
    int catFilter = 0;
    const std::vector<std::string> catList = { "广播", "航空", "水上", "对讲", "卫星", "其它", "自定义" };
    const char* categoriesTxt = "全部\0广播\0航空\0水上\0对讲\0卫星\0其它\0自定义\0";

    char newFreqBuf[32] = "145.000";
    char newNameBuf[128] = "";
    char newNoteBuf[256] = "";

    char rxFreqBuf[32] = "439.000";
    char shiftBuf[32] = "-5.000";
    int toneIdx = 0;
    std::vector<std::string> toneLabels = [] {
        std::vector<std::string> labels{ "无" };
        for (int i = 0; i < CTCSS_COUNT; i++) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1f Hz", CTCSS_TONES[i]);
            labels.push_back(buf);
        }
        return labels;
    }();
};

MOD_EXPORT void _INIT_() {}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ChinaRefModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (ChinaRefModule*)instance;
}

MOD_EXPORT void _END_() {}
