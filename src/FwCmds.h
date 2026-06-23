#pragma once

#include <string>
#include <functional>

using LineCallback = std::function<void(const std::string&)>;

// `fw` - manage OTA app slots: list them, install a firmware from a URL straight
// into the inactive slot (no SD / no filesystem needed), and switch which slot
// boots. Slot 1 = ota_0 = hamsTerm; slot 2 = ota_1 = an installed firmware.
class FwCmds {
public:
    static void fw(const std::string& args, LineCallback emit);

private:
    static void list(LineCallback emit);
    static void install(const std::string& url, LineCallback emit);
    static void boot(int slot, LineCallback emit);
};
