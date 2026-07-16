#include "SystemCmds.h"
#include "Helpers.h"
#include "Version.h"

#include <M5Cardputer.h>
#include <LittleFS.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>

// Human-readable size: B / K / M / G.
static std::string humanBytes(uint64_t b) {
    char t[24];
    if (b < 1024ULL) {
        snprintf(t, sizeof(t), "%lluB", (unsigned long long)b);
    } else if (b < 1024ULL * 1024) {
        snprintf(t, sizeof(t), "%.1fK", b / 1024.0);
    } else if (b < 1024ULL * 1024 * 1024) {
        snprintf(t, sizeof(t), "%.1fM", b / (1024.0 * 1024));
    } else {
        snprintf(t, sizeof(t), "%.2fG", b / (1024.0 * 1024 * 1024));
    }
    return std::string(t);
}

void SystemCmds::help(LineCallback emit) {
    emit("hamsTerm commands:\n");
    emit("Files: ls [-a] cd pwd cat\n");
    emit("       head tail find\n");
    emit("       touch mkdir rmdir\n");
    emit("       rm[-r] cp[-r] mv\n");
    emit("       echo date clear\n");
    emit("Edit:  edit/ed <file>\n");
    emit("Disk:  mount umount df\n");
    emit("WiFi:  wf s | wf c <ssid> [-p pw]\n");
    emit("       wf dc\n");
    emit("AP:    ap -s <ssid> [-p pw] start\n");
    emit("       ap stop | ap status\n");
    emit("Net:   net s | net s p <host> [ports]\n");
    emit("       air s [secs]\n");
    emit("       air w <ssid|bssid>\n");
    emit("Sig:   beep [n] [dur] [gap]\n");
    emit("       led <color> [n] [dur] [gap]\n");
    emit("       ping [-c N] <host>\n");
    emit("       wget <url> [-o path]\n");
    emit("       httpd [start [path]|stop|status]\n");
    emit("       telnet <host> [port]\n");
    emit("       nc <host> <port>\n");
    emit("       ssh [user@]host [port]\n");
    emit("Sys:   help sysinfo free battery\n");
    emit("       clear | reboot | sh [-v] <file> [args]\n");
    emit("       sleep <seconds>\n");
    emit("Vars:  set [NAME val] | NAME=val\n");
    emit("       read [-p ask] name...\n");
    emit("       unset NAME | $NAME expands\n");
    emit("       $1..$9 $@ $# (script args)\n");
    emit("       $? = last exit code\n");
    emit("Pipe:  cmd > f | cmd >> f\n");
    emit("       a | grep <t> | cut -f N\n");
    emit("       a | tr [-d|-s] SET1 [SET2]\n");
    emit("       a | wc [-l] [-w] [-c]\n");
    emit("       a && b | a || b\n");
    emit("Test:  [ -f x ] [ a = b ]\n");
    emit("Flow:  if C; then ...; fi\n");
    emit("       while C; do ...; done\n");
    emit("       for x in ...; do ...; done\n");
    emit("Range: {1..5} {0..10..2}\n");
    emit("Math:  $((i+1)) + - * / %\n");
    emit("Subst: x=$(cmd)  $(cmd)\n");
    emit("       -e -f -d -s -z -n\n");
    emit("       = != -eq -lt -gt ...\n");
    emit("Keys:  Tab=complete\n");
    emit("       Fn+;/. = history\n");
    emit("       Fn+,// = cursor\n");
    emit("       Ctrl+;/. = scroll\n");
}

// Query the public (WAN) IP from an external service. Returns "" on any
// failure or timeout. Blocking, with a short timeout so sysinfo stays snappy.
static std::string fetch_wan_ip() {
    if (WiFi.status() != WL_CONNECTED) return "";
    HTTPClient http;
    WiFiClient client;
    if (!http.begin(client, "http://api.ipify.org")) return "";
    http.setUserAgent("hamsTerm/1.0");
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    std::string out;
    if (http.GET() == HTTP_CODE_OK) {
        String s = http.getString();
        s.trim();
        // sanity: looks like an IPv4 (digits and dots, short)
        bool ok = s.length() >= 7 && s.length() <= 15;
        for (size_t i = 0; ok && i < s.length(); ++i) {
            char c = s[i];
            if (!((c >= '0' && c <= '9') || c == '.')) ok = false;
        }
        if (ok) out = std::string(s.c_str());
    }
    http.end();
    return out;
}

