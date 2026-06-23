#include "FwCmds.h"

#include <M5Cardputer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"

// ESP image / partition constants used to recognise image types.
static const uint8_t  ESP_IMAGE_MAGIC   = 0xE9;
static const uint32_t APP_DESC_MAGIC     = 0xABCD5432; // esp_app_desc_t.magic_word
static const uint16_t PART_ENTRY_MAGIC   = 0x50AA;     // partition table entry magic
static const int      APP_DESC_OFFSET    = 0x20;       // app_desc within an app image
static const int      PART_TABLE_OFFSET  = 0x8000;     // partition table in a 0x0 image
static const int      HDR_SNIFF          = 0x9000;     // read this much to inspect/parse

static int slotOf(const esp_partition_t* p) {
    if (!p) return 0;
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) return 1;
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) return 2;
    return 0;
}

static const esp_partition_t* partitionOfSlot(int slot) {
    esp_partition_subtype_t st =
        (slot == 1) ? ESP_PARTITION_SUBTYPE_APP_OTA_0 :
        (slot == 2) ? ESP_PARTITION_SUBTYPE_APP_OTA_1 :
                      ESP_PARTITION_SUBTYPE_ANY;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, st, NULL);
}

static inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Scan an in-memory partition table for the application partition (factory or
// ota_0). Returns its offset+size within the flash image.
static bool findAppPartition(const uint8_t* tbl, int len, uint32_t& off, uint32_t& size) {
    for (int i = 0; i + 32 <= len; i += 32) {
        uint16_t magic = (uint16_t)tbl[i] | ((uint16_t)tbl[i + 1] << 8);
        if (magic != PART_ENTRY_MAGIC) break; // end of table (MD5 entry / 0xFF)
        uint8_t type = tbl[i + 2], subtype = tbl[i + 3];
        if (type == 0x00 && (subtype == 0x00 || subtype == 0x10)) { // app: factory or ota_0
            off  = rd32(tbl + i + 4);
            size = rd32(tbl + i + 8);
            return true;
        }
    }
    return false;
}

static int streamReadExact(WiFiClient* s, uint8_t* dst, int count, uint32_t timeoutMs) {
    int got = 0, sinceYield = 0;
    uint32_t last = millis();
    while (got < count) {
        int avail = s->available();
        if (avail > 0) {
            int want = count - got;
            if (want > avail) want = avail;
            int n = s->readBytes(dst + got, want);
            got += n; last = millis();
            sinceYield += n; if (sinceYield >= 16384) { sinceYield = 0; delay(1); }
        } else {
            if (!s->connected() && s->available() == 0) break;
            if (millis() - last > timeoutMs) break;
            delay(1);
        }
    }
    return got;
}

static bool streamSkip(WiFiClient* s, uint32_t count, uint32_t timeoutMs) {
    uint8_t tmp[512];
    uint32_t done = 0; int sinceYield = 0;
    uint32_t last = millis();
    while (done < count) {
        int avail = s->available();
        if (avail > 0) {
            uint32_t want = count - done;
            if (want > sizeof(tmp)) want = sizeof(tmp);
            int n = s->readBytes(tmp, want);
            done += n; last = millis();
            sinceYield += n; if (sinceYield >= 16384) { sinceYield = 0; delay(1); }
        } else {
            if (!s->connected() && s->available() == 0) break;
            if (millis() - last > timeoutMs) break;
            delay(1);
        }
    }
    return done == count;
}

// Write up to `cap` bytes from the stream into the OTA handle.
static int streamToOta(WiFiClient* s, esp_ota_handle_t h, uint32_t cap,
                       uint32_t timeoutMs, bool& ok) {
    uint8_t buf[1024];
    uint32_t written = 0; int sinceYield = 0;
    uint32_t last = millis();
    ok = true;
    while (written < cap) {
        int avail = s->available();
        if (avail > 0) {
            uint32_t want = cap - written;
            if (want > sizeof(buf)) want = sizeof(buf);
            int n = s->readBytes(buf, want);
            if (esp_ota_write(h, buf, n) != ESP_OK) { ok = false; break; }
            written += n; last = millis();
            sinceYield += n; if (sinceYield >= 16384) { sinceYield = 0; delay(1); }
        } else {
            if (!s->connected() && s->available() == 0) break;
            if (millis() - last > timeoutMs) break;
            delay(1);
        }
    }
    return (int)written;
}

void FwCmds::list(LineCallback emit) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot    = esp_ota_get_boot_partition();

    for (int slot = 1; slot <= 2; ++slot) {
        const esp_partition_t* p = partitionOfSlot(slot);
        if (!p) continue;

        esp_app_desc_t desc;
        bool hasApp = (esp_ota_get_partition_description(p, &desc) == ESP_OK);

        const char* mark = (p == running) ? " <- running"
                         : (p == boot)    ? " <- boots next"
                                          : "";
        char line[128];
        if (hasApp) {
            snprintf(line, sizeof(line), "slot %d  %uKB  %s %s%s\n",
                     slot, (unsigned)(p->size / 1024),
                     desc.project_name[0] ? desc.project_name : "app",
                     desc.version[0] ? desc.version : "",
                     mark);
        } else {
            snprintf(line, sizeof(line), "slot %d  %uKB  <empty>%s\n",
                     slot, (unsigned)(p->size / 1024), mark);
        }
        emit(line);
    }
    emit("Use 'fw install <url>' or 'fw boot <1|2>'.\n");
}

