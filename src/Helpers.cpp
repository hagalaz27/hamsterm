#include "Helpers.h"
#include <utility>

std::string Helpers::currentDir = "/";
std::vector<std::string> Helpers::lastOutput;
const char* Helpers::ntpServer = "pool.ntp.org";
const long Helpers::gmtOffset_sec = 2 * 3600;     // UTC+2 (Ukraine)
const int Helpers::daylightOffset_sec = 3600;

bool Helpers::sdMounted = false;
const char* Helpers::SD_MOUNT = "/sd";

/* ===================== FS router (LittleFS / SD) ===================== */

bool Helpers::isSdPath(const std::string& abs) {
    return abs == "/sd" || abs.rfind("/sd/", 0) == 0;
}

fs::FS& Helpers::fsFor(const std::string& abs, std::string& fsPath) {
    if (isSdPath(abs)) {
        // "/sd" -> "/", "/sd/foo/bar" -> "/foo/bar"
        fsPath = abs.substr(3);
        if (fsPath.empty()) fsPath = "/";
        return SD;
    }
    fsPath = abs;
    return (fs::FS&)LittleFS;
}

File Helpers::fsOpen(const std::string& abs, const char* mode) {
    std::string p;
    fs::FS& F = fsFor(abs, p);
    return F.open(p.c_str(), mode);
}

bool Helpers::fsExists(const std::string& abs) {
    std::string p;
    fs::FS& F = fsFor(abs, p);
    return F.exists(p.c_str());
}

bool Helpers::fsMkdir(const std::string& abs) {
    std::string p;
    fs::FS& F = fsFor(abs, p);
    return F.mkdir(p.c_str());
}

bool Helpers::fsRmdir(const std::string& abs) {
    std::string p;
    fs::FS& F = fsFor(abs, p);
    return F.rmdir(p.c_str());
}

bool Helpers::fsRemove(const std::string& abs) {
    std::string p;
    fs::FS& F = fsFor(abs, p);
    return F.remove(p.c_str());
}

bool Helpers::fsRename(const std::string& from, const std::string& to) {
    // rename only works within a single FS. Between LittleFS and SD
    // return false so the caller falls back to copy+remove.
    if (isSdPath(from) != isSdPath(to)) return false;
    std::string a, b;
    fs::FS& F = fsFor(from, a);
    fsFor(to, b); // translated destination path (same FS)
    return F.rename(a.c_str(), b.c_str());
}

/* ===================== mount / umount ===================== */

bool Helpers::mountSD(LineCallback emit) {
    if (sdMounted) {
        emit("SD already mounted at /sd\n");
        return true;
    }

    // microSD pins on the M5Cardputer (change here for other board revisions):
    //   SCK=40, MISO=39, MOSI=14, CS=12
    const int SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14, SD_CS = 12;

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    if (!SD.begin(SD_CS, SPI)) {
        emit("mount: SD card not found\n");
        SPI.end();
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        emit("mount: no SD card\n");
        SD.end();
        SPI.end();
        return false;
    }

    sdMounted = true;

    const char* typeStr = (cardType == CARD_MMC)  ? "MMC" :
                          (cardType == CARD_SD)   ? "SDSC" :
                          (cardType == CARD_SDHC) ? "SDHC" : "UNKNOWN";

    char buf[64];
    uint64_t sizeMB = SD.cardSize() / (1024ULL * 1024ULL);
    snprintf(buf, sizeof(buf), "SD mounted at /sd (%s, %lluMB)\n", typeStr, (unsigned long long)sizeMB);
    emit(buf);
    return true;
}

void Helpers::umountSD(LineCallback emit) {
    if (!sdMounted) {
        emit("umount: nothing mounted\n");
        return;
    }

    SD.end();
    SPI.end();
    sdMounted = false;

    // If we are inside /sd, fall back to the internal FS root.
    if (isSdPath(currentDir)) {
        currentDir = "/";
    }

    emit("SD unmounted\n");
}

size_t Helpers::utf8_strlen(std::string& str) {
    size_t len = 0;
    for (size_t i = 0; i < str.length(); i++)
        if ((str[i] & 0xC0) != 0x80) len++;
    return len;
}

std::string Helpers::utf8_substr(std::string& str, size_t max_chars) {
    std::string res;
    size_t count = 0;
    for (size_t i = 0; i < str.length() && count < max_chars; ) {
        unsigned char c = str[i];
        size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        res += str.substr(i, len);
        i += len;
        count++;
    }
    return res;
}

