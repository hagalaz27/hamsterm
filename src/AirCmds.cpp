#include "AirCmds.h"
#include "Helpers.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <stdio.h>

// All capture state and frame parsing lives in this translation unit. The
// promiscuous RX callback runs in the WiFi task and only touches the fixed
// arrays below (no allocation, no printing); the report is built afterwards,
// once promiscuous mode is off, so there is no concurrent access.
namespace {

const int MAX_AP  = 48;
const int MAX_STA = 192;

struct ApRec {
    uint8_t     bssid[6];
    char        ssid[33];
    uint8_t     channel;
    int8_t      rssi;
    const char* enc;      // "open" / "wep" / "wpa" / "wpa2"
};

struct StaRec {
    uint8_t mac[6];
    uint8_t bssid[6];     // AP it was seen talking to (zero if unknown)
    int8_t  rssi;
    bool    associated;   // seen in a data frame with an AP
    char    probe[33];    // SSID from a probe request (may be empty)
};

ApRec  g_aps[MAX_AP];
StaRec g_stas[MAX_STA];
volatile int g_apCount  = 0;
volatile int g_staCount = 0;

bool macEq(const uint8_t* a, const uint8_t* b)  { return memcmp(a, b, 6) == 0; }
bool macZero(const uint8_t* a) { for (int i = 0; i < 6; i++) if (a[i]) return false; return true; }
bool macGroup(const uint8_t* a) { return (a[0] & 0x01) != 0; } // broadcast/multicast bit

std::string macStr(const uint8_t* m) {
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    return std::string(b);
}

// Wall-clock HH:MM:SS if the RTC/NTP time is set, otherwise seconds since boot.
std::string nowStamp() {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        char t[16];
        strftime(t, sizeof(t), "%H:%M:%S", &ti);
        return std::string(t);
    }
    char t[16];
    snprintf(t, sizeof(t), "+%lus", (unsigned long)(millis() / 1000));
    return std::string(t);
}

int findAp(const uint8_t* bssid) {
    for (int i = 0; i < g_apCount; i++) if (macEq(g_aps[i].bssid, bssid)) return i;
    return -1;
}
int findSta(const uint8_t* mac) {
    for (int i = 0; i < g_staCount; i++) if (macEq(g_stas[i].mac, mac)) return i;
    return -1;
}

// Walk tagged parameters: pull the SSID (tag 0), DS channel (tag 3) and the
// security type from RSN (48) / WPA vendor (221). Non-printable SSID bytes are
// replaced with '.' so a hostile SSID can't scramble the screen.
void parseTags(const uint8_t* p, int len, char* ssidOut,
               uint8_t* chOut, const char** encOut, bool privacy) {
    if (ssidOut) ssidOut[0] = 0;
    bool rsn = false, wpa = false;
    int i = 0;
    while (i + 2 <= len) {
        uint8_t id = p[i], l = p[i + 1];
        if (i + 2 + (int)l > len) break;
        const uint8_t* d = p + i + 2;
        if (id == 0 && ssidOut) {
            int n = l; if (n > 32) n = 32;
            for (int k = 0; k < n; k++) {
                char c = (char)d[k];
                ssidOut[k] = (c >= 0x20 && c < 0x7f) ? c : '.';
            }
            ssidOut[n] = 0;
        } else if (id == 3 && l >= 1 && chOut) {
            *chOut = d[0];
        } else if (id == 48) {
            rsn = true;
        } else if (id == 221 && l >= 4 &&
                   d[0] == 0x00 && d[1] == 0x50 && d[2] == 0xf2 && d[3] == 0x01) {
            wpa = true;
        }
        i += 2 + l;
    }
    if (encOut) {
        if (!privacy)   *encOut = "open";
        else if (rsn)   *encOut = "wpa2";
        else if (wpa)   *encOut = "wpa";
        else            *encOut = "wep";
    }
}

