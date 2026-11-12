/*
 * System HTTP proxy discovery.
 *
 * SDRIAK talks to KiwiSDR servers over raw sockets (directory fetch and the
 * WebSocket client), so libcurl's own proxy handling never applies to them.
 * On Windows the proxy is whatever WinINET/the system settings say; elsewhere
 * the conventional http_proxy/https_proxy environment variables are used.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#pragma once

#include <memory>
#include <string>

#include "net.h"

namespace net::proxy {

    struct Settings {
        std::string host;
        int port = 0;
        // Semicolon separated bypass list in the platform's own syntax
        // (Windows ProxyOverride, or a comma separated no_proxy value).
        std::string bypass;

        bool valid() const { return !host.empty() && port > 0; }
    };

    // Proxy configured for this system, or an invalid Settings when none is.
    Settings systemSettings();

    // Global toggle, persisted in the KiwiSDR module config. When off, active()
    // reports no proxy and connections go out directly.
    void setUseSystemProxy(bool enable);
    bool useSystemProxy();

    // Effective proxy: systemSettings() when the toggle is on, else disabled.
    Settings active();

    // True when the bypass list says <host> should be reached directly.
    bool bypasses(const Settings& settings, const std::string& host);

    // "http://host:port", or an empty string when the settings are disabled.
    std::string toUrl(const Settings& settings);

    // Connects to host:port directly, or through `settings` with an HTTP
    // CONNECT tunnel when they describe a usable proxy. Returns nullptr on
    // failure, exactly like net::connect().
    std::shared_ptr<Socket> connect(const std::string& host, int port, int timeoutMs,
                                    const Settings& settings);
}
