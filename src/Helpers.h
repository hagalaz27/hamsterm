#pragma once

#include <M5Cardputer.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <vector>
#include <utility>
#include <string>
#include <functional>
#include <ESP32Ping.h>
#include <lwip/etharp.h>
#include <lwip/sockets.h>
#include <HTTPClient.h>
#include <time.h>

using LineCallback = std::function<void(const std::string&)>;

class Helpers {

    public:
    static std::string currentDir;
    static std::vector<std::string> lastOutput;
    static const char* ntpServer;
    static const long gmtOffset_sec;
    static const int daylightOffset_sec;

    // --- microSD mount point ---
    static bool sdMounted;
    static const char* SD_MOUNT; // "/sd"

    // Whether an absolute path points to SD ("/sd" or "/sd/...").
    static bool isSdPath(const std::string& abs);
    // Returns the matching FS (SD or LittleFS) and the path translated for it.
    static fs::FS& fsFor(const std::string& abs, std::string& fsPath);

    // FS wrappers that route by absolute path.
    static File fsOpen(const std::string& abs, const char* mode = "r");
    static bool fsExists(const std::string& abs);
    static bool fsMkdir(const std::string& abs);
    static bool fsRmdir(const std::string& abs);
    static bool fsRemove(const std::string& abs);
    static bool fsRename(const std::string& from, const std::string& to);

    // mount / umount
    static bool mountSD(LineCallback emit);
    static void umountSD(LineCallback emit);

    static size_t utf8_strlen(std::string& str);
    static std::string utf8_substr(std::string& str, size_t max_chars);
    static std::string clearFilename(std::string filename);
    static void trim(std::string& s);
    static std::string make_absolute(const std::string& path);
    static std::vector<std::string> parse_parts(const std::string& args);

    // --- quote-aware tokenizing (shell-style) ---
    // tokenize: split on unquoted whitespace, honoring "..." and '...'; quotes
    //   are removed. tokenize_ex also reports whether each token contained a
    //   '*'/'?' OUTSIDE quotes (so quoting suppresses globbing).
    static std::vector<std::string> tokenize(const std::string& s);
    static std::vector<std::pair<std::string, bool>> tokenize_ex(const std::string& s);
    // First index of `ch` that lies outside any quotes, or npos.
    static size_t find_unquoted(const std::string& s, char ch, size_t from = 0);
    // First whitespace-token of s with quotes removed (for redirect targets).
    static std::string strip_quotes(const std::string& s);
    // Wrap in double quotes if the string contains whitespace (else unchanged).
    static std::string requote(const std::string& s);

    static void checkConnection(LineCallback emit);
    static std::string getHostname(IPAddress ip);
    static std::string getSSDPInfo(IPAddress ip);
    static std::string getMDNSHostname(IPAddress ip);
    static std::string getVendorFromAPI(String mac);

    static void initTime();

};