void recordAp(const uint8_t* bssid, int8_t rssi, uint8_t chan,
              const uint8_t* body, int bodyLen) {
    char ssid[33] = {0};
    uint8_t ch = chan;
    const char* enc = "?";
    bool privacy = false;
    if (bodyLen >= 12) {
        uint16_t cap = body[10] | (body[11] << 8);
        privacy = (cap & 0x0010) != 0;
        parseTags(body + 12, bodyLen - 12, ssid, &ch, &enc, privacy);
    }
    int idx = findAp(bssid);
    if (idx < 0) {
        if (g_apCount >= MAX_AP) return;
        idx = g_apCount++;
        memcpy(g_aps[idx].bssid, bssid, 6);
        g_aps[idx].ssid[0] = 0;
        g_aps[idx].rssi = rssi;
    }
    if (rssi > g_aps[idx].rssi) g_aps[idx].rssi = rssi;
    g_aps[idx].channel = ch;
    g_aps[idx].enc = enc;
    if (ssid[0]) { strncpy(g_aps[idx].ssid, ssid, 32); g_aps[idx].ssid[32] = 0; }
}

void recordSta(const uint8_t* mac, const uint8_t* bssid, int8_t rssi,
               bool associated, const char* probe) {
    if (macZero(mac) || macGroup(mac)) return; // only real unicast stations
    int idx = findSta(mac);
    if (idx < 0) {
        if (g_staCount >= MAX_STA) return;
        idx = g_staCount++;
        memset(&g_stas[idx], 0, sizeof(StaRec));
        memcpy(g_stas[idx].mac, mac, 6);
        g_stas[idx].rssi = rssi;
    }
    if (rssi > g_stas[idx].rssi) g_stas[idx].rssi = rssi;
    if (associated && bssid && !macZero(bssid) && !macGroup(bssid)) {
        memcpy(g_stas[idx].bssid, bssid, 6);
        g_stas[idx].associated = true;
    }
    if (probe && probe[0] && !g_stas[idx].probe[0]) {
        strncpy(g_stas[idx].probe, probe, 32); g_stas[idx].probe[32] = 0;
    }
}

void snifferCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;
    const uint8_t* pl = pkt->payload;
    int8_t  rssi = pkt->rx_ctrl.rssi;
    uint8_t ch   = pkt->rx_ctrl.channel;

    uint8_t fctl0 = pl[0], fctl1 = pl[1];
    uint8_t ftype = (fctl0 >> 2) & 0x3;
    uint8_t fsub  = (fctl0 >> 4) & 0xF;
    const uint8_t* addr1 = pl + 4;
    const uint8_t* addr2 = pl + 10;

    if (ftype == 0) {                     // management
        if (fsub == 8 || fsub == 5) {     // beacon / probe response -> AP
            recordAp(addr2, rssi, ch, pl + 24, len - 24);
        } else if (fsub == 4) {           // probe request -> unassociated station
            char ssid[33] = {0};
            parseTags(pl + 24, len - 24, ssid, NULL, NULL, false);
            recordSta(addr2, NULL, rssi, false, ssid);
        }
    } else if (ftype == 2) {              // data
        bool toDS   = (fctl1 & 0x01) != 0;
        bool fromDS = (fctl1 & 0x02) != 0;
        if (toDS && !fromDS)              // client -> AP: addr1=AP, addr2=client
            recordSta(addr2, addr1, rssi, true, NULL);
        else if (!toDS && fromDS)         // AP -> client: addr1=client, addr2=AP
            recordSta(addr1, addr2, rssi, true, NULL);
    }
}

bool airBreakPressed() {
    M5Cardputer.update();
    auto& kb = M5Cardputer.Keyboard;
    if (kb.isPressed()) {
        Keyboard_Class::KeysState st = kb.keysState();
        if (st.ctrl) {
            for (auto c : st.word) {
                char lc = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
                if (lc == 'c') return true;
            }
        }
    }
    return false;
}

// ---- watch mode: live monitor of one AP ----
const int MAX_WATCH = 48;
struct WClient {
    uint8_t  mac[6];
    int8_t   rssi;
    uint32_t lastSeen;
    bool     active;
    uint8_t  pendAppear; // pending "appeared" event: 0 none, 1 data, 2 assoc, 3 auth
    bool     pendLeave;  // pending explicit deauth/disassoc
};
WClient      g_wc[MAX_WATCH];
int          g_wcCount = 0;
uint8_t      g_watchBssid[6];
portMUX_TYPE g_watchMux = portMUX_INITIALIZER_UNLOCKED;

