#pragma once

#include <string>
#include "Helpers.h"

// tar - pack/unpack files and directories in the classic ustar format (512-byte
// blocks, no compression). Streamed in 512-byte blocks so nothing large sits in
// RAM. Long names (>100 bytes) are rejected (ustar limit; no GNU extensions).
//
//   tar -c -f <archive> <path>...   create an archive from the paths
//   tar -x -f <archive>             extract into the current directory
//   tar -t -f <archive>             list the contents
//
// Flags may be combined (tar -cf a.tar dir). -f <archive> is required (there is
// no useful binary stdout on the device).
class TarCmds {
public:
    static void run(const std::string& args, LineCallback emit);
};
