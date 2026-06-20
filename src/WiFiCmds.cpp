#include "WiFiCmds.h"
#include "Helpers.h"

// For the SoftAP connected-client list (ap status).
#include "esp_wifi.h"
#include "esp_netif.h"

bool WiFiCmds::waitingForPass = false;
std::string WiFiCmds::pendingSSID = "";

void WiFiCmds::wifi_scan(LineCallback emit) {
    emit("Scanning Wi-Fi...\n");

    char line[128];

    // Put the radio in a clean, scan-ready state. After a failed connect the
    // driver can be left looping on the bad credentials, which makes
    // scanNetworks() return nothing. Reset that here, but never drop an
    // active, working connection.
    if (WiFi.getMode() == WIFI_OFF) {
        WiFi.mode(WIFI_STA);
    }
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(false, false); // cancel any pending/looping connect
        delay(100);
    }
    WiFi.scanDelete(); // drop stale results from a previous scan

    int n = WiFi.scanNetworks();
    if (n <= 0) { // 0 = none found, <0 = scan failed / still running
        emit("No networks found.\n");
    } else {
        snprintf(line, sizeof(line), "%-19s | %-4s | %s\n", "SSID", "RSSI", "AUTH");
        emit(line);
        emit("---------------------------------\n");

        Helpers::lastOutput.clear();
        for (int i = 0; i < std::min(n, 10); ++i) {
            std::string ssid = WiFi.SSID(i).c_str();
            Helpers::lastOutput.push_back(ssid);

            size_t visualLen = Helpers::utf8_strlen(ssid);
            if (visualLen > 18) {
                ssid = Helpers::utf8_substr(ssid, 15) + "...";
                visualLen = 18;
            }

            int spacesNeeded = 19 - (int)visualLen;
            if (spacesNeeded < 0) spacesNeeded = 0;

            std::string paddedSsid = ssid;
            for (int s = 0; s < spacesNeeded; s++) {
                paddedSsid += " ";
            }

            snprintf(line, sizeof(line), "%s | %-4d | %s\n",
                    paddedSsid.c_str(),
                    WiFi.RSSI(i),
                    (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "Open" : "Pass");
            emit(line);
        }
    }

    // FIX: free the memory used by the scan results.
    WiFi.scanDelete();
}

void WiFiCmds::wifi_connect(std::string ssid, LineCallback emit) {
    WiFi.disconnect(true, true);
    delay(200);

    std::string password = "";
    bool passwordProvided = false;

    size_t pPos = ssid.find(" -p");
    if (pPos != std::string::npos) {
        passwordProvided = true;

        // Password after "-p"
        password = ssid.substr(pPos + 3);
        ssid = ssid.substr(0, pPos);

        // FIX: with "-p password" (space-separated) a leading space leaked
        // into the password. Trim surrounding whitespace.
        Helpers::trim(password);
        Helpers::trim(ssid);
    }

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "Connecting to %s...\n", ssid.c_str());
    emit(buffer);

    int n = WiFi.scanNetworks();
    bool isOpen = true;

    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == ssid.c_str()) {
            if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) {
                isOpen = false;
            }
            break;
        }
    }
    WiFi.scanDelete();

    if (isOpen) {
        WiFi.begin(ssid.c_str());
        Helpers::checkConnection(emit);
        return;
    }

    if (passwordProvided) {
        WiFi.begin(ssid.c_str(), password.c_str());
        Helpers::checkConnection(emit);
        return;
    }

    waitingForPass = true;
    pendingSSID = ssid;
    emit("Enter password: ");
}

void WiFiCmds::wifi_connect_with_pass(std::string password, LineCallback emit) {
    WiFi.begin(pendingSSID.c_str(), password.c_str());
    Helpers::checkConnection(emit);
    waitingForPass = false;
    pendingSSID = "";
}

