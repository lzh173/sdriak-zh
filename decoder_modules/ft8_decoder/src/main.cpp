#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <gui/widgets/waterfall.h>
#include <core.h>
#include <config.h>
#include <signal_path/signal_path.h>
#include <dsp/stream.h>
#include <dsp/convert/stereo_to_mono.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>
#include <utils/optionlist.h>

#include <common/monitor.h>
#include <ft8/constants.h>
#include <ft8/decode.h>
#include <ft8/message.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

SDRPP_MOD_INFO{
    /* Name:            */ "ft8_decoder",
    /* Description:     */ "FT8/FT4 接收解码，并把解码结果显示在频谱上",
    /* Author:          */ "lzh173",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

namespace {

    constexpr double DECODE_RATE = 12000.0;   // ft8_lib expects 12 kHz audio
    constexpr int MAX_CANDIDATES = 140;
    constexpr int MIN_SCORE = 10;
    constexpr int LDPC_ITERATIONS = 25;
    constexpr size_t MAX_RESULTS = 400;
    constexpr size_t MAX_QUEUE_SAMPLES = (size_t)(DECODE_RATE * 60.0);

    // ---- callsign hash table -------------------------------------------
    // ftx_message_decode() resolves hashed callsigns through this interface.
    // Single module instance, so a file-scope table keeps the callbacks plain C.

    struct HashEntry {
        char callsign[12];
        uint32_t hash;
    };
    HashEntry hashTable[256];

    void hashtableInit() {
        std::memset(hashTable, 0, sizeof(hashTable));
    }

    void hashtableAdd(const char* callsign, uint32_t hash) {
        uint16_t hash10 = (hash >> 12) & 0x3FFu;
        int idx = (hash10 * 23) % 256;
        while (hashTable[idx].callsign[0] != '\0') {
            if (((hashTable[idx].hash & 0x3FFFFFu) == hash) && (0 == std::strcmp(hashTable[idx].callsign, callsign))) {
                return;
            }
            idx = (idx + 1) % 256;
        }
        std::strncpy(hashTable[idx].callsign, callsign, 11);
        hashTable[idx].callsign[11] = '\0';
        hashTable[idx].hash = hash;
    }

    bool hashtableLookup(ftx_callsign_hash_type_t type, uint32_t hash, char* callsign) {
        uint8_t shift = (type == FTX_CALLSIGN_HASH_10_BITS) ? 12 : (type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0);
        uint16_t hash10 = (uint16_t)((hash >> (12 - shift)) & 0x3FFu);
        int idx = (hash10 * 23) % 256;
        while (hashTable[idx].callsign[0] != '\0') {
            if (((hashTable[idx].hash & 0x3FFFFFu) >> shift) == hash) {
                std::strcpy(callsign, hashTable[idx].callsign);
                return true;
            }
            idx = (idx + 1) % 256;
        }
        callsign[0] = '\0';
        return false;
    }

    ftx_callsign_hash_interface_t hashIf = { hashtableLookup, hashtableAdd };

    std::string formatUtc(uint32_t hhmmss) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
                      (hhmmss / 10000u) % 100u, (hhmmss / 100u) % 100u, hhmmss % 100u);
        return buf;
    }

}  // namespace

class Ft8DecoderModule : public ModuleManager::Instance {
public:
    Ft8DecoderModule(std::string name) {
        this->name = name;

        auto cfg = core::configManager.read();
        protocolFt4 = cfg.value("ft8ProtocolFt4", false);
        showOnWaterfall = cfg.value("ft8ShowOnWaterfall", true);
        cqOnly = cfg.value("ft8CqOnly", false);
        selectedStreamName = cfg.value("ft8Stream", std::string());
        slotPeriod = protocolFt4 ? FT4_SLOT_TIME : FT8_SLOT_TIME;

        resetDecoder();
        running = true;
        worker = std::thread(&Ft8DecoderModule::workerMain, this);

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        flog::info("ft8: created (protocol {}, stream '{}')", protocolFt4 ? "FT4" : "FT8", selectedStreamName);
    }

    ~Ft8DecoderModule() {
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        running = false;
        queueCv.notify_all();
        if (worker.joinable()) { worker.join(); }
        deselectStream();
        sigpath::sinkManager.onStreamRegistered.unbindHandler(&onStreamRegisteredHandler);
        sigpath::sinkManager.onStreamUnregister.unbindHandler(&onStreamUnregisterHandler);
        if (monitorReady) {
            monitor_free(&monitor);
            monitorReady = false;
        }
    }

