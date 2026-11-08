#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <config.h>
#include <core.h>
#include <vector>
#include <unordered_map>
#include <memory>
#include "signal_path/signal_path.h"
#include <radio_interface.h>
#include "if_nr.h"
#include "af_nr.h"

// Ported from SDR++Brown (https://github.com/sannysanoff/SDRPlusPlusBrown),
// misc_modules/noise_reduction_logmmse. Trimmed relative to the original:
//  - SNR chart widget removed (needs Brown-only SNR meter extension points).
//  - Per-VFO IF-chain LogMMSE (AFNRLogMMSE) removed: dead code upstream,
//    its radio IFCHAIN interface commands are intentionally not ported.
// Kept: wideband baseband NR (LogMMSE IQ preprocessor) and per-radio
// OMLSA-MCRA audio NR ("Audio NR2").

ConfigManager config;

SDRPP_MOD_INFO{
    /* Name:            */ "noise_reduction_logmmse",
    /* Description:     */ "LOGMMSE noise reduction",
    /* Author:          */ "sannysanoff",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

class NRModule : public ModuleManager::Instance {

    dsp::IFNRLogMMSE ifnrProcessor;

    std::unordered_map<std::string, std::shared_ptr<dsp::AFNR_OMLSA_MCRA>> afnrProcessors2; // instance by radio name.

public:
    NRModule(std::string name) {
        this->name = name;
        {
            auto configAccess = config.read();
            configAccess.tryGet("IFNR", ifnr);
            configAccess.tryGet("DisableCpuDeactivation", disableCpuDeactivation);
        }

        ifnrProcessor.setDisableCpuDeactivation(disableCpuDeactivation);

        gui::menu.registerEntry(name, menuHandler, this, this);
        updateBindings();
        actuateIFNR();
    }

    ~NRModule() {
        disable(); // detach from IQ chain, radios and events before dying
        gui::menu.removeEntry(name);
    }

    void postInit() {}

    void enable() {
        if (!enabled) {
            enabled = true;
            ifnrProcessor.setDisableCpuDeactivation(disableCpuDeactivation);
            updateBindings();
            actuateIFNR();
        }
    }

    void disable() {
        if (enabled) {
            enabled = false;
            actuateIFNR();
            updateBindings();
        }
    }

    bool isEnabled() {
        return enabled;
    }


private:
    bool ifnr = false;
    bool disableCpuDeactivation = false;

    void attachAFToRadio(const std::string& instanceName) {
        const std::shared_ptr<dsp::AFNR_OMLSA_MCRA> afnromlsa = std::make_shared<dsp::AFNR_OMLSA_MCRA>();
        afnromlsa->init(nullptr);
        afnrProcessors2[instanceName] = afnromlsa;
        afnromlsa->setAudioSampleRate((int)sigpath::sinkManager.getStreamSampleRate(instanceName));

        bool afnr2 = config.read().value("AF_NR2_" + instanceName, false);
        afnromlsa->allowed = afnr2;

        // Only splice the block into the running AF chain when the feature is
        // on; a merely added block costs nothing. Toggled live by the checkbox.
        core::modComManager.callInterface(instanceName, RADIO_IFACE_CMD_ADD_TO_AFCHAIN, afnromlsa.get(), NULL);
        if (afnr2) {
            core::modComManager.callInterface(instanceName, RADIO_IFACE_CMD_ENABLE_IN_AFCHAIN, afnromlsa.get(), NULL);
        }
    }

    void detachAFFromRadio(const std::string& instanceName) {
        if (afnrProcessors2.find(instanceName) != afnrProcessors2.end()) {
            core::modComManager.callInterface(instanceName, RADIO_IFACE_CMD_REMOVE_FROM_AFCHAIN,
                                              afnrProcessors2[instanceName].get(), NULL);
            afnrProcessors2.erase(instanceName);
        }
    }

    void updateBindings() {
        if (enabled) {
            flog::info("Enabling noise reduction things");

            sigpath::iqFrontEnd.addPreprocessor(&ifnrProcessor, false);

            currentFrequencyChangedHandler.ctx = this;
            currentFrequencyChangedHandler.handler = [](double v, void* ctx) {
                auto _this = (NRModule*)ctx;
                _this->ifnrProcessor.reset(); // reset noise profile
            };
            sigpath::sourceManager.onRetune.bindHandler(&currentFrequencyChangedHandler);

            auto names = core::modComManager.findInterfaces("radio");
            for (auto& name : names) {
                attachAFToRadio(name);
            }
            instanceCreatedHandler.ctx = this;
            instanceCreatedHandler.handler = [](std::string v, void* ctx) {
                auto _this = (NRModule*)ctx;
                auto modname = core::moduleManager.getInstanceModuleName(v);
                if (modname == "radio") {
                    // radio created after the NR module.
                    _this->attachAFToRadio(v);
                }
            };
            core::moduleManager.onInstanceCreated.bindHandler(&instanceCreatedHandler);
        }
        else {
            core::moduleManager.onInstanceCreated.unbindHandler(&instanceCreatedHandler);
            sigpath::sourceManager.onRetune.unbindHandler(&currentFrequencyChangedHandler);

            // Detach OMLSA processors from the radios' AF chains. Without this,
            // re-enabling would attach fresh processors while the old (freed)
            // ones are still wired into the chains.
            std::vector<std::string> attached;
            for (auto& [k, v] : afnrProcessors2) { attached.push_back(k); }
            for (auto& k : attached) { detachAFFromRadio(k); }

            sigpath::iqFrontEnd.removePreprocessor(&ifnrProcessor);
        }
    }

    void actuateIFNR() {
        bool shouldRun = enabled && ifnr;
        if (ifnrProcessor.bypass != !shouldRun) {
            ifnrProcessor.bypass = !shouldRun;
            sigpath::iqFrontEnd.togglePreprocessor(&ifnrProcessor, shouldRun);
        }
    }

    void menuHandler() {
        if (!enabled) { style::beginDisabled(); }

        if (ImGui::Checkbox("基带降噪##_sdrpp_if_nr", &ifnr)) {
            config.edit().set("IFNR", ifnr);
            if (ifnr) { // toggled on - attempt to run.
                ifnrProcessor.stopReason = nullptr;
            }
            actuateIFNR();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("对整个 IQ 流的 LogMMSE 降噪，位于\n"
                              "FFT 和所有解调器之前：净化瀑布图和每个 VFO。\n"
                              "持续自适应，无需静音训练。\n"
                              "高采样率下 CPU 占用较高。");
        }
        ImGui::SameLine();
        const char* stopReason = ifnrProcessor.stopReason;
        if (stopReason && ifnr) { // wants to stop -> stop it.
            ifnr = false;
            config.edit().set("IFNR", ifnr);
            actuateIFNR();
        }
        if (stopReason) { // stopped because reason -> show it.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0, 0, 1.0f));
            ImGui::Text("%s", stopReason);
            ImGui::PopStyleColor(1);
        }
        else {
            // show cpu usage
            if (ifnrProcessor.percentUsage >= 0) {
                if (ifnrProcessor.percentUsage > 80) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0, 0, 1.0f));
                }
                std::string cpuText = std::to_string((int)ifnrProcessor.percentUsage) + "% cpu";
                ImVec2 textSize = ImGui::CalcTextSize(cpuText.c_str());
                std::string cpuLabel = cpuText + "##_sdrpp_ifnr_cpu_load";
                bool clicked = ImGui::Selectable(cpuLabel.c_str(), false, ImGuiSelectableFlags_None, textSize);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("基带降噪处理负载。超过约 95%% 时降噪会自动\n"
                                      "关闭；点击可禁用该保护（文字带删除线\n"
                                      "表示保护已关闭）。");
                }
                if (clicked) {
                    disableCpuDeactivation = !disableCpuDeactivation;
                    ifnrProcessor.setDisableCpuDeactivation(disableCpuDeactivation);
                    config.edit().set("DisableCpuDeactivation", disableCpuDeactivation);
                }
                if (disableCpuDeactivation) {
                    auto drawList = ImGui::GetWindowDrawList();
                    ImVec2 min = ImGui::GetItemRectMin();
                    ImVec2 max = ImGui::GetItemRectMax();
                    drawList->AddLine(ImVec2(min.x, (min.y + max.y) / 2), ImVec2(max.x, (min.y + max.y) / 2), ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
                }
                if (ifnrProcessor.percentUsage > 80) {
                    ImGui::PopStyleColor(1);
                }
            }
        }

        for (auto& [k, v] : afnrProcessors2) {
            // Track the sink's audio rate (cheap; applied by the DSP thread only on change)
            v->setAudioSampleRate((int)sigpath::sinkManager.getStreamSampleRate(k));
            if (ImGui::Checkbox(("音频降噪（OMLSA） " + k + "##_radio_omlsa_nr_" + k).c_str(), &v->allowed)) {
                core::modComManager.callInterface(k, v->allowed ? RADIO_IFACE_CMD_ENABLE_IN_AFCHAIN : RADIO_IFACE_CMD_DISABLE_IN_AFCHAIN, v.get(), NULL);
                // Disabling joins the block's worker thread, so the buffer can
                // be cleared here; otherwise stale audio would flush on re-enable.
                if (!v->allowed) { v->buffer.clear(); }
                config.edit().set("AF_NR2_" + k, v->allowed);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("对本电台音频输出使用 OMLSA-MCRA 语音降噪。\n"
                                  "在 SSB/AM 语音上效果最佳；会降低数字模式和音乐的音质。\n"
                                  "输出为单声道，会增加约一个块（约 32 ms）的延迟。");
            }
            ImGui::SameLine();
            ImGui::Text("%0.01f", 32767.0 / v->scaled);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("进入降噪器的最近音频峰值（1.0 = 满量程）。\n"
                                  "降噪以 16 位定点处理，并自动缩放到\n"
                                  "该电平以保留余量。");
            }
        }

        if (!enabled) { style::endDisabled(); }
    }

    static void menuHandler(void* ctx) {
        NRModule* _this = (NRModule*)ctx;
        _this->menuHandler();
    }

    std::string name;
    bool enabled = true;
    EventHandler<double> currentFrequencyChangedHandler;
    EventHandler<std::string> instanceCreatedHandler;
};


MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/noise_reduction_logmmse_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new NRModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (NRModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
