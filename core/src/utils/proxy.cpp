/*
 * System HTTP proxy discovery. See proxy.h for why this exists.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#include "proxy.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

    std::atomic<bool> proxyToggle{ false };

    std::string trim(const std::string& in) {
        size_t begin = 0;
        size_t end = in.size();
        while (begin < end && std::isspace((unsigned char)in[begin])) { begin++; }
        while (end > begin && std::isspace((unsigned char)in[end - 1])) { end--; }
        return in.substr(begin, end - begin);
    }

    std::string lower(std::string in) {
        for (auto& c : in) { c = (char)std::tolower((unsigned char)c); }
        return in;
    }

    // Accepts "host:port", "http://host:port" and "[v6]:port".
    bool parseHostPort(const std::string& valueIn, std::string& host, int& port) {
        std::string value = trim(valueIn);
        if (value.empty()) { return false; }

        auto schemeEnd = value.find("://");
        if (schemeEnd != std::string::npos) { value = value.substr(schemeEnd + 3); }
        // Drop any path, e.g. "http://proxy:3128/" or socks5 URLs with "/".
        auto slash = value.find('/');
        if (slash != std::string::npos) { value = value.substr(0, slash); }

        // Strip credentials if present ("user:pass@host:port").
        auto at = value.rfind('@');
        if (at != std::string::npos) { value = value.substr(at + 1); }

        auto colon = value.rfind(':');
        if (colon == std::string::npos) { return false; }

        std::string portStr = value.substr(colon + 1);
        if (portStr.empty()) { return false; }
        for (char c : portStr) {
            if (!std::isdigit((unsigned char)c)) { return false; }
        }
        int parsedPort = std::atoi(portStr.c_str());
        if (parsedPort <= 0 || parsedPort > 65535) { return false; }

        host = trim(value.substr(0, colon));
        // Strip IPv6 brackets for the address parser.
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }
        if (host.empty()) { return false; }

        port = parsedPort;
        return true;
    }

    bool wildcardMatch(const std::string& pattern, const std::string& text) {
        // Small '*' glob matcher, enough for Windows ProxyOverride entries such
        // as "*.local" or "192.168.*".
        size_t p = 0;
        size_t t = 0;
        size_t starPos = std::string::npos;
        size_t matchPos = 0;
        while (t < text.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || std::tolower((unsigned char)pattern[p]) == std::tolower((unsigned char)text[t]))) {
                p++;
                t++;
            }
            else if (p < pattern.size() && pattern[p] == '*') {
                starPos = p++;
                matchPos = t;
            }
            else if (starPos != std::string::npos) {
                p = starPos + 1;
                t = ++matchPos;
            }
            else {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*') { p++; }
        return p == pattern.size();
    }

#ifdef _WIN32
    std::string readRegistryString(HKEY key, const wchar_t* name) {
        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) { return {}; }
        if (type != REG_SZ && type != REG_EXPAND_SZ) { return {}; }
        std::wstring wbuf(size / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)wbuf.data(), &size) != ERROR_SUCCESS) { return {}; }
        while (!wbuf.empty() && wbuf.back() == L'\0') { wbuf.pop_back(); }
        if (wbuf.empty()) { return {}; }

        int needed = WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), (int)wbuf.size(), nullptr, 0, nullptr, nullptr);
        std::string out(needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wbuf.c_str(), (int)wbuf.size(), out.data(), needed, nullptr, nullptr);
        return out;
    }

    bool readRegistryDword(HKEY key, const wchar_t* name, DWORD& value) {
        DWORD type = 0;
        DWORD size = sizeof(value);
        if (RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)&value, &size) != ERROR_SUCCESS) { return false; }
        return type == REG_DWORD;
    }
#endif

}  // namespace

namespace net::proxy {

    Settings systemSettings() {
        Settings settings;

#ifdef _WIN32
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                          0, KEY_READ, &key) == ERROR_SUCCESS) {
            DWORD enabled = 0;
            const bool hasEnable = readRegistryDword(key, L"ProxyEnable", enabled);
            const std::string server = readRegistryString(key, L"ProxyServer");
            settings.bypass = readRegistryString(key, L"ProxyOverride");
            RegCloseKey(key);

            if (hasEnable && enabled && !server.empty()) {
                // ProxyServer may be "host:port" or a per-scheme list such as
                // "http=host:port;https=host:port;socks=host:port".
                std::string candidate = server;
                if (server.find('=') != std::string::npos) {
                    candidate.clear();
                    size_t pos = 0;
                    while (pos <= server.size()) {
                        size_t next = server.find(';', pos);
                        std::string entry = server.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
                        auto eq = entry.find('=');
                        if (eq != std::string::npos) {
                            std::string scheme = lower(trim(entry.substr(0, eq)));
                            if (scheme == "http") {
                                candidate = entry.substr(eq + 1);
                                break;
                            }
                            if (scheme == "https" && candidate.empty()) { candidate = entry.substr(eq + 1); }
                        }
                        if (next == std::string::npos) { break; }
                        pos = next + 1;
                    }
                }
                parseHostPort(candidate, settings.host, settings.port);
            }
        }
#endif

        // Environment fallback: also the only source on Linux/macOS/Android.
        if (!settings.valid()) {
            const char* names[] = { "http_proxy", "HTTP_PROXY", "all_proxy", "ALL_PROXY" };
            for (const char* name : names) {
                const char* value = std::getenv(name);
                if (!value || !*value) { continue; }
                std::string host;
                int port = 0;
                if (parseHostPort(value, host, port)) {
                    settings.host = host;
                    settings.port = port;
                    break;
                }
            }
        }

        if (!settings.valid()) { settings = Settings(); }
        return settings;
    }

    void setUseSystemProxy(bool enable) { proxyToggle.store(enable); }

    bool useSystemProxy() { return proxyToggle.load(); }

    Settings active() {
        if (!proxyToggle.load()) { return Settings(); }
        return systemSettings();
    }

    bool bypasses(const Settings& settings, const std::string& host) {
        if (settings.bypass.empty() || host.empty()) { return false; }

        const std::string hostLower = lower(host);
        bool hostHasDot = hostLower.find('.') != std::string::npos;

        size_t pos = 0;
        const std::string& list = settings.bypass;
        while (pos <= list.size()) {
            size_t next = list.find_first_of(";,", pos);
            std::string entry = trim(list.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
            if (!entry.empty()) {
                if (lower(entry) == "<local>") {
                    // Windows: plain host names (no dot) bypass the proxy.
                    if (!hostHasDot) { return true; }
                }
                else if (wildcardMatch(entry, host)) {
                    return true;
                }
            }
            if (next == std::string::npos) { break; }
            pos = next + 1;
        }
        return false;
    }

    std::string toUrl(const Settings& settings) {
        if (!settings.valid()) { return {}; }
        return "http://" + settings.host + ":" + std::to_string(settings.port);
    }

    std::shared_ptr<Socket> connect(const std::string& host, int port, int timeoutMs,
                                    const Settings& settings) {
        if (!settings.valid() || bypasses(settings, host)) {
            return net::connect(host, port, timeoutMs);
        }

        auto sock = net::connect(settings.host, settings.port, timeoutMs);
        if (!sock) { return nullptr; }

        const std::string authority = host + ":" + std::to_string(port);
        const std::string request =
            "CONNECT " + authority + " HTTP/1.1\r\n"
            "Host: " + authority + "\r\n"
            "Proxy-Connection: Keep-Alive\r\n\r\n";
        if (sock->sendstr(request) < 1) { return nullptr; }

        std::string line;
        if (sock->recvline(line, 1024, timeoutMs) < 1) { return nullptr; }

        const auto space = line.find(' ');
        const int status = (space == std::string::npos) ? 0 : std::atoi(line.c_str() + space + 1);

        // Drain the rest of the response head so the tunneled stream starts at
        // the first byte of the target's own response.
        while (true) {
            if (sock->recvline(line, 1024, timeoutMs) < 1) { break; }
            if (line == "\r" || line.empty()) { break; }
        }

        if (status != 200) {
            sock->close();
            return nullptr;
        }
        return sock;
    }
}