void FwCmds::install(const std::string& url, LineCallback emit) {
    if (url.empty()) { emit("Usage: fw install <url>\n"); return; }
    if (WiFi.status() != WL_CONNECTED) { emit("Not connected to WiFi\n"); return; }

    bool https = (url.rfind("https://", 0) == 0);
    if (!https && url.rfind("http://", 0) != 0) {
        emit("fw: URL must start with http:// or https://\n");
        return;
    }

    const esp_partition_t* target = esp_ota_get_next_update_partition(NULL);
    if (!target) { emit("fw: no free OTA slot\n"); return; }

    char line[128];
    snprintf(line, sizeof(line), "Target: slot %d (%uKB)\n",
             slotOf(target), (unsigned)(target->size / 1024));
    emit(line);

    HTTPClient http;
    WiFiClientSecure sclient;
    WiFiClient pclient;
    bool ok;
    if (https) { sclient.setInsecure(); ok = http.begin(sclient, url.c_str()); }
    else       { ok = http.begin(pclient, url.c_str()); }
    if (!ok) { emit("fw: could not start request (bad URL?)\n"); return; }

    http.setUserAgent("hamsTerm/1.0");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(20000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        snprintf(line, sizeof(line), "fw: HTTP %d\n", code);
        emit(line); http.end(); return;
    }
    int total = http.getSize();
    WiFiClient* stream = http.getStreamPtr();

    // --- sniff the header to recognise app-only vs factory/merged image ---
    uint8_t* hdr = (uint8_t*)malloc(HDR_SNIFF);
    if (!hdr) { emit("fw: out of memory\n"); http.end(); return; }
    int hdrLen = streamReadExact(stream, hdr, HDR_SNIFF, 20000);

    if (hdrLen < APP_DESC_OFFSET + 4 || hdr[0] != ESP_IMAGE_MAGIC) {
        emit("fw: not an ESP firmware image\n");
        free(hdr); http.end(); return;
    }

    bool appOnly = (rd32(hdr + APP_DESC_OFFSET) == APP_DESC_MAGIC);

    uint32_t appOff = 0, appPartSize = 0;
    if (!appOnly) {
        if (hdrLen < PART_TABLE_OFFSET + 32 ||
            !findAppPartition(hdr + PART_TABLE_OFFSET, hdrLen - PART_TABLE_OFFSET,
                              appOff, appPartSize)) {
            emit("fw: unrecognised image (not app-only, no app partition found)\n");
            free(hdr); http.end(); return;
        }
        snprintf(line, sizeof(line), "Factory image: extracting app at 0x%X\n",
                 (unsigned)appOff);
        emit(line);
    }

    uint32_t cap;
    if (appOnly) cap = (total > 0) ? (uint32_t)total : target->size;
    else         cap = appPartSize;
    if (cap > target->size) cap = target->size;

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, cap, &handle);
    if (err != ESP_OK) {
        snprintf(line, sizeof(line), "fw: ota_begin failed (%d)\n", (int)err);
        emit(line); free(hdr); http.end(); return;
    }

    emit("Writing firmware - do NOT power off...\n");

    int written = 0;
    bool failed = false;

    if (appOnly) {
        int firstN = hdrLen; if ((uint32_t)firstN > cap) firstN = cap;
        if (esp_ota_write(handle, hdr, firstN) != ESP_OK) failed = true;
        written += firstN;
        free(hdr); hdr = nullptr;
        if (!failed && (uint32_t)written < cap) {
            bool w;
            written += streamToOta(stream, handle, cap - written, 20000, w);
            if (!w) failed = true;
        }
    } else {
        free(hdr); hdr = nullptr;
        uint32_t pos = (uint32_t)hdrLen;
        if (appOff < pos) {
            emit("fw: unexpected layout (app before 0x9000)\n");
            failed = true;
        } else if (appOff > pos) {
            if (!streamSkip(stream, appOff - pos, 20000)) failed = true;
        }
        if (!failed) {
            bool w;
            written = streamToOta(stream, handle, cap, 20000, w);
            if (!w) failed = true;
        }
    }
    http.end();

    if (failed) {
        emit("fw: write/download error\n");
        esp_ota_end(handle);
        return;
    }

    err = esp_ota_end(handle); // verifies the image (magic byte + checksum)
    if (err != ESP_OK) {
        snprintf(line, sizeof(line),
                 "fw: invalid image (%d) - not a firmware .bin?\n", (int)err);
        emit(line);
        return;
    }

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(target, &desc) != ESP_OK) {
        emit("fw: written image has no app descriptor (wrong image type)\n");
        return;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        snprintf(line, sizeof(line), "fw: set boot failed (%d)\n", (int)err);
        emit(line);
        return;
    }

    snprintf(line, sizeof(line), "Installed %s %s (%d bytes) to slot %d. Rebooting...\n",
             desc.project_name[0] ? desc.project_name : "app",
             desc.version[0] ? desc.version : "",
             written, slotOf(target));
    emit(line);
    delay(700);
    esp_restart();
}

