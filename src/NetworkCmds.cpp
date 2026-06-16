#include "NetworkCmds.h"
#include "Helpers.h"

void NetworkCmds::net_scan(LineCallback emit) {
    if (WiFi.status() != WL_CONNECTED) {
        emit("Not connected to WiFi\n");
        return;
    }

    emit("Scanning network...\n");

    IPAddress localIP = WiFi.localIP();
    IPAddress subnet = WiFi.subnetMask();
    IPAddress gateway = WiFi.gatewayIP();
    (void)gateway;

    auto ipToUInt = [](IPAddress ip) -> uint32_t {
        return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3];
    };

    auto uIntToIP = [](uint32_t addr) -> IPAddress {
        return IPAddress((addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF);
    };

    uint32_t net = ipToUInt(localIP) & ipToUInt(subnet);
    uint32_t broadcast = net | ~ipToUInt(subnet);

    // NOTE: for a /24 this is up to ~254 sequential pings with timeouts,
    // a blocking operation that can take minutes (TODO:
    // make the scan interruptible / asynchronous).
    for (uint32_t addr = net + 1; addr < broadcast; addr++) {
        IPAddress target = uIntToIP(addr);
        if (target == localIP) continue; // skip our own IP

        Ping.ping(target, 1);

        delay(1); // keep the ESP32 watchdog happy
    }

    struct eth_addr *ret_ethaddr;
    ip4_addr_t *ret_ipaddr;
    struct netif *ret_netif;

    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        if (etharp_get_entry(i, &ret_ipaddr, &ret_netif, &ret_ethaddr)) {
            IPAddress ip = IPAddress(ret_ipaddr->addr);

            // Build the MAC address string (snprintf instead of sprintf)
            char macStr[18];
            snprintf(macStr, sizeof(macStr), "%02X-%02X-%02X-%02X-%02X-%02X",
                    ret_ethaddr->addr[0], ret_ethaddr->addr[1], ret_ethaddr->addr[2],
                    ret_ethaddr->addr[3], ret_ethaddr->addr[4], ret_ethaddr->addr[5]);

            // Try to resolve the hostname (NetBIOS -> mDNS -> SSDP)
            std::string hostname = Helpers::getHostname(ip);
            if (hostname == "") hostname = Helpers::getMDNSHostname(ip);
            if (hostname == "") hostname = Helpers::getSSDPInfo(ip);

            if (hostname == "") {
                hostname = "Unknown Device";
            }

            std::string vendor = Helpers::getVendorFromAPI(String(macStr));

            char buffer[128];
            snprintf(buffer, sizeof(buffer), "Host: %s\n", hostname.c_str());
            emit(buffer);

            snprintf(buffer, sizeof(buffer), "IP: %s\n", ip.toString().c_str());
            emit(buffer);

            snprintf(buffer, sizeof(buffer), "MAC: %s\n", macStr);
            emit(buffer);

            snprintf(buffer, sizeof(buffer), "Vendor: %s\n\n", vendor.c_str());
            emit(buffer);

            delay(1200); // rate-limit requests to api.macvendors.com
        }
    }

    emit("Scan complete\n");
}

void NetworkCmds::net_scan_ports(const IPAddress ip, const std::vector<uint16_t>& ports, LineCallback emit) {
    if (WiFi.status() != WL_CONNECTED) {
        emit("Not connected to WiFi\n");
        return;
    }

    char buffer[80];
    snprintf(buffer, sizeof(buffer), "Scanning %u port(s) on %s...\n",
             (unsigned)ports.size(), ip.toString().c_str());
    emit(buffer);

    int openCount = 0;

    for (uint16_t port : ports) {
        WiFiClient client;
        client.setTimeout(500);

        // connect with a ~500 ms timeout. A closed port returns RST
        // quickly; a filtered port waits for the timeout.
        if (client.connect(ip, port, 500)) {
            snprintf(buffer, sizeof(buffer), "Open port: %u\n", port);
            emit(buffer);
            openCount++;
        }

        client.stop();
        delay(1); // watchdog-friendly
    }

    if (openCount == 0) {
        emit("No open ports\n");
    }

    snprintf(buffer, sizeof(buffer), "Scan complete (%d open)\n", openCount);
    emit(buffer);
}

// Extracts a clean hostname: strips the scheme (http://, https://),
// path/query/fragment and port. "https://www.google.com/search?q=x" -> "www.google.com"
static std::string ping_extract_host(const std::string& target) {
    std::string host = target;

    // trim whitespace
    while (!host.empty() && host.front() == ' ') host.erase(0, 1);
    while (!host.empty() && host.back() == ' ') host.pop_back();

    // scheme://
    size_t scheme = host.find("://");
    if (scheme != std::string::npos) host = host.substr(scheme + 3);

    // drop everything after the first / ? #
    size_t pathPos = host.find_first_of("/?#");
    if (pathPos != std::string::npos) host = host.substr(0, pathPos);

    // drop the port in host:port (IPv6 not supported)
    size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);

    return host;
}

void NetworkCmds::ping(const std::string& target, uint8_t count, LineCallback emit) {
    if (WiFi.status() != WL_CONNECTED) {
        emit("Not connected to WiFi\n");
        return;
    }

    std::string host = ping_extract_host(target);
    if (host.empty()) {
        emit("ping: invalid target\n");
        return;
    }

    // If it is already an IP, use it as is; otherwise resolve via DNS.
    IPAddress ip;
    bool isIP = ip.fromString(String(host.c_str()));
    if (!isIP) {
        if (!WiFi.hostByName(host.c_str(), ip) || ip == IPAddress(0, 0, 0, 0)) {
            emit("ping: cannot resolve " + host + "\n");
            return;
        }
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "PING %s (%s)\n", host.c_str(), ip.toString().c_str());
    emit(buf);

    int sent = 0, recv = 0;
    float totalTime = 0.0f;

    for (uint8_t i = 0; i < count; i++) {
        sent++;
        bool ok = Ping.ping(ip, 1); // single echo request
        if (ok) {
            float t = Ping.averageTime(); // time of the last ping, ms
            recv++;
            totalTime += t;
            snprintf(buf, sizeof(buf), "Reply from %s: time=%.1f ms\n",
                     ip.toString().c_str(), t);
        } else {
            snprintf(buf, sizeof(buf), "Request timed out\n");
        }
        emit(buf);
        delay(100); // small gap + watchdog-friendly
    }

    int loss = (sent > 0) ? (100 * (sent - recv) / sent) : 0;
    snprintf(buf, sizeof(buf),
             "--- %s ping statistics ---\n%d sent, %d received, %d%% loss\n",
             host.c_str(), sent, recv, loss);
    emit(buf);

    if (recv > 0) {
        snprintf(buf, sizeof(buf), "avg = %.1f ms\n", totalTime / recv);
        emit(buf);
    }
}
