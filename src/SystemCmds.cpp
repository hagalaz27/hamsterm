#include "SystemCmds.h"
#include "Helpers.h"
#include "Version.h"

#include <M5Cardputer.h>
#include <LittleFS.h>
#include <SD.h>
#include <WiFi.h>

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
    emit("Net:   net s | net s p <ip> <ports>\n");
    emit("       ping <host> [n]\n");
    emit("       telnet <host> [port]\n");
    emit("       nc <host> <port>\n");
    emit("       ssh [user@]host [port]\n");
    emit("Sys:   help sysinfo free battery\n");
    emit("Vars:  set [NAME val] | NAME=val\n");
    emit("       unset NAME | $NAME expands\n");
    emit("Pipe:  cmd > f | cmd >> f\n");
    emit("       cmd | grep <text>\n");
    emit("Keys:  Tab=complete\n");
    emit("       Fn+;/. = history\n");
    emit("       Fn+,// = scroll\n");
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

    if (WiFi.status() == WL_CONNECTED) {
        snprintf(buf, sizeof(buf), "WiFi: %s\n", WiFi.SSID().c_str());
        emit(buf);
        snprintf(buf, sizeof(buf), "IP: %s (RSSI %d)\n",
                 WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        emit(buf);
    } else {
        emit("WiFi: not connected\n");
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