void WiFiCmds::wifi_disconnect(LineCallback emit) {
    if (WiFi.status() == WL_CONNECTED) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "Disconnecting from %s...\n", WiFi.SSID().c_str());
        emit(buffer);
        WiFi.disconnect(true, true); // true also erases saved network settings

        // Small delay so the stack can close the session
        unsigned long startDel = millis();
        while (WiFi.status() == WL_CONNECTED && millis() - startDel < 2000) {
            delay(100);
        }
        emit("Disconnected.\n"); // FIX: typo "Disconected"
    } else {
        emit("Not connected to WiFi.\n");
    }
}

// ---- SoftAP (access point) ----

void WiFiCmds::ap_start(const std::string& ssid, const std::string& password, LineCallback emit) {
    if (ssid.empty() || ssid.size() > 32) {
        emit("ap: SSID must be 1-32 chars\n");
        return;
    }
    bool open = password.empty();
    if (!open && (password.size() < 8 || password.size() > 63)) {
        emit("ap: password must be 8-63 chars (omit -p for an open AP)\n");
        return;
    }
    bool sta = (WiFi.status() == WL_CONNECTED);

    // AP+STA when keeping a station link (must use the station channel).
    WiFi.mode(sta ? WIFI_AP_STA : WIFI_AP);
    int chan = sta ? WiFi.channel() : 1;
    bool ok = open ? WiFi.softAP(ssid.c_str(), NULL, chan)
                   : WiFi.softAP(ssid.c_str(), password.c_str(), chan);
    if (!ok) {
        emit("ap: failed to start\n");
        return;
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "AP \"%s\" up (%s, ch %d)\n",
             ssid.c_str(), open ? "open" : "WPA2", chan);
    emit(buf);
    snprintf(buf, sizeof(buf), "IP: %s\n", WiFi.softAPIP().toString().c_str());
    emit(buf);
}

void WiFiCmds::ap_stop(LineCallback emit) {
    wifi_mode_t m = WiFi.getMode();
    if (m != WIFI_AP && m != WIFI_AP_STA) {
        emit("AP is not running.\n");
        return;
    }
    WiFi.softAPdisconnect(true);   // stop AP and free its resources
    WiFi.mode(WIFI_STA);           // back to station-only
    emit("AP stopped.\n");
}

void WiFiCmds::ap_status(LineCallback emit) {
    wifi_mode_t m = WiFi.getMode();
    if (m != WIFI_AP && m != WIFI_AP_STA) {
        emit("AP: off\n");
        return;
    }
    char buf[160];
    int n = WiFi.softAPgetStationNum();
    snprintf(buf, sizeof(buf), "AP: on  IP %s  clients %d\n\n",
             WiFi.softAPIP().toString().c_str(), n);
    emit(buf);
    if (n == 0) { emit("No clients connected.\n"); return; }

    // Per-client details (IP + MAC from the netif lease list; Host + Vendor
    // resolved like 'net s'). Vendor lookups hit an online API, so this is slow.
    wifi_sta_list_t staList;
    esp_netif_sta_list_t netList;
    if (esp_wifi_ap_get_sta_list(&staList) != ESP_OK ||
        esp_netif_get_sta_list(&staList, &netList) != ESP_OK) {
        emit("(could not read client list)\n");
        return;
    }
    for (int i = 0; i < netList.num; i++) {
        esp_netif_sta_info_t& s = netList.sta[i];
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X-%02X-%02X-%02X-%02X-%02X",
                 s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5]);
        IPAddress ip(s.ip.addr);

        std::string host = Helpers::getHostname(ip);
        if (host == "") host = Helpers::getMDNSHostname(ip);
        if (host == "") host = Helpers::getSSDPInfo(ip);
        if (host == "") host = "Unknown Device";
        std::string vendor = Helpers::getVendorFromAPI(String(macStr));

        snprintf(buf, sizeof(buf), "Host: %s\n", host.c_str());   emit(buf);
        snprintf(buf, sizeof(buf), "IP: %s\n", ip.toString().c_str()); emit(buf);
        snprintf(buf, sizeof(buf), "MAC: %s\n", macStr);          emit(buf);
        snprintf(buf, sizeof(buf), "Vendor: %s\n\n", vendor.c_str()); emit(buf);
        delay(1200); // rate-limit requests to api.macvendors.com
    }
}