int wcFind(const uint8_t* mac) {
    for (int i = 0; i < g_wcCount; i++) if (macEq(g_wc[i].mac, mac)) return i;
    return -1;
}

bool parseMac(const std::string& s, uint8_t out[6]) {
    unsigned v[6];
    if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) { if (v[i] > 255) return false; out[i] = (uint8_t)v[i]; }
    return true;
}

// Does this frame concern our AP? If so, fill clientMac + kind
// (1 data, 2 assoc, 3 auth, 4 leave).
bool watchClassify(const uint8_t* pl, int len, uint8_t& kind, const uint8_t*& clientMac) {
    (void)len;
    uint8_t fctl0 = pl[0], fctl1 = pl[1];
    uint8_t ftype = (fctl0 >> 2) & 0x3, fsub = (fctl0 >> 4) & 0xF;
    const uint8_t* a1 = pl + 4;
    const uint8_t* a2 = pl + 10;
    const uint8_t* a3 = pl + 16;

    if (ftype == 0) {                         // management: addr3 = BSSID
        if (!macEq(a3, g_watchBssid)) return false;
        clientMac = macEq(a1, g_watchBssid) ? a2 : a1;
        if (macEq(clientMac, g_watchBssid) || macGroup(clientMac)) return false;
        if (fsub <= 3)          kind = 2;     // (re)assoc request/response
        else if (fsub == 11)    kind = 3;     // authentication
        else if (fsub == 10 || fsub == 12) kind = 4; // disassoc / deauth
        else return false;                    // beacon/probe/etc - ignore for watch
        return true;
    } else if (ftype == 2) {                  // data
        bool toDS = (fctl1 & 0x01) != 0, fromDS = (fctl1 & 0x02) != 0;
        if (toDS && !fromDS)      { if (!macEq(a1, g_watchBssid)) return false; clientMac = a2; }
        else if (!toDS && fromDS) { if (!macEq(a2, g_watchBssid)) return false; clientMac = a1; }
        else return false;
        if (macGroup(clientMac) || macEq(clientMac, g_watchBssid)) return false;
        kind = 1;
        return true;
    }
    return false;
}

void watchCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;
    uint8_t kind; const uint8_t* mac;
    if (!watchClassify(pkt->payload, len, kind, mac)) return;
    int8_t rssi = pkt->rx_ctrl.rssi;

    portENTER_CRITICAL(&g_watchMux);
    int i = wcFind(mac);
    if (i < 0) {
        if (g_wcCount < MAX_WATCH) {
            i = g_wcCount++;
            memcpy(g_wc[i].mac, mac, 6);
            g_wc[i].active = true;
            g_wc[i].pendLeave = false;
            g_wc[i].pendAppear = (kind == 4) ? 0 : kind;
        }
    } else if (!g_wc[i].active && kind != 4) {
        g_wc[i].active = true;
        g_wc[i].pendAppear = kind;
    }
    if (i >= 0) {
        g_wc[i].rssi = rssi;
        g_wc[i].lastSeen = millis();
        if (kind == 4) g_wc[i].pendLeave = true;
    }
    portEXIT_CRITICAL(&g_watchMux);
}

