#pragma once

#include <string>
#include <functional>

using LineCallback = std::function<void(const std::string&)>;

class SystemCmds {
public:
    static void help(LineCallback emit);
    static void sysinfo(LineCallback emit);
    static void freemem(LineCallback emit);
    static void df(LineCallback emit);
    static void battery(LineCallback emit);
};
