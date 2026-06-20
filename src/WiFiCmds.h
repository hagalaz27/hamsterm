#pragma once

#include <M5Cardputer.h>
#include <WiFi.h>
#include <vector>
#include <string>
#include <functional>

using LineCallback = std::function<void(const std::string&)>;

class WiFiCmds {
public:
    static bool waitingForPass;
    static std::string pendingSSID;

    static void wifi_scan(LineCallback emit);
    static void wifi_connect(std::string ssid, LineCallback emit);
    static void wifi_connect_with_pass(std::string password, LineCallback emit);
    static void wifi_disconnect(LineCallback emit);

    // SoftAP (access point) mode.
    static void ap_start(const std::string& ssid, const std::string& password, LineCallback emit);
    static void ap_stop(LineCallback emit);
    static void ap_status(LineCallback emit);
};