void printReport(LineCallback emit) {
    char b[96];

    // AP indices sorted by RSSI (strongest first) - selection sort, tiny arrays.
    int order[MAX_AP];
    for (int i = 0; i < g_apCount; i++) order[i] = i;
    for (int i = 0; i < g_apCount; i++)
        for (int j = i + 1; j < g_apCount; j++)
            if (g_aps[order[j]].rssi > g_aps[order[i]].rssi) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }

    bool printed[MAX_STA];
    for (int i = 0; i < g_staCount; i++) printed[i] = false;

    snprintf(b, sizeof(b), "\n=== APs: %d ===\n", g_apCount);
    emit(b);
    for (int oi = 0; oi < g_apCount; oi++) {
        ApRec& ap = g_aps[order[oi]];
        snprintf(b, sizeof(b), "#%d ch%-2d %d %-4s %s\n", oi + 1, ap.channel,
                 ap.rssi, ap.enc, ap.ssid[0] ? ap.ssid : "<hidden>");
        emit(b);
        emit("   " + macStr(ap.bssid) + "\n");
        int shown = 0;
        for (int s = 0; s < g_staCount; s++) {
            if (g_stas[s].associated && macEq(g_stas[s].bssid, ap.bssid)) {
                snprintf(b, sizeof(b), "   - sta %d ", g_stas[s].rssi);
                emit(std::string(b) + macStr(g_stas[s].mac) + "\n");
                printed[s] = true;
                shown++;
            }
        }
        if (!shown) emit("   (no clients seen)\n");
    }

    int un = 0;
    for (int s = 0; s < g_staCount; s++) if (!printed[s]) un++;
    snprintf(b, sizeof(b), "\n=== Unassociated: %d ===\n", un);
    emit(b);
    for (int s = 0; s < g_staCount; s++) {
        if (printed[s]) continue;
        std::string line = "  " + std::to_string(g_stas[s].rssi) + " " + macStr(g_stas[s].mac);
        if (g_stas[s].associated && !macZero(g_stas[s].bssid))
            line += " ->" + macStr(g_stas[s].bssid); // talks to an AP we did not capture
        else if (g_stas[s].probe[0])
            line += " probe:\"" + std::string(g_stas[s].probe) + "\"";
        emit(line + "\n");
    }

    snprintf(b, sizeof(b), "\n%d APs, %d stations seen\n", g_apCount, g_staCount);
    emit(b);
}

} // namespace

void AirCmds::sniff(uint32_t seconds, LineCallback emit) {
    if (seconds == 0)   seconds = 60;
    if (seconds > 300)  seconds = 300; // cap at 5 minutes

    bool wasConnected = (WiFi.status() == WL_CONNECTED);

    char b[96];
    emit("air: entering monitor mode (WiFi will disconnect)\n");
    snprintf(b, sizeof(b),
             "air: sniffing ~%us on channels 1-13, Ctrl+C to stop\n",
             (unsigned)seconds);
    emit(b);

    g_apCount = 0;
    g_staCount = 0;

    // Take the radio for monitoring.
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_STA);
    esp_wifi_start();

    wifi_promiscuous_filter_t filt;
    memset(&filt, 0, sizeof(filt));
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&snifferCb);
    esp_wifi_set_promiscuous(true);

    const uint8_t channels[] = {1,2,3,4,5,6,7,8,9,10,11,12,13};
    const int nch = (int)sizeof(channels);
    uint32_t endAt = millis() + seconds * 1000;
    bool interrupted = false;
    int ci = 0;

    while ((int32_t)(endAt - millis()) > 0) {
        esp_wifi_set_channel(channels[ci], WIFI_SECOND_CHAN_NONE);
        ci = (ci + 1) % nch;
        for (int d = 0; d < 5; d++) {       // ~250 ms dwell, responsive to Ctrl+C
            if (airBreakPressed()) { interrupted = true; break; }
            delay(50);
        }
        if (interrupted) break;
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    delay(50); // let any in-flight callback finish before reading the buffers

    if (interrupted) emit("^C\n");

    printReport(emit);

    emit("air: monitor stopped. WiFi disconnected");
    emit(wasConnected ? " - reconnect with 'wf c <ssid>'\n" : "\n");
}

