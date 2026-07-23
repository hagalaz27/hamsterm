#include "Tar.h"
#include "CommonCmds.h"   // base_name
#include <cstring>
#include <cstdio>
#include <vector>

namespace {

const int BLK = 512;

// ---- ustar header field helpers (all host-testable, no FS) ----

// Write (width-1) zero-padded octal digits followed by a NUL into field.
void write_octal(char* field, int width, unsigned long value) {
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%0*lo", width - 1, value);
    // value never exceeds width-1 digits on this device; copy the low digits.
    size_t len = strlen(tmp);
    if ((int)len > width - 1) {            // paranoia: keep the least-significant
        memcpy(field, tmp + (len - (width - 1)), width - 1);
    } else {
        memcpy(field, tmp, len);
        for (int i = len; i < width - 1; ++i) field[i] = '0';
    }
    field[width - 1] = '\0';
}

// Parse a run of octal digits (stops at the first non-octal char, e.g. NUL/space).
unsigned long parse_octal(const char* field, int width) {
    unsigned long v = 0;
    for (int i = 0; i < width; ++i) {
        char c = field[i];
        if (c < '0' || c > '7') break;
        v = v * 8 + (unsigned long)(c - '0');
    }
    return v;
}

// Fill a zeroed 512-byte ustar header. type: '0' file, '5' directory.
void build_header(char* h, const std::string& name, unsigned long size, char type) {
    memset(h, 0, BLK);
    memcpy(h, name.c_str(), name.size() < 100 ? name.size() : 100);
    write_octal(h + 100, 8, type == '5' ? 0755 : 0644); // mode
    write_octal(h + 108, 8, 0);                          // uid
    write_octal(h + 116, 8, 0);                          // gid
    write_octal(h + 124, 12, size);                      // size
    write_octal(h + 136, 12, 0);                         // mtime (0 - no RTC guarantee)
    memset(h + 148, ' ', 8);                             // checksum field = spaces
    h[156] = type;                                       // typeflag
    memcpy(h + 257, "ustar", 5);                         // magic "ustar\0"
    h[263] = '0'; h[264] = '0';                          // version "00"

    unsigned long sum = 0;
    for (int i = 0; i < BLK; ++i) sum += (unsigned char)h[i];
    char c[8];
    snprintf(c, sizeof(c), "%06lo", sum & 07777777UL);
    memcpy(h + 148, c, 6);
    h[154] = '\0';
    h[155] = ' ';
}

bool verify_checksum(const char* h) {
    unsigned long stored = parse_octal(h + 148, 8);
    unsigned long sum = 0;
    for (int i = 0; i < BLK; ++i)
        sum += (i >= 148 && i < 156) ? (unsigned long)' ' : (unsigned long)(unsigned char)h[i];
    return sum == stored;
}

bool is_zero_block(const char* h) {
    for (int i = 0; i < BLK; ++i) if (h[i]) return false;
    return true;
}

// Reject absolute paths and any ".." to prevent writing outside the cwd.
bool safe_name(const std::string& n) {
    if (n.empty() || n[0] == '/') return false;
    if (n.find("..") != std::string::npos) return false;
    return true;
}

// ---- create ----

bool add_path(File& ar, const std::string& absPath, const std::string& stored,
              int depth, LineCallback emit) {
    if (depth > 32) { emit("tar: too deep, skipping " + stored + "\n"); return false; }

    File f = Helpers::fsOpen(absPath, "r");
    if (!f) { emit("tar: cannot open " + Helpers::clearFilename(absPath) + "\n"); return false; }

    if (f.isDirectory()) {
        std::vector<std::string> children;
        File c = f.openNextFile();
        while (c) { children.push_back(CommonCmds::base_name(c.name())); c = f.openNextFile(); }
        f.close();

        if (stored.size() + 1 > 100) { emit("tar: name too long: " + stored + "\n"); return false; }
        char h[BLK];
        build_header(h, stored + "/", 0, '5');
        ar.write((const uint8_t*)h, BLK);

        bool ok = true;
        for (const auto& ch : children)
            if (!add_path(ar, absPath + "/" + ch, stored + "/" + ch, depth + 1, emit)) ok = false;
        return ok;
    }

    unsigned long size = (unsigned long)f.size();
    if (stored.size() > 100) { emit("tar: name too long: " + stored + "\n"); f.close(); return false; }

    char h[BLK];
    build_header(h, stored, size, '0');
    ar.write((const uint8_t*)h, BLK);

    char buf[BLK];
    unsigned long total = 0;
    while (total < size) {
        int r = f.read((uint8_t*)buf, BLK);
        if (r <= 0) break;
        if (r < BLK) memset(buf + r, 0, BLK - r); // zero-pad the final block
        ar.write((const uint8_t*)buf, BLK);
        total += (unsigned long)r;
        if ((total & 0x3FFF) == 0) delay(1);
    }
    f.close();
    if (total != size) { emit("tar: short read on " + Helpers::clearFilename(absPath) +
                              " (SD read bug on large files?)\n"); return false; }
    return true;
}

void create(const std::string& archiveAbs, const std::vector<std::string>& paths,
            LineCallback emit) {
    File ar = Helpers::fsOpen(archiveAbs, "w");
    if (!ar) { emit("tar: cannot create " + Helpers::clearFilename(archiveAbs) + "\n");
               Helpers::cmd_status = 1; return; }

    bool ok = true;
    for (const auto& p : paths) {
        std::string absP = Helpers::make_absolute(p);
        std::string stored = CommonCmds::base_name(absP);
        if (stored.empty()) { emit("tar: skipping " + p + "\n"); ok = false; continue; }
        if (!add_path(ar, absP, stored, 0, emit)) ok = false;
    }

    char zero[BLK]; memset(zero, 0, BLK);   // two zero blocks mark the end
    ar.write((const uint8_t*)zero, BLK);
    ar.write((const uint8_t*)zero, BLK);
    ar.close();

    if (ok) emit("tar: created " + Helpers::clearFilename(archiveAbs) + "\n");
    Helpers::cmd_status = ok ? 0 : 1;
}

// ---- extract / list ----

// Make every parent directory of absDir that does not exist yet.
void mkdir_parents(const std::string& absPath) {
    size_t start = (!absPath.empty() && absPath[0] == '/') ? 1 : 0;
    while (true) {
        size_t slash = absPath.find('/', start);
        if (slash == std::string::npos) break;   // last component is the leaf
        std::string sub = absPath.substr(0, slash);
        if (!sub.empty() && sub != "/" && !Helpers::fsExists(sub)) Helpers::fsMkdir(sub);
        start = slash + 1;
    }
}

void extract(const std::string& archiveAbs, bool listOnly, LineCallback emit) {
    File ar = Helpers::fsOpen(archiveAbs, "r");
    if (!ar) { emit("tar: cannot open " + Helpers::clearFilename(archiveAbs) + "\n");
               Helpers::cmd_status = 1; return; }

    char h[BLK], data[BLK];
    bool ok = true;
    while (true) {
        int r = ar.read((uint8_t*)h, BLK);
        if (r <= 0) break;                       // clean EOF
        if (r < BLK) { emit("tar: truncated archive\n"); ok = false; break; }
        if (is_zero_block(h)) break;             // end-of-archive marker
        if (!verify_checksum(h)) { emit("tar: bad header checksum\n"); ok = false; break; }

        size_t nlen = 0; while (nlen < 100 && h[nlen]) ++nlen;
        std::string name(h, nlen);
        unsigned long size = parse_octal(h + 124, 12);
        char type = h[156];
        unsigned long blocks = (size + BLK - 1) / BLK;

        if (listOnly) {
            emit(name + "\n");
            for (unsigned long i = 0; i < blocks; ++i) ar.read((uint8_t*)data, BLK);
            continue;
        }

        if (!safe_name(name)) {
            emit("tar: unsafe path skipped: " + name + "\n");
            for (unsigned long i = 0; i < blocks; ++i) ar.read((uint8_t*)data, BLK);
            ok = false;
            continue;
        }

        std::string abs = Helpers::make_absolute(name);
        if (type == '5') {
            if (!Helpers::fsExists(abs)) Helpers::fsMkdir(abs);
        } else {
            mkdir_parents(abs);
            File out = Helpers::fsOpen(abs, "w");
            if (!out) {
                emit("tar: cannot write " + Helpers::clearFilename(abs) + "\n");
                for (unsigned long i = 0; i < blocks; ++i) ar.read((uint8_t*)data, BLK);
                ok = false;
                continue;
            }
            unsigned long remaining = size;
            for (unsigned long i = 0; i < blocks; ++i) {
                if (ar.read((uint8_t*)data, BLK) != BLK) { ok = false; break; }
                unsigned long w = remaining < (unsigned long)BLK ? remaining : (unsigned long)BLK;
                if (w) out.write((const uint8_t*)data, w);
                remaining -= w;
                if ((i & 0x3F) == 0) delay(1);
            }
            out.close();
            emit(name + "\n");
        }
    }
    ar.close();
    Helpers::cmd_status = ok ? 0 : 1;
}

} // namespace

