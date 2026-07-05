#pragma once

#include <M5Cardputer.h>
#include <WiFi.h>
#include <string>
#include <functional>

using LineCallback = std::function<void(const std::string&)>;

// Air monitor: passive 802.11 sniffing (and, later, other RF). "air sniff"
// hops the 2.4 GHz channels for a while collecting access points, their
// associated clients, and unassociated (probing) stations, then prints a
// report. Passive only - it reads frame headers, never injects or deauths.
class AirCmds {
public:
    // Sniff for `seconds` (0 -> default). Breaks the WiFi connection while it
    // runs (promiscuous mode owns the radio). Interruptible with Ctrl+C.
    static void sniff(uint32_t seconds, LineCallback emit);

    // Live monitor of a single AP (given by SSID or BSSID): shows clients
    // joining/leaving in real time, each line time-stamped. A client is
    // considered gone after `timeoutSec` of silence (or on an explicit
    // deauth/disassoc). `poll` is called frequently: it scrolls the view and
    // returns true when the user asks to stop (Ctrl+C).
    static void watch(const std::string& target, uint32_t timeoutSec,
                      LineCallback emit, std::function<bool()> poll);
};
