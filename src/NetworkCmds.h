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
    // Ping a host. count == 0 means ping forever (until Ctrl+C).
    static void ping(const std::string& target, uint32_t count, LineCallback emit);

    // Resolve a host|ip|url target to an IPAddress. Return code:
    //   0 = ok (out set), 1 = empty/invalid target,
    //   2 = WiFi not connected (needed for DNS), 3 = DNS resolution failed.
    // `host` receives the extracted hostname (useful for error messages).
    static int resolve_target(const std::string& target, IPAddress& out, std::string& host);

    // Space-separated list of the ~1000 most common TCP ports (with ranges),
    // used by "net s p <host>" when no ports are given.
    static const char* default_ports();

    // wget <url> [-o <path>] : download a URL to a file (streamed, not buffered).
    static void wget(const std::string& args, LineCallback emit);
    // Derive a local filename from a URL (strips scheme/query/fragment; "" -> index.html).
    static std::string filename_from_url(const std::string& url);
};