void AirCmds::watch(const std::string& target, uint32_t timeoutSec,
                    LineCallback emit, std::function<bool()> poll) {
    if (timeoutSec == 0) timeoutSec = 30;

    // Resolve the target (SSID or BSSID) to a BSSID + channel via a quick scan.
    uint8_t bssid[6];
    bool givenBssid = parseMac(target, bssid);
    std::string ssidStr;
    int channel = 0;
    bool found = false;

    emit("air: scanning for target AP...\n");
    int n = WiFi.scanNetworks(false, true); // sync, include hidden
    for (int i = 0; i < n; i++) {
        if (givenBssid) {
            if (memcmp(WiFi.BSSID(i), bssid, 6) == 0) {
                channel = WiFi.channel(i); ssidStr = WiFi.SSID(i).c_str(); found = true; break;
            }
        } else if (target == std::string(WiFi.SSID(i).c_str())) {
            memcpy(bssid, WiFi.BSSID(i), 6);
            channel = WiFi.channel(i); ssidStr = target; found = true; break;
        }
    }
    WiFi.scanDelete();
    if (!found) {
        emit("air: AP not found (try 'wf s' or give the BSSID)\n");
        Helpers::cmd_status = 1;
        return;
    }

    char b[128];
    snprintf(b, sizeof(b), "watching %s (%s) ch%d, timeout %us -- Ctrl+C to stop\n",
             ssidStr.empty() ? "<hidden>" : ssidStr.c_str(),
             macStr(bssid).c_str(), channel, (unsigned)timeoutSec);
    emit(b);

    memcpy(g_watchBssid, bssid, 6);
    g_wcCount = 0;
    bool wasConnected = (WiFi.status() == WL_CONNECTED);

    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_STA);
    esp_wifi_start();

    wifi_promiscuous_filter_t filt;
    memset(&filt, 0, sizeof(filt));
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&watchCb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE); // lock, no hopping

    bool interrupted = false;
    struct Ev { uint8_t mac[6]; int8_t rssi; uint8_t type; }; // 1 data,2 assoc,3 auth,4 leave,5 timeout
    while (true) {
        if (poll()) { interrupted = true; break; }

        Ev evs[32];
        int ne = 0;
        uint32_t now = millis();
        portENTER_CRITICAL(&g_watchMux);
        for (int i = 0; i < g_wcCount && ne < 30; i++) {
            if (g_wc[i].pendAppear) {
                memcpy(evs[ne].mac, g_wc[i].mac, 6); evs[ne].rssi = g_wc[i].rssi;
                evs[ne].type = g_wc[i].pendAppear; ne++; g_wc[i].pendAppear = 0;
            }
            if (g_wc[i].pendLeave) {
                memcpy(evs[ne].mac, g_wc[i].mac, 6); evs[ne].rssi = g_wc[i].rssi;
                evs[ne].type = 4; ne++; g_wc[i].pendLeave = false; g_wc[i].active = false;
            }
            if (g_wc[i].active && (now - g_wc[i].lastSeen) > timeoutSec * 1000) {
                memcpy(evs[ne].mac, g_wc[i].mac, 6); evs[ne].rssi = g_wc[i].rssi;
                evs[ne].type = 5; ne++; g_wc[i].active = false;
            }
        }
        portEXIT_CRITICAL(&g_watchMux);

        for (int e = 0; e < ne; e++) {
            const char* tag; const char* label;
            switch (evs[e].type) {
                case 2: tag = "+"; label = "assoc";   break;
                case 3: tag = "+"; label = "auth";    break;
                case 1: tag = "+"; label = "data";    break;
                case 4: tag = "-"; label = "deauth";  break;
                default: tag = "~"; label = "timeout"; break;
            }
            std::string ts = nowStamp();
            if (evs[e].type == 4 || evs[e].type == 5)
                snprintf(b, sizeof(b), "%s %s %s %s\n", ts.c_str(), tag,
                         macStr(evs[e].mac).c_str(), label);
            else
                snprintf(b, sizeof(b), "%s %s %s %d %s\n", ts.c_str(), tag,
                         macStr(evs[e].mac).c_str(), evs[e].rssi, label);
            emit(b);
        }

        // idle gap, staying responsive to scroll/stop keys
        for (int d = 0; d < 6; d++) {
            if (poll()) { interrupted = true; break; }
            delay(25);
        }
        if (interrupted) break;
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    delay(50); // let any in-flight callback finish

    if (interrupted) emit("^C\n");

    int act = 0;
    emit("\n--- active clients ---\n");
    for (int i = 0; i < g_wcCount; i++) {
        if (g_wc[i].active) {
            snprintf(b, sizeof(b), "  %s %d\n", macStr(g_wc[i].mac).c_str(), g_wc[i].rssi);
            emit(b);
            act++;
        }
    }
    snprintf(b, sizeof(b), "%d active, %d total seen\n", act, g_wcCount);
    emit(b);

    emit("air: monitor stopped. WiFi disconnected");
    emit(wasConnected ? " - reconnect with 'wf c <ssid>'\n" : "\n");
}
