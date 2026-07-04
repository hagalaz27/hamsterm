#include "NetworkCmds.h"
#include "Helpers.h"
#include <M5Cardputer.h>
#include <math.h>

void NetworkCmds::net_scan(LineCallback emit) {
    if (WiFi.status() != WL_CONNECTED) {
        emit("Not connected to WiFi\n");
        Helpers::cmd_status = 1;
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

// Best-guess service name for a well-known port number (like nmap without -sV:
// a label by port, not a probe). Returns "" for unknown ports.
static const char* port_service(uint16_t port) {
    switch (port) {
        case 20: case 21:             return "ftp";
        case 22:                      return "ssh";
        case 23:                      return "telnet";
        case 25: case 587:            return "smtp";
        case 43:                      return "whois";
        case 53:                      return "dns";
        case 67: case 68:             return "dhcp";
        case 69:                      return "tftp";
        case 79:                      return "finger";
        case 80:                      return "http";
        case 88:                      return "kerberos";
        case 110:                     return "pop3";
        case 111:                     return "rpcbind";
        case 119:                     return "nntp";
        case 123:                     return "ntp";
        case 135:                     return "msrpc";
        case 137: case 138: case 139: return "netbios";
        case 143:                     return "imap";
        case 161: case 162:           return "snmp";
        case 179:                     return "bgp";
        case 389:                     return "ldap";
        case 443:                     return "https";
        case 445:                     return "smb";
        case 465:                     return "smtps";
        case 500:                     return "isakmp";
        case 514:                     return "syslog";
        case 515:                     return "printer";
        case 548:                     return "afp";
        case 554:                     return "rtsp";
        case 631:                     return "ipp";
        case 636:                     return "ldaps";
        case 873:                     return "rsync";
        case 989: case 990:           return "ftps";
        case 993:                     return "imaps";
        case 995:                     return "pop3s";
        case 1080:                    return "socks";
        case 1194:                    return "openvpn";
        case 1433: case 1434:         return "mssql";
        case 1521:                    return "oracle";
        case 1723:                    return "pptp";
        case 1883:                    return "mqtt";
        case 2049:                    return "nfs";
        case 2082: case 2083:         return "cpanel";
        case 2181:                    return "zookeeper";
        case 2375: case 2376:         return "docker";
        case 3128:                    return "proxy";
        case 3306:                    return "mysql";
        case 3389:                    return "rdp";
        case 3690:                    return "svn";
        case 5060: case 5061:         return "sip";
        case 5222: case 5269:         return "xmpp";
        case 5353:                    return "mdns";
        case 5432:                    return "postgresql";
        case 5672:                    return "amqp";
        case 5900: case 5901: case 5902: return "vnc";
        case 5984:                    return "couchdb";
        case 6379:                    return "redis";
        case 6667: case 6697:         return "irc";
        case 8000: case 8008: case 8080: case 8081: case 8888: return "http-alt";
        case 8086:                    return "influxdb";
        case 8443:                    return "https-alt";
        case 9092:                    return "kafka";
        case 9200:                    return "elasticsearch";
        case 9418:                    return "git";
        case 11211:                   return "memcached";
        case 27017: case 27018:       return "mongodb";
        default:                      return "";
    }
}

void NetworkCmds::net_scan_ports(const IPAddress ip, const std::vector<uint16_t>& ports, LineCallback emit) {
    if (WiFi.status() != WL_CONNECTED) {
        emit("Not connected to WiFi\n");
        Helpers::cmd_status = 1;
        return;
    }

    char buffer[80];
    snprintf(buffer, sizeof(buffer), "Scanning %u port(s) on %s...\n",
             (unsigned)ports.size(), ip.toString().c_str());
    emit(buffer);

    int openCount = 0;

    for (uint16_t port : ports) {
        WiFiClient client;
        client.setTimeout(250);

        // connect with a ~250 ms timeout. A closed port returns RST
        // quickly; a filtered port waits for the timeout. 250 ms keeps the
        // scan fast while staying above typical internet round-trip times.
        if (client.connect(ip, port, 250)) {
            const char* svc = port_service(port);
            if (svc[0])
                snprintf(buffer, sizeof(buffer), "Open port: %u (%s)\n", port, svc);
            else
                snprintf(buffer, sizeof(buffer), "Open port: %u\n", port);
            emit(buffer);
            openCount++;
        }

        client.stop();
        delay(1); // watchdog-friendly
    }

    if (openCount == 0) {
        emit("No open ports\n");
        Helpers::cmd_status = 1;
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

int NetworkCmds::resolve_target(const std::string& target, IPAddress& out,
                                std::string& host) {
    host = ping_extract_host(target);
    if (host.empty()) return 1;
    if (out.fromString(String(host.c_str()))) return 0;   // already a literal IP
    if (WiFi.status() != WL_CONNECTED) return 2;          // DNS needs WiFi
    if (WiFi.hostByName(host.c_str(), out) && out != IPAddress(0, 0, 0, 0)) return 0;
    return 3;
}

const char* NetworkCmds::default_ports() {
    // nmap top-1000 TCP ports (ranges kept compact); expands to exactly 1000 ports
    return
        "1 3-4 6-7 9 13 17 19-26 30 32-33 37 42-43 49 53 70 79-85 88-90 99-100 106 109-111 113 119 "
        "125 135 139 143-144 146 161 163 179 199 211-212 222 254-256 259 264 280 301 306 311 340 366 "
        "389 406-407 416-417 425 427 443-445 458 464-465 481 497 500 512-515 524 541 543-545 548 "
        "554-555 563 587 593 616-617 625 631 636 646 648 666-668 683 687 691 700 705 711 714 720 722 "
        "726 749 765 777 783 787 800-801 808 843 873 880 888 898 900-903 911-912 981 987 990 992-993 "
        "995 999-1002 1007 1009-1011 1021-1100 1102 1104-1108 1110-1114 1117 1119 1121-1124 1126 "
        "1130-1132 1137-1138 1141 1145 1147-1149 1151-1152 1154 1163-1166 1169 1174-1175 1183 "
        "1185-1187 1192 1198-1199 1201 1213 1216-1218 1233-1234 1236 1244 1247-1248 1259 1271-1272 "
        "1277 1287 1296 1300-1301 1309-1311 1322 1328 1334 1352 1417 1433-1434 1443 1455 1461 1494 "
        "1500-1501 1503 1521 1524 1533 1556 1580 1583 1594 1600 1641 1658 1666 1687-1688 1700 "
        "1717-1721 1723 1755 1761 1782-1783 1801 1805 1812 1839-1840 1862-1864 1875 1900 1914 1935 "
        "1947 1971-1972 1974 1984 1998-2010 2013 2020-2022 2030 2033-2035 2038 2040-2043 2045-2049 "
        "2065 2068 2099-2100 2103 2105-2107 2111 2119 2121 2126 2135 2144 2160-2161 2170 2179 "
        "2190-2191 2196 2200 2222 2251 2260 2288 2301 2323 2366 2381-2383 2393-2394 2399 2401 2492 "
        "2500 2522 2525 2557 2601-2602 2604-2605 2607-2608 2638 2701-2702 2710 2717-2718 2725 2800 "
        "2809 2811 2869 2875 2909-2910 2920 2967-2968 2998 3000-3001 3003 3005-3007 3011 3013 3017 "
        "3030-3031 3052 3071 3077 3128 3168 3211 3221 3260-3261 3268-3269 3283 3300-3301 3306 "
        "3322-3325 3333 3351 3367 3369-3372 3389-3390 3404 3476 3493 3517 3527 3546 3551 3580 3659 "
        "3689-3690 3703 3737 3766 3784 3800-3801 3809 3814 3826-3828 3851 3869 3871 3878 3880 3889 "
        "3905 3914 3918 3920 3945 3971 3986 3995 3998 4000-4006 4045 4111 4125-4126 4129 4224 4242 "
        "4279 4321 4343 4443-4446 4449 4550 4567 4662 4848 4899-4900 4998 5000-5004 5009 5030 5033 "
        "5050-5051 5054 5060-5061 5080 5087 5100-5102 5120 5190 5200 5214 5221-5222 5225-5226 5269 "
        "5280 5298 5357 5405 5414 5431-5432 5440 5500 5510 5544 5550 5555 5560 5566 5631 5633 5666 "
        "5678-5679 5718 5730 5800-5802 5810-5811 5815 5822 5825 5850 5859 5862 5877 5900-5904 "
        "5906-5907 5910-5911 5915 5922 5925 5950 5952 5959-5963 5987-5989 5998-6007 6009 6025 6059 "
        "6100-6101 6106 6112 6123 6129 6156 6346 6389 6502 6510 6543 6547 6565-6567 6580 6646 "
        "6666-6669 6689 6692 6699 6779 6788-6789 6792 6839 6881 6901 6969 7000-7002 7004 7007 7019 "
        "7025 7070 7100 7103 7106 7200-7201 7402 7435 7443 7496 7512 7625 7627 7676 7741 7777-7778 "
        "7800 7911 7920-7921 7937-7938 7999-8002 8007-8011 8021-8022 8031 8042 8045 8080-8090 8093 "
        "8099-8100 8180-8181 8192-8194 8200 8222 8254 8290-8292 8300 8333 8383 8400 8402 8443 8500 "
        "8600 8649 8651-8652 8654 8701 8800 8873 8888 8899 8994 9000-9003 9009-9011 9040 9050 9071 "
        "9080-9081 9090-9091 9099-9103 9110-9111 9200 9207 9220 9290 9415 9418 9485 9500 9502-9503 "
        "9535 9575 9593-9595 9618 9666 9876-9878 9898 9900 9917 9929 9943-9944 9968 9998-10004 "
        "10009-10010 10012 10024-10025 10082 10180 10215 10243 10566 10616-10617 10621 10626 "
        "10628-10629 10778 11110-11111 11967 12000 12174 12265 12345 13456 13722 13782-13783 14000 "
        "14238 14441-14442 15000 15002-15004 15660 15742 16000-16001 16012 16016 16018 16080 16113 "
        "16992-16993 17877 17988 18040 18101 18988 19101 19283 19315 19350 19780 19801 19842 20000 "
        "20005 20031 20221-20222 20828 21571 22939 23502 24444 24800 25734-25735 26214 27000 "
        "27352-27353 27355-27356 27715 28201 30000 30718 30951 31038 31337 32768-32785 33354 33899 "
        "34571-34573 35500 38292 40193 40911 41511 42510 44176 44442-44443 44501 45100 48080 "
        "49152-49161 49163 49165 49167 49175-49176 49400 49999-50003 50006 50300 50389 50500 50636 "
        "50800 51103 51493 52673 52822 52848 52869 54045 54328 55055-55056 55555 55600 56737-56738 "
        "57294 57797 58080 60020 60443 61532 61900 62078 63331 64623 64680 65000 65129 65389 280 4567 "
        "7001 8008 9080";
}

// Poll the keyboard for Ctrl+C so a running ping can be stopped (like Linux).
static bool ping_break_pressed() {
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

void NetworkCmds::ping(const std::string& target, uint32_t count, LineCallback emit) {
    if (WiFi.status() != WL_CONNECTED) {
        emit("Not connected to WiFi\n");
        Helpers::cmd_status = 1;
        return;
    }

    std::string host = ping_extract_host(target);
    if (host.empty()) {
        emit("ping: invalid target\n");
        Helpers::cmd_status = 1;
        return;
    }

    // If it is already an IP, use it as is; otherwise resolve via DNS.
    IPAddress ip;
    bool isIP = ip.fromString(String(host.c_str()));
    if (!isIP) {
        if (!WiFi.hostByName(host.c_str(), ip) || ip == IPAddress(0, 0, 0, 0)) {
            emit("ping: cannot resolve " + host + "\n");
            Helpers::cmd_status = 1;
            return;
        }
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "PING %s (%s)\n", host.c_str(), ip.toString().c_str());
    emit(buf);

    int sent = 0, recv = 0;
    float sum = 0.0f, sumSq = 0.0f, minT = 0.0f, maxT = 0.0f;
    bool interrupted = false;

    for (uint32_t i = 0; count == 0 || i < count; i++) {
        if (ping_break_pressed()) { interrupted = true; break; }

        sent++;
        bool ok = Ping.ping(ip, 1); // single echo request
        if (ok) {
            float t = Ping.averageTime(); // time of the last ping, ms
            recv++;
            sum += t;
            sumSq += t * t;
            if (recv == 1 || t < minT) minT = t;
            if (recv == 1 || t > maxT) maxT = t;
            snprintf(buf, sizeof(buf), "Reply from %s: icmp_seq=%d time=%.1f ms\n",
                     ip.toString().c_str(), sent, t);
        } else {
            snprintf(buf, sizeof(buf), "Request timed out: icmp_seq=%d\n", sent);
        }
        emit(buf);

        // gap between pings, staying responsive to Ctrl+C
        bool last = (count != 0 && i + 1 >= count);
        for (int d = 0; d < 5 && !last; d++) {
            if (ping_break_pressed()) { interrupted = true; break; }
            delay(60);
        }
        if (interrupted) break;
    }

    if (interrupted) emit("^C\n");

    int loss = (sent > 0) ? (100 * (sent - recv) / sent) : 0;
    snprintf(buf, sizeof(buf),
             "--- %s ping statistics ---\n%d sent, %d received, %d%% loss\n",
             host.c_str(), sent, recv, loss);
    emit(buf);

    if (recv > 0) {
        float avg = sum / recv;
        float var = (sumSq / recv) - (avg * avg);
        if (var < 0.0f) var = 0.0f; // guard against floating-point rounding
        float mdev = sqrtf(var);
        snprintf(buf, sizeof(buf),
                 "rtt min/avg/max/mdev = %.1f/%.1f/%.1f/%.1f ms\n",
                 minT, avg, maxT, mdev);
        emit(buf);
    } else {
        Helpers::cmd_status = 1; // 100% loss = failure (like real ping)
    }
}

// ---- wget : download a URL to a file (streamed, never buffered in RAM) ----

std::string NetworkCmds::filename_from_url(const std::string& url) {
    std::string s = url;
    size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    size_t q = s.find('?'); if (q != std::string::npos) s = s.substr(0, q);
    size_t h = s.find('#'); if (h != std::string::npos) s = s.substr(0, h);
    size_t slash = s.find_last_of('/');
    std::string name = (slash == std::string::npos) ? std::string("") : s.substr(slash + 1);
    if (name.empty()) name = "index.html";
    return name;
}

void NetworkCmds::wget(const std::string& args, LineCallback emit) {
    // tokenize on whitespace
    std::vector<std::string> toks;
    {
        size_t i = 0;
        while (i < args.size()) {
            while (i < args.size() && args[i] == ' ') i++;
            size_t j = i;
            while (j < args.size() && args[j] != ' ') j++;
            if (j > i) toks.push_back(args.substr(i, j - i));
            i = j;
        }
    }
    std::string url, outPath;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (toks[i] == "-o" && i + 1 < toks.size()) outPath = toks[++i];
        else if (url.empty() && toks[i][0] != '-') url = toks[i];
    }
    if (url.empty()) { emit("Usage: wget <url> [-o <path>]\n"); Helpers::cmd_status = 1; return; }
    if (WiFi.status() != WL_CONNECTED) { emit("Not connected to WiFi\n"); Helpers::cmd_status = 1; return; }

    bool https = (url.rfind("https://", 0) == 0);
    if (!https && url.rfind("http://", 0) != 0) {
        emit("wget: URL must start with http:// or https://\n");
        Helpers::cmd_status = 1;
        return;
    }

    std::string fname = outPath.empty() ? filename_from_url(url) : outPath;
    std::string abs = Helpers::make_absolute(fname);

    HTTPClient http;
    WiFiClientSecure sclient;
    WiFiClient pclient;
    bool ok;
    if (https) { sclient.setInsecure(); ok = http.begin(sclient, url.c_str()); } // no cert check
    else { ok = http.begin(pclient, url.c_str()); }
    if (!ok) { emit("wget: could not start request (bad URL?)\n"); Helpers::cmd_status = 1; return; }

    http.setUserAgent("hamsTerm/1.0");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char b[64];
        snprintf(b, sizeof(b), "wget: HTTP %d\n", code);
        emit(b);
        Helpers::cmd_status = 1;
        http.end();
        return;
    }

    File f = Helpers::fsOpen(abs, "w");
    if (!f) { emit("wget: cannot open output file\n"); http.end(); Helpers::cmd_status = 1; return; }

    int total = http.getSize(); // >=0 if Content-Length known, -1 if chunked/unknown
    {
        char b[96];
        if (total > 0) snprintf(b, sizeof(b), "Downloading %d bytes -> %s\n", total, fname.c_str());
        else           snprintf(b, sizeof(b), "Downloading -> %s\n", fname.c_str());
        emit(b);
    }

    int written = 0;
    bool failed = false;

    if (total >= 0) {
        // Content-Length known: the raw stream is exactly the body (no chunk
        // framing), so copy it ourselves. Crucially, the SD card can accept fewer
        // bytes than asked mid-transfer (a "short write"); we retry the remainder
        // instead of giving up. writeToStream() does not do this and aborts with
        // error -10 (HTTPC_ERROR_STREAM_WRITE) on bigger files. No display output
        // happens inside this loop (that would contend with SD on the SPI bus).
        WiFiClient* stream = http.getStreamPtr();
        uint8_t buf[2048];
        int remaining = total;
        uint32_t lastData = millis();
        while (remaining > 0) {
            size_t avail = stream->available();
            if (avail) {
                int toRead = (int)((avail > sizeof(buf)) ? sizeof(buf) : avail);
                if (toRead > remaining) toRead = remaining;
                int n = stream->readBytes(buf, toRead);
                int off = 0, stuck = 0;
                while (off < n) {
                    size_t w = f.write(buf + off, (size_t)(n - off));
                    off += (int)w;
                    if (w == 0) { if (++stuck > 200) break; delay(2); } // let the card catch up
                    else stuck = 0;
                }
                if (off < n) { failed = true; break; } // SD write truly stuck
                written += n;
                remaining -= n;
                lastData = millis();
            } else {
                if (!http.connected()) { failed = true; break; }    // connection dropped early
                if (millis() - lastData > 20000) { failed = true; break; } // stalled
                delay(1);
            }
        }
    } else {
        // Unknown length (chunked / close-delimited): writeToStream decodes chunked.
        written = http.writeToStream(&f);
        if (written < 0) failed = true;
    }

    f.close();
    http.end();

    if (failed) {
        char b[80];
        snprintf(b, sizeof(b), "wget: write failed after %d bytes (SD/network)\n",
                 written < 0 ? 0 : written);
        emit(b);
        Helpers::cmd_status = 1;
        Helpers::fsRemove(abs); // drop the partial file
        return;
    }
    char b[96];
    snprintf(b, sizeof(b), "Saved %s (%d bytes)\n", fname.c_str(), written);
    emit(b);
}