void TarCmds::run(const std::string& args, LineCallback emit) {
    std::vector<std::string> toks = Helpers::tokenize(args);
    char mode = 0;            // 'c', 'x' or 't'
    bool wantF = false;
    std::vector<std::string> positional;

    for (const auto& t : toks) {
        if (t.size() >= 1 && t[0] == '-') {
            for (size_t i = 1; i < t.size(); ++i) {
                char c = t[i];
                if (c == 'c' || c == 'x' || c == 't') mode = c;
                else if (c == 'f') wantF = true;
                else { emit(std::string("tar: unknown flag -") + c + "\n"); Helpers::cmd_status = 1; return; }
            }
        } else {
            positional.push_back(t);
        }
    }

    if (mode == 0) { emit("Usage: tar -c|-x|-t -f <archive> [path...]\n"); Helpers::cmd_status = 1; return; }
    if (!wantF || positional.empty()) { emit("tar: -f <archive> is required\n"); Helpers::cmd_status = 1; return; }

    std::string archive = Helpers::make_absolute(positional[0]);
    std::vector<std::string> paths(positional.begin() + 1, positional.end());

    if (mode == 'c') {
        if (paths.empty()) { emit("tar: nothing to archive\n"); Helpers::cmd_status = 1; return; }
        create(archive, paths, emit);
    } else if (mode == 'x') {
        extract(archive, false, emit);
    } else { // 't'
        extract(archive, true, emit);
    }
}
