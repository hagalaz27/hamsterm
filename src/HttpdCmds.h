#pragma once

#include <string>
#include <functional>

using LineCallback = std::function<void(const std::string&)>;

// `httpd` - a single HTTP file manager on port 80. The page is baked into the
// firmware (PROGMEM). It browses, downloads, uploads, edits, deletes files and
// creates folders, confined to a served root (default "/", i.e. internal + /sd).
// Routes (the "API") are fixed in firmware. The server listens on every active
// interface, so it is reachable on both the SoftAP IP and the Wi-Fi IP.
class HttpdCmds {
public:
    static void httpd(const std::string& args, LineCallback emit); // start [path] | stop | status
    static void loop();      // call from the main loop to service requests
    static bool running();

private:
    static void start(LineCallback emit, const std::string& rootArg);
    static void stop(LineCallback emit);
    static void status(LineCallback emit);
};