std::string Helpers::clearFilename(std::string filename) {
    // FIX: substr(1) on an empty string threw std::out_of_range.
    if (filename.empty()) return filename;
    if (filename.front() == '/') return filename.substr(1);
    return filename;
}

void Helpers::trim(std::string& s) {
    while (!s.empty() && s.front() == ' ')
        s.erase(0, 1);

    while (!s.empty() && s.back() == ' ')
        s.pop_back();
}

std::string normalize_path(const std::string& path) {
        std::vector<std::string> parts;
        std::string temp;

        for (size_t i = 0; i <= path.size(); i++) {
            if (i == path.size() || path[i] == '/') {
                if (temp == "..") {
                    if (!parts.empty())
                        parts.pop_back();
                }
                else if (!temp.empty() && temp != ".") {
                    parts.push_back(temp);
                }
                temp.clear();
            } else {
                temp += path[i];
            }
        }

        std::string result = "/";
            for (size_t i = 0; i < parts.size(); i++) {
                result += parts[i];
                if (i + 1 < parts.size()) result += "/";
            }

        return result;
    }

std::string Helpers::make_absolute(const std::string& path) {
    std::string newPath = path;
    newPath.erase(0, newPath.find_first_not_of(" "));
    size_t lastNonSpace = newPath.find_last_not_of(" ");
    if (lastNonSpace != std::string::npos)
        newPath.erase(lastNonSpace + 1);
    else
        newPath.clear();

    if (newPath.empty()) return Helpers::currentDir;

    std::string fullPath;
    if (newPath[0] == '/') {
        fullPath = newPath;
    } else {
        if (Helpers::currentDir == "/")
            fullPath = "/" + newPath;
        else
            fullPath = Helpers::currentDir + "/" + newPath;
    }

    return normalize_path(fullPath);
}

std::vector<std::pair<std::string, bool>> Helpers::tokenize_ex(const std::string& s) {
    std::vector<std::pair<std::string, bool>> out;
    std::string cur;
    bool inWord = false, glob = false;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == ' ' || c == '\t') {
            if (inWord) { out.push_back(std::make_pair(cur, glob)); cur.clear(); inWord = false; glob = false; }
            i++;
        } else if (c == '"' || c == '\'') {
            char q = c; i++; inWord = true;
            while (i < s.size() && s[i] != q) { cur += s[i]; i++; } // inside quotes: literal
            if (i < s.size()) i++; // skip closing quote
        } else {
            inWord = true;
            if (c == '*' || c == '?') glob = true; // unquoted glob char
            cur += c; i++;
        }
    }
    if (inWord) out.push_back(std::make_pair(cur, glob));
    return out;
}

std::vector<std::string> Helpers::tokenize(const std::string& s) {
    std::vector<std::string> out;
    for (auto& t : tokenize_ex(s)) out.push_back(t.first);
    return out;
}

size_t Helpers::find_unquoted(const std::string& s, char ch, size_t from) {
    bool inS = false, inD = false;
    for (size_t i = from; i < s.size(); i++) {
        char c = s[i];
        if (c == '\'' && !inD) inS = !inS;
        else if (c == '"' && !inS) inD = !inD;
        else if (c == ch && !inS && !inD) return i;
    }
    return std::string::npos;
}

std::string Helpers::strip_quotes(const std::string& s) {
    auto toks = tokenize(s);
    return toks.empty() ? std::string("") : toks[0];
}

std::string Helpers::requote(const std::string& s) {
    if (s.empty() || s.find(' ') != std::string::npos || s.find('\t') != std::string::npos)
        return "\"" + s + "\"";
    return s;
}

std::vector<std::string> Helpers::parse_parts(const std::string& args) {
    std::vector<std::string> result;
    for (auto& tok : tokenize(args)) {
        if (!tok.empty()) result.push_back(make_absolute(tok));
    }
    return result;
}


void Helpers::checkConnection(LineCallback emit) {
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500);
        retry++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer),
                "Connected! IP: %s\n",
                WiFi.localIP().toString().c_str());

        emit(buffer);
        initTime();
    } else {
        emit("Failed to connect.\n");
        // Stop the background retry loop: otherwise the driver keeps trying
        // with the wrong credentials, which also breaks the next Wi-Fi scan.
        WiFi.disconnect(false, false);
    }
}

