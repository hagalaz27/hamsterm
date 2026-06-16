#include "WiFiCmds.h"
#include "Helpers.h"

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