void FwCmds::boot(int slot, LineCallback emit) {
    if (slot != 1 && slot != 2) { emit("fw: slot must be 1 or 2\n"); return; }

    const esp_partition_t* p = partitionOfSlot(slot);
    if (!p) { emit("fw: slot not found\n"); return; }

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(p, &desc) != ESP_OK) {
        emit("fw: that slot is empty (no valid app)\n");
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        char l[64];
        snprintf(l, sizeof(l), "fw: set boot failed (%d)\n", (int)err);
        emit(l);
        return;
    }
    char l[48];
    snprintf(l, sizeof(l), "Booting slot %d...\n", slot);
    emit(l);
    delay(700);
    esp_restart();
}

void FwCmds::fw(const std::string& args, LineCallback emit) {
    std::string sub, rest;
    {
        size_t i = 0;
        while (i < args.size() && args[i] == ' ') i++;
        size_t j = i;
        while (j < args.size() && args[j] != ' ') j++;
        sub = args.substr(i, j - i);
        while (j < args.size() && args[j] == ' ') j++;
        rest = args.substr(j);
        while (!rest.empty() && rest.back() == ' ') rest.pop_back();
    }

    if (sub.empty() || sub == "list") {
        list(emit);
    } else if (sub == "install") {
        install(rest, emit);
    } else if (sub == "boot") {
        if (rest.empty()) { emit("Usage: fw boot <1|2>\n"); return; }
        boot(atoi(rest.c_str()), emit);
    } else {
        emit("Usage: fw [list] | fw install <url> | fw boot <1|2>\n");
    }
}
