#pragma once

#include <M5Cardputer.h>
#include <WiFi.h>
#include <vector>
#include <string>
#include <functional>
#include <ESP32Ping.h>
#include <lwip/etharp.h>
#include <lwip/sockets.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

using LineCallback = std::function<void(const std::string&)>;

class NetworkCmds {
public:
    static void net_scan(LineCallback emit);
    static void net_scan_ports(const IPAddress ip, const std::vector<uint16_t>& ports, LineCallback emit);
    static void ping(const std::string& target, uint8_t count, LineCallback emit);

    // wget <url> [-o <path>] : download a URL to a file (streamed, not buffered).
    static void wget(const std::string& args, LineCallback emit);
    // Derive a local filename from a URL (strips scheme/query/fragment; "" -> index.html).
    static std::string filename_from_url(const std::string& url);
};