void SystemCmds::sysinfo(LineCallback emit) {
    char buf[96];

    snprintf(buf, sizeof(buf), "hamsTerm v%s\n", HAMSTERM_VERSION);
    emit(buf);

    snprintf(buf, sizeof(buf), "Chip: %s rev%d\n",
             ESP.getChipModel(), (int)ESP.getChipRevision());
    emit(buf);

    snprintf(buf, sizeof(buf), "Cores: %d @ %u MHz\n",
             (int)ESP.getChipCores(), (unsigned)ESP.getCpuFreqMHz());
    emit(buf);

    snprintf(buf, sizeof(buf), "Flash: %s\n",
             humanBytes(ESP.getFlashChipSize()).c_str());
    emit(buf);

    snprintf(buf, sizeof(buf), "Heap free: %s / %s\n",
             humanBytes(ESP.getFreeHeap()).c_str(),
             humanBytes(ESP.getHeapSize()).c_str());
    emit(buf);

    size_t psram = ESP.getPsramSize();
    if (psram > 0) {
        snprintf(buf, sizeof(buf), "PSRAM: %s\n", humanBytes(psram).c_str());
        emit(buf);
    } else {
        emit("PSRAM: none\n");
    }

    snprintf(buf, sizeof(buf), "MAC: %s\n", WiFi.macAddress().c_str());
    emit(buf);

    unsigned long s = millis() / 1000UL;
    snprintf(buf, sizeof(buf), "Uptime: %luh %lum %lus\n",
             s / 3600UL, (s % 3600UL) / 60UL, s % 60UL);
    emit(buf);

    bool sta = (WiFi.status() == WL_CONNECTED);
    wifi_mode_t mode = WiFi.getMode();
    bool apUp = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);

    if (sta) {
        snprintf(buf, sizeof(buf), "WiFi: %s (RSSI %d)\n",
                 WiFi.SSID().c_str(), (int)WiFi.RSSI());
        emit(buf);
        snprintf(buf, sizeof(buf), "LAN IP: %s\n",
                 WiFi.localIP().toString().c_str());
        emit(buf);
    } else {
        emit("WiFi: not connected\n");
    }

    if (apUp) {
        snprintf(buf, sizeof(buf), "AP IP:  %s\n",
                 WiFi.softAPIP().toString().c_str());
        emit(buf);
    }

    // WAN IP needs an external request; only attempt it when connected as a
    // station (AP-only mode has no route to the internet).
    if (sta) {
        std::string wan = fetch_wan_ip();
        snprintf(buf, sizeof(buf), "WAN IP: %s\n",
                 wan.empty() ? "n/a" : wan.c_str());
        emit(buf);
    }
}

void SystemCmds::freemem(LineCallback emit) {
    char buf[80];

    uint32_t total = ESP.getHeapSize();
    uint32_t freeNow = ESP.getFreeHeap();

    snprintf(buf, sizeof(buf), "Heap total: %s\n", humanBytes(total).c_str());
    emit(buf);

    int pct = total ? (int)((uint64_t)freeNow * 100 / total) : 0;
    snprintf(buf, sizeof(buf), "Heap free:  %s (%d%%)\n",
             humanBytes(freeNow).c_str(), pct);
    emit(buf);

    snprintf(buf, sizeof(buf), "Min free:   %s\n",
             humanBytes(ESP.getMinFreeHeap()).c_str());
    emit(buf);

    snprintf(buf, sizeof(buf), "Max block:  %s\n",
             humanBytes(ESP.getMaxAllocHeap()).c_str());
    emit(buf);

    size_t psram = ESP.getPsramSize();
    if (psram > 0) {
        snprintf(buf, sizeof(buf), "PSRAM free: %s / %s\n",
                 humanBytes(ESP.getFreePsram()).c_str(),
                 humanBytes(psram).c_str());
        emit(buf);
    }
}

void SystemCmds::df(LineCallback emit) {
    // Internal storage (LittleFS)
    uint64_t lt = LittleFS.totalBytes();
    uint64_t lu = LittleFS.usedBytes();
    int lp = lt ? (int)(lu * 100 / lt) : 0;

    emit("Flash (/):\n");
    emit("  " + humanBytes(lu) + " / " + humanBytes(lt) +
         " (" + std::to_string(lp) + "%)\n");

    // microSD
    if (Helpers::sdMounted) {
        uint64_t st = SD.totalBytes();
        uint64_t su = SD.usedBytes();
        int sp = st ? (int)(su * 100 / st) : 0;
        emit("SD (/sd):\n");
        emit("  " + humanBytes(su) + " / " + humanBytes(st) +
             " (" + std::to_string(sp) + "%)\n");
    } else {
        emit("SD (/sd): not mounted\n");
    }
}

void SystemCmds::battery(LineCallback emit) {
    char buf[64];

    int level = M5.Power.getBatteryLevel();   // 0..100 or -1 (unknown)
    int mv    = M5.Power.getBatteryVoltage(); // mV

    if (level < 0) {
        emit("Battery: level unknown\n");
    } else {
        snprintf(buf, sizeof(buf), "Battery: %d%%\n", level);
        emit(buf);
    }

    if (mv > 0) {
        snprintf(buf, sizeof(buf), "Voltage: %.2f V\n", mv / 1000.0);
        emit(buf);
    }

    int chg = (int)M5.Power.isCharging();
    if (chg == 1) {
        emit("Status: charging\n");
    } else if (chg == 0) {
        emit("Status: discharging\n");
    } else {
        emit("Status: unknown\n");
    }
}
