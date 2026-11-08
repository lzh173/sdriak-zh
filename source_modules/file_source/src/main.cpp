/*
 * Adapted from the SDRPP distribution (https://github.com/qrp73/SDRPP).
 * Copyright (c) 2023 qrp73.
 * Modified for SDRIAK, 2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#define NOMINMAX
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <wavreader.h>
#include <flacreader.h>
#include <core.h>
#include <gui/widgets/file_select.h>
#include <filesystem>
#include <regex>
#include <gui/tuner.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <atomic>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "file_source",
    /* Description:     */ "Wav file source module for SDRIAK",
    /* Author:          */ "Ryzerth, qrp73",
    /* Version:         */ 0, 1, 2,
    /* Max instances    */ 1
};

ConfigManager config;

// Converters from one WAV sample to float, all little-endian
// (per-format conversion adopted from qrp73/SDRPP)
typedef float (*SampleConv)(const uint8_t*);

static float convU8(const uint8_t* p) {
    return ((float)p[0] - 127.5f) * (1.0f / 127.5f);
}
static float convI16(const uint8_t* p) {
    int16_t v;
    memcpy(&v, p, sizeof(v));
    return (float)v * (1.0f / 32768.0f);
}
static float convI24(const uint8_t* p) {
    int32_t v = (int32_t)((uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16)) << 8) >> 8;
    return (float)v * (1.0f / 8388608.0f);
}
static float convI32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return (float)((double)v * (1.0 / 2147483648.0));
}
static float convF32(const uint8_t* p) {
    float v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static float convF64(const uint8_t* p) {
    double v;
    memcpy(&v, p, sizeof(v));
    return (float)v;
}

static SampleConv getSampleConv(WAVE_FORMAT format, int bitDepth) {
    if (format == WAVE_FORMAT::PCM) {
        switch (bitDepth) {
            case 8:  return convU8;
            case 16: return convI16;
            case 24: return convI24;
            case 32: return convI32;
        }
    }
    else if (format == WAVE_FORMAT::IEEE_FLOAT) {
        switch (bitDepth) {
            case 32: return convF32;
            case 64: return convF64;
        }
    }
    return NULL;
}

// Pick the reader by the file's magic bytes rather than its extension:
// "RIFF"/"RF64" -> WavReader, "fLaC" -> FlacReader. Returns NULL for anything
// else. The caller owns the returned reader.
static SampleReader* openReader(const std::string& path) {
    uint8_t magic[4] = { 0 };
    std::ifstream(path, std::ios::binary).read((char*)magic, sizeof(magic));
    if (!memcmp(magic, "fLaC", 4)) { return new FlacReader(path); }
    if (!memcmp(magic, "RIFF", 4) || !memcmp(magic, "RF64", 4)) { return new WavReader(path); }
    return NULL;
}

// The worker assumes all of this, so it must hold before starting one
static bool isPlayable(SampleReader* reader) {
    if (!reader) { return false; }
    int channels = reader->getChannelCount();
    int bytesPerChan = reader->getBitDepth() / 8;
    return getSampleConv(reader->getFormat(), reader->getBitDepth()) != NULL
        && channels >= 1
        && reader->getBlockAlign() >= channels * bytesPerChan
        && reader->getSampleCount() >= 1;
}

// "mm:ss" or "hh:mm:ss", whichever fits the file's duration. The bar and the
// format line must agree on the width, so both sides use this one helper.
static void formatPlaybackTime(double seconds, bool withHours, char* buf, size_t bufSize) {
    if (!(seconds > 0.0)) { seconds = 0.0; }
    uint64_t total = (uint64_t)std::llround(seconds);
    if (withHours) {
        snprintf(buf, bufSize, "%02u:%02u:%02u",
                 (unsigned)(total / 3600), (unsigned)((total / 60) % 60), (unsigned)(total % 60));
    }
    else {
        snprintf(buf, bufSize, "%02u:%02u", (unsigned)((total / 60) % 60), (unsigned)(total % 60));
    }
}

// Media-player style playback bar: elapsed / total centred in the track, a
// progress strip along the bottom and a playhead. A drag reports the target
// sample through seekTarget; the WAV/FLAC readers seek at frame granularity,
// so scrubbing takes effect on the next block the worker reads.
static bool drawPlaybackBar(uint64_t position, uint64_t totalSamples, uint32_t sampleRate, uint64_t* seekTarget) {
    if (totalSamples == 0 || sampleRate == 0) { return false; }

    const float height = ImGui::GetFrameHeight();
    const float width = ImGui::GetContentRegionAvail().x;
    if (width < 1.0f) { return false; }

    ImGui::InvisibleButton("##file_source_position", ImVec2(width, height));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 pMin = ImGui::GetItemRectMin();
    const ImVec2 pMax = ImGui::GetItemRectMax();
    const float trackWidth = pMax.x - pMin.x;

    double fraction = (double)position / (double)totalSamples;
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (active && trackWidth > 0.0f) {
        const float mouseFraction = (ImGui::GetIO().MousePos.x - pMin.x) / trackWidth;
        fraction = (double)std::clamp(mouseFraction, 0.0f, 1.0f);
        *seekTarget = (uint64_t)(fraction * (double)totalSamples);
    }

    const bool withHours = (double)totalSamples / (double)sampleRate >= 3600.0;
    char cur[16], dur[16], label[48];
    formatPlaybackTime((double)(active ? *seekTarget : position) / (double)sampleRate,
                       withHours, cur, sizeof(cur));
    formatPlaybackTime((double)totalSamples / (double)sampleRate, withHours, dur, sizeof(dur));
    snprintf(label, sizeof(label), "%s / %s", cur, dur);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(pMin, pMax, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);

    const float fillX = pMin.x + trackWidth * (float)fraction;
    const ImU32 grabCol = ImGui::GetColorU32(active ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab);
    const float stripH = std::min(height, std::max(2.0f, style::dp(3.0f)));
    if (fraction > 0.0) {
        dl->AddRectFilled(ImVec2(pMin.x, pMax.y - stripH), ImVec2(fillX, pMax.y),
                          grabCol, rounding, ImDrawFlags_RoundCornersBottom);
        dl->AddRectFilled(ImVec2(fillX - 1.0f, pMin.y + 2.0f), ImVec2(fillX + 1.0f, pMax.y - 2.0f),
                          grabCol, 1.0f);
    }
    if (hovered || active) {
        dl->AddRect(pMin, pMax, ImGui::GetColorU32(ImGuiCol_Border), rounding);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    dl->PushClipRect(pMin, pMax, true);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pMin.x + (trackWidth - textSize.x) * 0.5f, pMin.y + (height - textSize.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), label);
    dl->PopClipRect();

    if (hovered && !active) { ImGui::SetTooltip("拖动以定位播放位置"); }
    return active;
}

class FileSourceModule : public ModuleManager::Instance {
public:
    FileSourceModule(std::string name) : fileSelect("", { "IQ 录音 (*.wav *.flac)", "*.wav *.flac", "所有文件", "*" }) {
        this->name = name;

        if (core::args["server"].b()) { return; }

        std::string savedPath = config.read().value("path", std::string());
        fileSelect.setPath(savedPath, true);

        // Load the saved file before the first frame: the playback bar and the
        // file info are then ready immediately, and --autostart can start the
        // file source (it runs before the menu is first drawn).
        if (!savedPath.empty() && !openFile(savedPath, false)) {
            flog::warn("FileSourceModule '{0}': could not load saved file '{1}'", name, savedPath);
        }

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        sigpath::sourceManager.registerSource("File", &handler);
    }

    ~FileSourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource("File");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void menuSelected(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", _this->centerFreq);
        sigpath::iqFrontEnd.setBuffering(false);
        gui::waterfall.centerFrequencyLocked = true;
        //gui::freqSelect.minFreq = _this->centerFreq - (_this->sampleRate/2);
        //gui::freqSelect.maxFreq = _this->centerFreq + (_this->sampleRate/2);
        //gui::freqSelect.limitFreq = true;
        flog::info("FileSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        sigpath::iqFrontEnd.setBuffering(true);
        //gui::freqSelect.limitFreq = false;
        gui::waterfall.centerFrequencyLocked = false;
        flog::info("FileSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        if (_this->running) { return; }
        if (!isPlayable(_this->reader)) { return; }
        _this->positionFrames.store(_this->reader->getSamplePosition());
        _this->seekRequest.store(UINT64_MAX);
        _this->running = true;
        _this->workerThread = std::thread(worker, _this);
        flog::info("FileSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        if (!_this->running) { return; }
        if (_this->reader == NULL) { return; }
        _this->stream.stopWriter();
        _this->workerThread.join();
        _this->stream.clearWriteStop();
        _this->running = false;
        _this->reader->rewind();
        _this->positionFrames.store(0);
        flog::info("FileSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        flog::info("FileSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;

        if (_this->running) { style::beginDisabled(); }
        bool fileChanged = _this->fileSelect.render("##file_source_" + _this->name);
        if (_this->running) { style::endDisabled(); }

        if (fileChanged && _this->fileSelect.pathIsValid()) {
            _this->openFile(_this->fileSelect.path, true);
            config.edit().set("path", _this->fileSelect.path);
        }

        if (!_this->formatStr.empty()) {
            ImGui::TextDisabled("%s", _this->formatStr.c_str());
        }

        // Playback position / seek bar, shown as soon as a file is loaded. It
        // also works while the source is stopped: seeking then just moves the
        // point where the next Start begins (Stop rewinds to 0).
        if (_this->reader != NULL && _this->totalSamples > 0 && _this->fileSampleRate > 0) {
            ImGui::Spacing();
            const uint64_t position = _this->running ? _this->positionFrames.load()
                                                     : _this->reader->getSamplePosition();
            uint64_t seekTarget = position;
            if (drawPlaybackBar(position, _this->totalSamples, _this->fileSampleRate, &seekTarget)) {
                if (_this->running) {
                    // The worker performs the seek before its next read.
                    _this->seekRequest.store(seekTarget);
                }
                else {
                    _this->reader->seek(seekTarget);
                    _this->positionFrames.store(seekTarget);
                }
            }
        }
    }

    // Opens (or reopens) a recording and derives the sample rate and centre
    // frequency from it. applyTuning is false when called from the constructor,
    // before the source is selected: menuSelected() tunes to centerFreq later.
    bool openFile(const std::string& path, bool applyTuning) {
        if (reader != NULL) {
            reader->close();
            delete reader;
            reader = NULL;
        }
        try {
            reader = openReader(path);
            if (reader == NULL) {
                throw std::runtime_error("Unsupported file format (not a WAV or FLAC file)");
            }
            if (!reader->isValid()) {
                throw std::runtime_error("Invalid or damaged file");
            }
            if (reader->getSampleRate() == 0) {
                throw std::runtime_error("Sample rate may not be zero");
            }
            if (!getSampleConv(reader->getFormat(), reader->getBitDepth())) {
                throw std::runtime_error(std::string("Unsupported sample format: ") + reader->getFormatName() + ", " + std::to_string(reader->getBitDepth()) + " bit");
            }
            if (reader->getSampleCount() < 1) {
                throw std::runtime_error("File contains no samples");
            }
            if (!isPlayable(reader)) {
                throw std::runtime_error("Invalid channel layout");
            }
            sampleRate = reader->getSampleRate();
            totalSamples = reader->getSampleCount();
            fileSampleRate = reader->getSampleRate();
            positionFrames.store(0);
            seekRequest.store(UINT64_MAX);
            centerFreq = getFrequency(std::filesystem::path(path).filename().string());
            if (applyTuning) {
                core::setInputSampleRate(sampleRate);
                tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", centerFreq);
            }
            updateFormatStr();
            return true;
        }
        catch (const std::exception& e) {
            flog::error("Error: {}", e.what());
            totalSamples = 0;
            fileSampleRate = 0;
            positionFrames.store(0);
            seekRequest.store(UINT64_MAX);
            if (reader != NULL) {
                reader->close();
                delete reader;
                reader = NULL;
            }
            updateFormatStr();
            return false;
        }
    }

    void updateFormatStr() {
        if (reader == NULL) {
            formatStr = "";
            return;
        }
        uint32_t rate = std::max(reader->getSampleRate(), (uint32_t)1);
        double durationSec = (double)reader->getSampleCount() / (double)rate;
        char duration[16];
        formatPlaybackTime(durationSec, durationSec >= 3600.0, duration, sizeof(duration));
        char buf[128];
        snprintf(buf, sizeof(buf), "%s %u bit, %u ch, %.6g MS/s, %s",
                 reader->getFormatName(), (unsigned)reader->getBitDepth(), (unsigned)reader->getChannelCount(),
                 rate / 1e6, duration);
        formatStr = buf;
    }

    static void worker(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        SampleReader* reader = _this->reader;
        uint32_t sampleRate = std::max(reader->getSampleRate(), (uint32_t)1);
        int blockSize = std::clamp((int)(sampleRate / 200), 1, (int)STREAM_BUFFER_SIZE);
        int channels = reader->getChannelCount();
        int blockAlign = reader->getBlockAlign();
        int bytesPerChan = reader->getBitDepth() / 8;

        // Cap the input buffer at 16 MB: a corrupt header could otherwise
        // declare sizes whose bad_alloc, thrown on this thread, would
        // terminate the app
        blockSize = std::clamp(blockSize, 1, std::max(1, (int)(16777216 / blockAlign)));

        // start() only runs the worker on an isPlayable() reader
        SampleConv conv = getSampleConv(reader->getFormat(), reader->getBitDepth());

        // Stereo float32 is already the stream's format, read it in directly
        bool direct = (conv == convF32 && channels == 2 && blockAlign == sizeof(dsp::complex_t));
        uint8_t* inBuf = direct ? NULL : new uint8_t[(size_t)blockSize * blockAlign];

        // The UI thread never touches the reader while this loop runs; it reads
        // positionFrames and posts seek requests through the two atomics below.
        _this->positionFrames.store(reader->getSamplePosition());

        while (true) {
            uint64_t requested = _this->seekRequest.exchange(UINT64_MAX);
            if (requested != UINT64_MAX) {
                reader->seek(requested);
                _this->positionFrames.store(reader->getSamplePosition());
            }

            // swap() exchanges writeBuf/readBuf, so re-fetch it every block
            void* readDst = direct ? (void*)_this->stream.writeBuf : (void*)inBuf;
            size_t read = reader->readSamples(readDst, (size_t)blockSize * blockAlign);
            if (read < (size_t)blockAlign) {
                // End of file, loop back to the start
                reader->rewind();
                read = reader->readSamples(readDst, (size_t)blockSize * blockAlign);
                if (read < (size_t)blockAlign) { break; }
            }
            int frames = read / blockAlign;
            if (!direct) {
                const uint8_t* src = inBuf;
                dsp::complex_t* dst = _this->stream.writeBuf;
                for (int i = 0; i < frames; i++) {
                    // Mono plays as I=Q; extra channels beyond 2 are ignored
                    float ival = conv(src);
                    dst[i].re = ival;
                    dst[i].im = (channels >= 2) ? conv(src + bytesPerChan) : ival;
                    src += blockAlign;
                }
            }
            _this->positionFrames.store(reader->getSamplePosition());
            if (!_this->stream.swap(frames)) { break; }
        }

        if (inBuf) { delete[] inBuf; }
    }

    double getFrequency(std::string filename) {
        // Match e.g. "14100000Hz" (SDR++, SDR#), "7100kHz" (HDSDR) or
        // "7.100MHz" (SDR Console)
        std::regex expr("([0-9]+(\\.[0-9]+)?)(G|M|k)?Hz");
        std::smatch matches;
        if (!std::regex_search(filename, matches, expr)) { return 0; }
        double freq = std::atof(matches[1].str().c_str());
        std::string unit = matches[3].str();
        if (unit == "G") { return freq * 1e9; }
        if (unit == "M") { return freq * 1e6; }
        if (unit == "k") { return freq * 1e3; }
        return freq;
    }

    FileSelect fileSelect;
    std::string name;
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    SampleReader* reader = NULL;
    bool running = false;
    bool enabled = true;
    float sampleRate = 1000000;
    std::thread workerThread;

    // Playback position and UI->worker seek hand-off. Only the worker thread
    // touches the reader while a file is playing (the UI thread instead reads
    // positionFrames), which keeps the two threads off each other's file I/O.
    std::atomic<uint64_t> positionFrames = 0;
    std::atomic<uint64_t> seekRequest = UINT64_MAX;
    uint64_t totalSamples = 0;
    uint32_t fileSampleRate = 0;

    double centerFreq = 100000000;

    std::string formatStr;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["path"] = "";
    config.setPath(core::args["root"].s() + "/file_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    return new FileSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FileSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