std::string Helpers::getHostname(IPAddress ip) {
    WiFiUDP udp;
    udp.begin(137);
    uint8_t packet[] = {
        0x80, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x20, 0x43, 0x4b, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
        0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
        0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x00, 0x00, 0x21,
        0x00, 0x01
    };

    udp.beginPacket(ip, 137);
    udp.write(packet, sizeof(packet));
    udp.endPacket();

    unsigned long start = millis();
    while (millis() - start < 500) {
        int size = udp.parsePacket();
        if (size > 72) {
            std::vector<uint8_t> res(size);
            udp.read(res.data(), size);

            char name[16] = {0};
            memcpy(name, &res[57], 15);

            std::string n(name);
            n.erase(0, n.find_first_not_of(' '));
            size_t last = n.find_last_not_of(' ');
            if (last != std::string::npos) n.erase(last + 1);

            if (n.length() > 0) {
                return n;
            }
        }
    }
    return "";
}

std::string Helpers::getSSDPInfo(IPAddress ip) {
    WiFiUDP udp;
    udp.begin(1900);

     const char* ssdpQuery =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: ssdp:all\r\n\r\n";

    udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
    udp.write((uint8_t*)ssdpQuery, strlen(ssdpQuery));
    udp.endPacket();

    unsigned long start = millis();
    while (millis() - start < 600) {
        int size = udp.parsePacket();
        if (size > 0 && udp.remoteIP() == ip) {
            String s = udp.readString();
            std::string res(s.c_str());

            size_t pos = res.find("Server:");
            if (pos == std::string::npos)
                pos = res.find("SERVER:");

            if (pos != std::string::npos) {
                size_t end = res.find("\r\n", pos);
                if (end != std::string::npos)
                    return res.substr(pos + 8, end - (pos + 8));
            }
        }
    }
    return "";
}

std::string Helpers::getMDNSHostname(IPAddress ip) {
    WiFiUDP udp;
    udp.begin(5353);

    // mDNS query for ANY service
    uint8_t mdnsQuery[] = {
        0x00, 0x00, // Transaction ID
        0x00, 0x00, // Flags
        0x00, 0x01, // Questions
        0x00, 0x00, // Answer RRs
        0x00, 0x00, // Authority RRs
        0x00, 0x00, // Additional RRs

        // _services._dns-sd._udp.local
        0x09, '_','s','e','r','v','i','c','e','s',
        0x07, '_','d','n','s','-','s','d',
        0x04, '_','u','d','p',
        0x05, 'l','o','c','a','l',
        0x00,

        0x00, 0x0C, // PTR
        0x00, 0x01  // IN
    };

    udp.beginPacket(IPAddress(224, 0, 0, 251), 5353);
    udp.write(mdnsQuery, sizeof(mdnsQuery));
    udp.endPacket();

    unsigned long start = millis();
    while (millis() - start < 800) {
        int size = udp.parsePacket();
        if (size > 0 && udp.remoteIP() == ip) {
            std::vector<uint8_t> buf(size);
            udp.read(buf.data(), size);

            for (int i = 0; i < size - 6; i++) {
                if (buf[i] > 0 && buf[i] < 64) {
                    std::string name;
                    int p = i;

                    while (p < size && buf[p] != 0x00) {
                        uint8_t len = buf[p++];

                        // DNS compression
                        if ((len & 0xC0) == 0xC0) break;

                        if (len > 32 || p + len > size) break;

                        name.append((char*)&buf[p], len);
                        name.push_back('.');
                        p += len;
                    }

                    if (name.size() >= 7 &&
                        name.compare(name.size() - 7, 7, ".local.") == 0) {
                        name.pop_back();
                        return name;
                    }
                }
            }
        }
    }
    return "";
}

std::string Helpers::getVendorFromAPI(String mac) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    // URL format: https://api.macvendors.com/FC-FB-FB-01-FA-21
    String url = "https://api.macvendors.com/" + mac;

    if (!http.begin(client, url)) {
        return "HTTP begin failed";
    }

    int httpCode = http.GET();

    std::string vendor = "Unknown Vendor";

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        payload.trim();
        vendor = payload.c_str();
    } else if (httpCode == 404) {
        vendor = "Not Found";
    } else if (httpCode == 429) {
        vendor = "Rate Limit (Wait)";
    } else {
        vendor = "API Error";
    }

    http.end();
    return vendor;
}

void Helpers::initTime() {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return;
    }
    Serial.println("Time synchronized");
}