    void postInit() {
        for (const auto& streamName : sigpath::sinkManager.getStreamNames()) {
            streams.define(streamName, streamName, streamName);
        }
        onStreamRegisteredHandler.ctx = this;
        onStreamRegisteredHandler.handler = streamRegisteredHandler;
        sigpath::sinkManager.onStreamRegistered.bindHandler(&onStreamRegisteredHandler);
        onStreamUnregisterHandler.ctx = this;
        onStreamUnregisterHandler.handler = streamUnregisterHandler;
        sigpath::sinkManager.onStreamUnregister.bindHandler(&onStreamUnregisterHandler);

        selectStream(selectedStreamName);
    }

    void enable() { enabled = true; }

    void disable() { enabled = false; }

    bool isEnabled() { return enabled; }

private:
    struct DecodeResult {
        uint32_t slotUtc = 0;
        double audioFreq = 0.0;
        float snr = 0.0f;
        float dt = 0.0f;
        std::string text;
        bool isCq = false;
        std::chrono::steady_clock::time_point seen;
    };

    // ---- audio path ----------------------------------------------------

    void selectStream(const std::string& streamName) {
        std::lock_guard<std::recursive_mutex> lock(pathMutex);
        deselectStream();
        if (streams.empty()) { return; }
        if (!streams.keyExists(streamName)) {
            selectStream(streams.key(0));
            return;
        }
        auto* bound = sigpath::sinkManager.bindStream(streamName);
        if (!bound) { return; }

        audioStream = bound;
        selectedStreamName = streamName;
        streamId = streams.keyId(streamName);
        inputRate = sigpath::sinkManager.getStreamSampleRate(streamName);
        core::configManager.edit().set("ft8Stream", streamName);

        s2m.init(audioStream);
        resamp.init(&s2m.out, inputRate, DECODE_RATE);
        sink.init(&resamp.out, sampleHandler, this);
        s2m.start();
        resamp.start();
        sink.start();
        flog::info("ft8: bound '{}' at {} Hz", streamName, inputRate);
    }

    void deselectStream() {
        std::lock_guard<std::recursive_mutex> lock(pathMutex);
        if (selectedStreamName.empty() || !audioStream) {
            selectedStreamName.clear();
            return;
        }
        sink.stop();
        resamp.stop();
        s2m.stop();
        sigpath::sinkManager.unbindStream(selectedStreamName, audioStream);
        selectedStreamName.clear();
        audioStream = NULL;
    }

    static void sampleHandler(float* data, int count, void* ctx) {
        Ft8DecoderModule* _this = (Ft8DecoderModule*)ctx;
        std::lock_guard<std::mutex> lock(_this->queueMutex);
        if (_this->sampleQueue.size() + (size_t)count > MAX_QUEUE_SAMPLES) {
            _this->sampleQueue.clear();
        }
        _this->sampleQueue.insert(_this->sampleQueue.end(), data, data + count);
        _this->queueCv.notify_one();
    }

