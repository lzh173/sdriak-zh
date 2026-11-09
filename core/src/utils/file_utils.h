#pragma once
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <stdint.h>

// Paths inside the application are kept as narrow std::string (config JSON,
// resource directory resolution, ...).  On Windows the narrow file APIs
// (std::ifstream(const char*), fopen, stb_image, ...) interpret those bytes
// with the ANSI code page, so an installation under a directory whose name is
// not representable there (for example a Chinese folder name) fails to load
// its resources even though the path is valid.  std::filesystem::path uses the
// wide Win32 API, so every file access in the core goes through these helpers.
namespace utils {
    // Narrow path strings reach us in two flavours: std::filesystem::path::string()
    // (the ANSI code page on Windows) for paths the app derives itself, and
    // UTF-8 for paths returned by the native file dialogs. GBK-encoded Windows
    // paths are practically never valid UTF-8, so use that to tell them apart.
    inline bool isValidUtf8(const std::string& text) {
        size_t i = 0;
        while (i < text.size()) {
            const unsigned char c = (unsigned char)text[i];
            size_t extra = 0;
            if (c < 0x80) { i++; continue; }
            else if ((c & 0xE0) == 0xC0) { extra = 1; }
            else if ((c & 0xF0) == 0xE0) { extra = 2; }
            else if ((c & 0xF8) == 0xF0) { extra = 3; }
            else { return false; }
            if (i + extra >= text.size()) { return false; }
            for (size_t n = 1; n <= extra; n++) {
                if (((unsigned char)text[i + n] & 0xC0) != 0x80) { return false; }
            }
            i += extra + 1;
        }
        return true;
    }

    inline std::filesystem::path toPath(const std::string& path) {
#ifdef _WIN32
        if (isValidUtf8(path)) {
            return std::filesystem::u8path(path);
        }
#endif
        return std::filesystem::path(path);
    }

    inline std::ifstream openRead(const std::string& path, std::ios::openmode mode = std::ios::in) {
        return std::ifstream(toPath(path), mode | std::ios::binary);
    }

    inline std::ofstream openWrite(const std::string& path, std::ios::openmode mode = std::ios::out) {
        return std::ofstream(toPath(path), mode | std::ios::binary);
    }

    inline FILE* fopenPath(const std::string& path, const char* mode) {
#ifdef _WIN32
        std::wstring wideMode(mode, mode + std::strlen(mode));
        return _wfopen(toPath(path).c_str(), wideMode.c_str());
#else
        return fopen(path.c_str(), mode);
#endif
    }

    // Reads a whole file; returns false if it cannot be opened or read.
    inline bool readFile(const std::string& path, std::vector<uint8_t>& out) {
        std::ifstream file = openRead(path);
        if (!file) { return false; }
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        if (size < 0) { return false; }
        file.seekg(0, std::ios::beg);
        out.resize((size_t)size);
        if (size > 0) {
            file.read((char*)out.data(), size);
            if (!file) { return false; }
        }
        return true;
    }
}