    std::vector<float> takeSamples(int blockSize) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCv.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return !running.load() || (int)sampleQueue.size() >= blockSize;
        });
        if (!running.load() || (int)sampleQueue.size() < blockSize) { return {}; }
        std::vector<float> out(sampleQueue.begin(), sampleQueue.begin() + blockSize);
        sampleQueue.erase(sampleQueue.begin(), sampleQueue.begin() + blockSize);
        return out;
    }

    // ---- decoding ------------------------------------------------------

    void resetDecoder() {
        std::lock_guard<std::mutex> lock(decoderMutex);
        if (monitorReady) {
            monitor_free(&monitor);
            monitorReady = false;
        }
        monitor_config_t cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.f_min = 200.0f;
        cfg.f_max = 3000.0f;
        cfg.sample_rate = (int)DECODE_RATE;
        cfg.time_osr = 2;
        cfg.freq_osr = 2;
        cfg.protocol = protocolFt4 ? FTX_PROTOCOL_FT4 : FTX_PROTOCOL_FT8;
        monitor_init(&monitor, &cfg);
        monitorReady = true;
        currentSlot = INT64_MIN;
        slotStartUtc = 0;
        hashtableInit();
        {
            std::lock_guard<std::mutex> resultsLock(resultsMutex);
            results.clear();
        }
        flog::info("ft8: reset (block_size {} nfft {} max_blocks {})",
                   monitor.block_size, monitor.nfft, monitor.wf.max_blocks);
    }

    void workerMain() {
        while (running.load()) {
            int blockSize = 0;
            {
                std::lock_guard<std::mutex> lock(decoderMutex);
                blockSize = monitorReady ? monitor.block_size : 0;
            }
            if (blockSize <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            auto chunk = takeSamples(blockSize);
            if (chunk.empty()) { continue; }
            processChunk(chunk);
        }
    }

    void processChunk(const std::vector<float>& chunk) {
        const int64_t slot = slotIndexForNow();
        std::lock_guard<std::mutex> lock(decoderMutex);
        if (!monitorReady) { return; }
        if (currentSlot == INT64_MIN) {
            currentSlot = slot;
            slotStartUtc = slotStartUtcTime(slot);
        }
        else if (slot != currentSlot) {
            decodeSlotLocked();
            monitor_reset(&monitor);
            currentSlot = slot;
            slotStartUtc = slotStartUtcTime(slot);
        }
        monitor_process(&monitor, chunk.data());
    }

    int64_t slotIndexForNow() const {
        using namespace std::chrono;
        const double t = duration<double>(system_clock::now().time_since_epoch()).count();
        return (int64_t)std::floor(t / slotPeriod);
    }

    uint32_t slotStartUtcTime(int64_t slot) const {
        const time_t start = (time_t)std::llround((double)slot * slotPeriod);
        struct tm tmUtc{};
#ifdef _WIN32
        gmtime_s(&tmUtc, &start);
#else
        gmtime_r(&start, &tmUtc);
#endif
        return (uint32_t)(tmUtc.tm_hour * 10000 + tmUtc.tm_min * 100 + tmUtc.tm_sec);
    }

    // Decodes the accumulated slot. Caller holds decoderMutex.
    void decodeSlotLocked() {
        const int minBlocks = monitor.wf.max_blocks * 2 / 3;
        if (monitor.wf.num_blocks < minBlocks) { return; }

        ftx_candidate_t candidates[MAX_CANDIDATES];
        const int count = ftx_find_candidates(&monitor.wf, MAX_CANDIDATES, candidates, MIN_SCORE);
        flog::debug("ft8: slot {} candidates {}", slotStartUtc, count);

        std::vector<std::pair<uint32_t, std::array<uint8_t, 10>>> seen;
        std::lock_guard<std::mutex> lock(resultsMutex);
        for (int i = 0; i < count; i++) {
            ftx_message_t message;
            ftx_decode_status_t status;
            if (!ftx_decode_candidate(&monitor.wf, &candidates[i], LDPC_ITERATIONS, &message, &status)) { continue; }

            bool duplicate = false;
            for (const auto& entry : seen) {
                if (entry.first == message.hash &&
                    std::memcmp(entry.second.data(), message.payload, sizeof(message.payload)) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) { continue; }
            std::array<uint8_t, 10> payload{};
            std::memcpy(payload.data(), message.payload, sizeof(message.payload));
            seen.emplace_back(message.hash, payload);

            char text[FTX_MAX_MESSAGE_LENGTH];
            ftx_message_offsets_t offsets;
            if (ftx_message_decode(&message, &hashIf, text, &offsets) != FTX_MESSAGE_RC_OK) { continue; }

            DecodeResult result;
            result.slotUtc = slotStartUtc;
            result.audioFreq = (monitor.min_bin + candidates[i].freq_offset +
                                (float)candidates[i].freq_sub / monitor.wf.freq_osr) / monitor.symbol_period;
            result.dt = (candidates[i].time_offset +
                         (float)candidates[i].time_sub / monitor.wf.time_osr) * monitor.symbol_period;
            result.snr = candidates[i].score * 0.5f;
            result.text = text;
            result.isCq = (result.text.rfind("CQ ", 0) == 0);
            result.seen = std::chrono::steady_clock::now();
            results.push_back(std::move(result));
        }
        while (results.size() > MAX_RESULTS) { results.erase(results.begin()); }
        flog::info("ft8: slot {} finished, {} results total", slotStartUtc, (int)results.size());
    }

    // ---- UI ------------------------------------------------------------

    static void menuHandler(void* ctx) {
        Ft8DecoderModule* _this = (Ft8DecoderModule*)ctx;

        if (!_this->streams.empty()) {
            int streamIndex = _this->streams.keyId(_this->selectedStreamName);
            if (streamIndex < 0) { streamIndex = 0; }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo(("##_ft8_stream_" + _this->name).c_str(), _this->streams.name(streamIndex).c_str()) {
                for (int i = 0; i < (int)_this->streams.size(); i++) {
                    if (ImGui::Selectable(_this->streams.name(i).c_str(), i == streamIndex)) {
                        _this->selectStream(_this->streams.key(i));
                    }
                }
                ImGui::EndCombo();
            }
        }
        else {
            ImGui::TextDisabled("先选择一个电台解调输出（VFO）");
        }

        int protocol = _this->protocolFt4 ? 1 : 0;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo(("##_ft8_protocol_" + _this->name).c_str(), &protocol, "FT8\0FT4\0")) {
            _this->protocolFt4 = (protocol == 1);
            _this->slotPeriod = _this->protocolFt4 ? FT4_SLOT_TIME : FT8_SLOT_TIME;
            core::configManager.edit().set("ft8ProtocolFt4", _this->protocolFt4);
            _this->resetDecoder();
        }

        if (ImGui::Checkbox(("在瀑布图上显示##_ft8_wf_" + _this->name).c_str(), &_this->showOnWaterfall)) {
            core::configManager.edit().set("ft8ShowOnWaterfall", _this->showOnWaterfall);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(("仅 CQ##_ft8_cq_" + _this->name).c_str(), &_this->cqOnly)) {
            core::configManager.edit().set("ft8CqOnly", _this->cqOnly);
        }

        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint(("##_ft8_search_" + _this->name).c_str(), "搜索呼号/内容…",
                                 _this->searchBuf, sizeof(_this->searchBuf));

        const double now = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const double phase = std::fmod(now, _this->slotPeriod) / _this->slotPeriod;
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", phase * 100.0);
        ImGui::ProgressBar((float)phase, ImVec2(-1, 0), overlay);

        const std::string query = _this->lower(_this->searchBuf);
        int shown = 0;
        ImGui::BeginChild(("##_ft8_list_" + _this->name).c_str(), ImVec2(0, 240.0f), false);
        {
            std::lock_guard<std::mutex> lock(_this->resultsMutex);
            for (auto it = _this->results.rbegin(); it != _this->results.rend(); ++it) {
                const DecodeResult& r = *it;
                if (_this->cqOnly && !r.isCq) { continue; }
                if (!query.empty() && _this->lower(r.text).find(query) == std::string::npos) { continue; }
                char line[256];
                std::snprintf(line, sizeof(line), "%s  %+5.1f dB  %+.1f  %6.0f Hz  %s",
                              formatUtc(r.slotUtc).c_str(), r.snr, r.dt, r.audioFreq, r.text.c_str());
                ImGui::PushID(shown);
                if (ImGui::Selectable(line)) { _this->tuneToDecode(r); }
                ImGui::PopID();
                shown++;
            }
        }
        ImGui::EndChild();

        ImGui::TextDisabled("%d 条结果%s", shown, _this->selectedStreamName.empty() ? "（未绑定流）" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton(("清空##_ft8_clear_" + _this->name).c_str())) {
            std::lock_guard<std::mutex> lock(_this->resultsMutex);
            _this->results.clear();
        }
    }

    void tuneToDecode(const DecodeResult& r) {
        if (selectedStreamName.empty()) { return; }
        auto vfo = gui::waterfall.vfos.find(selectedStreamName);
        if (vfo == gui::waterfall.vfos.end() || !vfo->second) { return; }
        const double absolute = gui::waterfall.getCenterFrequency() + vfo->second->generalOffset + r.audioFreq;
        tuner::tune(tuner::TUNER_MODE_NORMAL, selectedStreamName, absolute);
    }

    // ---- waterfall overlay ---------------------------------------------

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        Ft8DecoderModule* _this = (Ft8DecoderModule*)ctx;
        if (!_this->showOnWaterfall || _this->selectedStreamName.empty()) { return; }
        auto vfo = gui::waterfall.vfos.find(_this->selectedStreamName);
        if (vfo == gui::waterfall.vfos.end() || !vfo->second) { return; }
        const double dial = gui::waterfall.getCenterFrequency() + vfo->second->generalOffset;

        const auto now = std::chrono::steady_clock::now();
        const float rowHeight = ImGui::GetTextLineHeight() + 2.0f;
        constexpr int MAX_ROWS = 3;
        std::vector<std::pair<float, float>> used[MAX_ROWS];
        ImDrawList* drawList = args.window->DrawList;

        std::vector<const DecodeResult*> visible;
        {
            std::lock_guard<std::mutex> lock(_this->resultsMutex);
            for (const auto& r : _this->results) {
                if (std::chrono::duration<double>(now - r.seen).count() > _this->overlayHoldSec) { continue; }
                if (_this->cqOnly && !r.isCq) { continue; }
                visible.push_back(&r);
            }
        }
        std::reverse(visible.begin(), visible.end());

        int drawn = 0;
        for (const DecodeResult* r : visible) {
            if (drawn >= 32) { break; }
            const double freq = dial + r->audioFreq;
            if (freq < args.lowFreq || freq > args.highFreq) { continue; }
            const float x = (float)(args.min.x + (freq - args.lowFreq) * args.freqToPixelRatio);
            const float left = x + 3.0f;
            const ImVec2 textSize = ImGui::CalcTextSize(r->text.c_str());
            const float right = left + textSize.x;
            int row = 0;
            for (; row < MAX_ROWS; row++) {
                bool hit = false;
                for (const auto& span : used[row]) {
                    if (right > span.first && left < span.second) { hit = true; break; }
                }
                if (!hit) { break; }
            }
            if (row >= MAX_ROWS) { continue; }
            used[row].push_back({ left, right });
            const float y = args.min.y + 3.0f + row * rowHeight;
            const ImU32 color = r->isCq ? IM_COL32(255, 190, 60, 235) : IM_COL32(120, 220, 255, 225);
            drawList->AddLine(ImVec2(x, args.min.y), ImVec2(x, args.max.y), IM_COL32(255, 190, 60, 60), 1.0f);
            drawList->AddText(ImVec2(left, y), color, r->text.c_str());
            drawn++;
        }
    }

    // ---- helpers -------------------------------------------------------

    static std::string lower(std::string in) {
        for (auto& c : in) { c = (char)std::tolower((unsigned char)c); }
        return in;
    }

    static void streamRegisteredHandler(std::string streamName, void* ctx) {
        Ft8DecoderModule* _this = (Ft8DecoderModule*)ctx;
        _this->streams.define(streamName, streamName, streamName);
        if (_this->selectedStreamName.empty()) { _this->selectStream(streamName); }
    }

    static void streamUnregisterHandler(std::string streamName, void* ctx) {
        Ft8DecoderModule* _this = (Ft8DecoderModule*)ctx;
        if (_this->selectedStreamName == streamName) { _this->deselectStream(); }
        _this->streams.undefineKey(streamName);
    }

    // ---- state ---------------------------------------------------------

    std::string name;
    bool enabled = true;
    bool protocolFt4 = false;
    bool showOnWaterfall = true;
    bool cqOnly = false;
    double overlayHoldSec = 30.0;
    double slotPeriod = FT8_SLOT_TIME;

    OptionList<std::string, std::string> streams;
    std::string selectedStreamName;
    int streamId = 0;
    double inputRate = 48000.0;

    dsp::stream<dsp::stereo_t>* audioStream = NULL;
    dsp::convert::StereoToMono s2m;
    dsp::multirate::RationalResampler<float> resamp;
    dsp::sink::Handler<float> sink;
    std::recursive_mutex pathMutex;

    std::thread worker;
    std::atomic<bool> running{ false };
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::vector<float> sampleQueue;

    std::mutex decoderMutex;
    monitor_t monitor;
    bool monitorReady = false;
    int64_t currentSlot = INT64_MIN;
    uint32_t slotStartUtc = 0;

    std::mutex resultsMutex;
    std::vector<DecodeResult> results;
    char searchBuf[64] = "";

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
    EventHandler<std::string> onStreamRegisteredHandler;
    EventHandler<std::string> onStreamUnregisterHandler;
};

MOD_EXPORT void _INIT_() {}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new Ft8DecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (Ft8DecoderModule*)instance;
}

MOD_EXPORT void _END_() {}
