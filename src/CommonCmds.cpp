#include "CommonCmds.h"
#include "Helpers.h"
#include <algorithm>

/* ===================== INIT ===================== */

void CommonCmds::begin() {
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed");
    }

    /*LittleFS.format();

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed");
    }*/

    File file = LittleFS.open("/cell", "w");
    if (file) {
        file.print(
            "   (\\_/)\n"
            "  ( o.o )\n"
            "  >  ^  <\n"
        );
        file.close();
    }
}

/* ===================== FS ===================== */

void CommonCmds::cat(const std::vector<std::string>& files, const std::string& outputFile, bool append, LineCallback emit) {
    constexpr size_t LINE_WIDTH = 34;
    std::string outFile = outputFile;

    Helpers::trim(outFile);

    // --- write to file ---
    File fileForText;
    if (!outFile.empty()) {

        fileForText = Helpers::fsOpen(outFile, append ? "a" : "w");
        if (!fileForText) {
            emit("cat: cannot write to " + Helpers::clearFilename(outFile) + "\n");
            return;
        }
    }

    for (const auto& filename : files) {

        if (!Helpers::fsExists(filename)) {
            emit("cat: " + Helpers::clearFilename(filename) + ": No such file or directory\n");
            continue;
        }

        File file = Helpers::fsOpen(filename, "r");
        if (!file || file.isDirectory()) {
            emit("cat: " + Helpers::clearFilename(filename) + " is a directory\n");
            continue;
        }

        std::string line;
        line.reserve(LINE_WIDTH);

        while (file.available()) {
            char c = file.read();

            // Line break
            if (c == '\n') {
                if(!outFile.empty()){
                    fileForText.print(line.c_str());
                    fileForText.print("\n");
                } else{
                    emit(line += "\n");
                }
                line.clear();
                continue;
            }

            // Ignore \r (Windows line endings)
            if (c == '\r') {
                continue;
            }

            line += c;

            // Reached screen width
            if (line.length() >= LINE_WIDTH) {
                if(!outFile.empty()){
                    fileForText.print(line.c_str());
                    fileForText.print("\n");
                } else{
                    emit(line += "\n");
                }
                line.clear();
            }
        }

        // File tail (last partial line)
        if (!line.empty()) {
            if(!outFile.empty()){
                    fileForText.print(line.c_str());
                    fileForText.print("\n");
                } else{
                    emit(line += "\n");
                }
        }

        file.close();
    }
    if(!outFile.empty()){
        fileForText.close();
    }
}

// head - first n lines of a file (streamed; reads only the beginning).
void CommonCmds::head(const std::string& file, size_t n, LineCallback emit) {
    File f = Helpers::fsOpen(file, "r");
    if (!f || f.isDirectory()) {
        emit("head: " + Helpers::clearFilename(file) + ": No such file\n");
        return;
    }

    const size_t MAX_LINE = 512; // guard against a file with no line breaks
    size_t count = 0;
    size_t bytes = 0;
    std::string line;

    while (f.available() && count < n) {
        char c = (char)f.read();
        line += c;
        if (c == '\n') {
            emit(line);
            line.clear();
            count++;
        } else if (line.size() >= MAX_LINE) {
            emit(line + "\n");
            line.clear();
            count++;
        }
        if ((++bytes & 0x3FF) == 0) delay(1); // watchdog
    }
    if (count < n && !line.empty()) {
        emit(line + "\n");
    }
    f.close();
}

// tail - last n lines of a file (ring buffer of n lines).
void CommonCmds::tail(const std::string& file, size_t n, LineCallback emit) {
    if (n == 0) return;
    File f = Helpers::fsOpen(file, "r");
    if (!f || f.isDirectory()) {
        emit("tail: " + Helpers::clearFilename(file) + ": No such file\n");
        return;
    }

    const size_t MAX_LINE = 512;
    std::vector<std::string> ring;
    ring.reserve(n);
    size_t start = 0; // index of the oldest line in the ring (once full)

    auto pushLine = [&](const std::string& l) {
        if (ring.size() < n) {
            ring.push_back(l);
        } else {
            ring[start] = l;
            start = (start + 1) % n;
        }
    };

    std::string line;
    size_t bytes = 0;
    while (f.available()) {
        char c = (char)f.read();
        line += c;
        if (c == '\n') {
            pushLine(line);
            line.clear();
        } else if (line.size() >= MAX_LINE) {
            pushLine(line + "\n");
            line.clear();
        }
        if ((++bytes & 0x3FF) == 0) delay(1); // watchdog
    }
    if (!line.empty()) {
        pushLine(line + "\n"); // last line without a trailing newline
    }
    f.close();

    for (size_t i = 0; i < ring.size(); i++) {
        emit(ring[(start + i) % ring.size()]);
    }
}

void CommonCmds::cd(const std::string& command, LineCallback emit) {
    std::string newPath = Helpers::make_absolute(command);

    if (Helpers::isSdPath(newPath) && !Helpers::sdMounted) {
        emit("cd: /sd: SD not mounted (use 'mount')\n");
        return;
    }

    File check = Helpers::fsOpen(newPath);
    if (check && check.isDirectory()) {
        Helpers::currentDir = newPath;
    } else {
        emit("cd: " + newPath + ": No such directory\n");
    }
    if (check){
        check.close();
    }
}

void CommonCmds::clear() {
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setCursor(0, 0);
}

/* ---- path helpers ---- */

std::string CommonCmds::path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string CommonCmds::base_name(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

/* ---- copy a single file ---- */

bool CommonCmds::copy_file(const std::string& src, const std::string& dst, LineCallback emit) {
    File srcFile = Helpers::fsOpen(src, "r");
    if (!srcFile) {
        emit("cp: " + Helpers::clearFilename(src) + ": No such file\n");
        return false;
    }

    File dstFile = Helpers::fsOpen(dst, "w");
    if (!dstFile) {
        emit("cp: cannot create " + Helpers::clearFilename(dst) + "\n");
        srcFile.close();
        return false;
    }

    uint8_t buf[256];
    size_t n;
    while ((n = srcFile.read(buf, sizeof(buf))) > 0) {
        dstFile.write(buf, n);
    }

    srcFile.close();
    dstFile.close();
    return true;
}

/* ---- recursive directory copy ---- */

bool CommonCmds::copy_recursive(const std::string& src, const std::string& dst, LineCallback emit) {
    File s = Helpers::fsOpen(src, "r");
    if (!s) {
        emit("cp: " + Helpers::clearFilename(src) + ": No such file or directory\n");
        return false;
    }

    // Plain file: just copy it
    if (!s.isDirectory()) {
        s.close();
        return copy_file(src, dst, emit);
    }

    // Directory: create the destination (if missing)
    if (!Helpers::fsExists(dst)) {
        if (!Helpers::fsMkdir(dst)) {
            emit("cp: cannot create directory " + Helpers::clearFilename(dst) + "\n");
            s.close();
            return false;
        }
    }

    // Collect the child list first, then recurse (so we don't keep
    // the directory iterator open during recursion)
    std::vector<std::pair<std::string, bool>> children; // <name, isDir>
    File child = s.openNextFile();
    while (child) {
        children.push_back({ base_name(child.name()), child.isDirectory() });
        child = s.openNextFile();
    }
    s.close();

    bool ok = true;
    for (const auto& c : children) {
        std::string childSrc = path_join(src, c.first);
        std::string childDst = path_join(dst, c.first);
        if (!copy_recursive(childSrc, childDst, emit)) ok = false;
    }
    return ok;
}

void CommonCmds::cp(const std::vector<std::string>& srcs, const std::string& dst, bool recursive, LineCallback emit) {
    if (srcs.empty()) {
        emit("cp: missing file operand\n");
        return;
    }

    bool dstExists = Helpers::fsExists(dst);
    bool dstIsDir = false;

    if (dstExists) {
        File d = Helpers::fsOpen(dst, "r");
        dstIsDir = d.isDirectory();
        d.close();
    }

    // When copying several items, dst MUST be a directory
    if (srcs.size() > 1 && !dstIsDir) {
        emit("cp: target '" + Helpers::clearFilename(dst) + "' is not a directory\n");
        return;
    }

    for (const auto& src : srcs) {

        File srcFile = Helpers::fsOpen(src, "r");
        if (!srcFile) {
            emit("cp: " + Helpers::clearFilename(src) + ": No such file\n");
            continue;
        }

        bool srcIsDir = srcFile.isDirectory();
        srcFile.close();

        // ---- build finalDst ----
        // If the destination is an existing directory, copy INTO it
        // under the original name (like normal cp).
        std::string finalDst = dst;
        if (dstIsDir) {
            finalDst = path_join(dst, base_name(src));
        }

        if (srcIsDir) {
            if (!recursive) {
                emit("cp: -r not specified; omitting directory " + Helpers::clearFilename(src) + "\n");
                continue;
            }
            // Guard: refuse to copy a directory into itself or below itself
            // (otherwise infinite recursion and FS overflow).
            if (finalDst == src || finalDst.rfind(src + "/", 0) == 0) {
                emit("cp: cannot copy a directory into itself: " + Helpers::clearFilename(src) + "\n");
                continue;
            }
            copy_recursive(src, finalDst, emit);
        } else {
            copy_file(src, finalDst, emit);
        }
    }
}

void CommonCmds::date(const std::string& outputFile, bool append, LineCallback emit) {
    struct tm timeinfo;
    std::string outFile = outputFile;

    if (!getLocalTime(&timeinfo)) {
        emit("No time (NTP not synced, connect to WiFi)\n");
        return;
    }

    Helpers::trim(outFile);

    char buffer[64];
    strftime(buffer, sizeof(buffer), "%a %b %d %H:%M:%S %Y\n", &timeinfo);

    if (!outFile.empty()) {

        File file = Helpers::fsOpen(outFile, append ? "a" : "w");
        if (!file) {
            emit("date: cannot write to " + Helpers::clearFilename(outFile) + "\n");
            return;
        }

        file.print(buffer);

        file.close();
        return;
    }

    emit(buffer);
}

void CommonCmds::echo(const std::string& text, const std::string& outputFile, bool append, LineCallback emit) {
    bool newline = true;
    std::string outText = text;
    std::string outFile = outputFile;

    // -n
    /*if (text.rfind("-n ", 0) == 0) {
        newline = false;
        text = text.substr(3);
    }*/

    // >>

    Helpers::trim(outText);
    Helpers::trim(outFile);

    // --- write to file ---
    if (!outFile.empty()) {

        File file = Helpers::fsOpen(outFile, append ? "a" : "w");
        if (!file) {
            emit("echo: cannot write to " + Helpers::clearFilename(outFile) + "\n");
            return;
        }

        file.print(outText.c_str());
        if (newline){
            file.print("\n");
        }

        file.close();
        return;
    }

    // --- normal output ---
    emit(outText + "\n");
}

void CommonCmds::ls(const std::string& currentDir, bool showAll, size_t maxShown, LineCallback emit) {
    File root = Helpers::fsOpen(currentDir, "r");
    if (!root || !root.isDirectory()) {
        emit("FS Error: Can't open directory\n");
        return;
    }

    Helpers::lastOutput.clear();
    // Cap the tab-completion data; otherwise a directory with
    // thousands of files would eat all the memory.
    const size_t LASTOUTPUT_CAP = 128;

    // The virtual SD mount point is visible at the internal FS root.
    if (currentDir == "/" && Helpers::sdMounted) {
        emit("[DIR] sd\n");
        Helpers::lastOutput.push_back("sd");
    }

    // IMPORTANT (fix for hang / out-of-memory on large directories):
    //  1) output is streamed, no vector accumulation - O(1) memory;
    //  2) a single pass over the directory (was two with rewindDirectory -
    //     which doubled SD reads and could silently fail on some
    //     drivers; we sacrifice dirs-first grouping for
    //     robustness - directories are still marked with the [DIR] prefix);
    //  3) a periodic delay(1) feeds the task watchdog (bare yield() does
    //     not reset it) and keeps the system from hanging/rebooting;
    //  4) for SCREEN output the line count is capped (maxShown);
    //     when redirecting to a file (maxShown == 0) everything is printed.
    size_t shown = 0;
    size_t scanned = 0;
    bool truncated = false;

    File file = root.openNextFile();
    while (file) {
        std::string base = base_name(file.name());
        bool isDir = file.isDirectory();
        size_t sz = isDir ? 0 : file.size();
        file.close(); // free the descriptor immediately

        bool hidden = !base.empty() && base[0] == '.';
        if (showAll || !hidden) {
            if (maxShown != 0 && shown >= maxShown) {
                truncated = true;
                break;
            }
            if (isDir) {
                emit("[DIR] " + base + "\n");
            } else {
                emit(" " + base + " (" + std::to_string(sz) + "b)\n");
            }
            if (Helpers::lastOutput.size() < LASTOUTPUT_CAP) {
                Helpers::lastOutput.push_back(base);
            }
            shown++;
        }

        // Feed the watchdog and stay responsive on large directories.
        if ((++scanned & 0x3F) == 0) {
            delay(1);
        }

        file = root.openNextFile();
    }

    root.close();

    if (truncated) {
        emit("... output truncated (use 'ls > file' for full list)\n");
    }
}

void CommonCmds::ls_target(const std::string& absPath, const std::string& name,
                           bool showAll, size_t maxShown, bool withHeader, LineCallback emit) {
    File f = Helpers::fsOpen(absPath, "r");
    if (!f) {
        emit("ls: " + name + ": not found\n");
        return;
    }
    bool isDir = f.isDirectory();
    size_t sz = isDir ? 0 : f.size();
    f.close();

    if (isDir) {
        if (withHeader) emit(name + ":\n");
        ls(absPath, showAll, maxShown, emit); // reuse directory listing
        if (withHeader) emit("\n");
    } else {
        // An explicitly named file is always shown (even if hidden).
        emit(" " + base_name(absPath) + " (" + std::to_string(sz) + "b)\n");
    }
}

void CommonCmds::mkdir(const std::vector<std::string>& dirs, LineCallback emit) {
    for (const auto& dirname : dirs) {
        if (!Helpers::fsMkdir(dirname)) {
            emit("mkdir: failed to create " + Helpers::clearFilename(dirname) + "\n");
        }
    }
}

void CommonCmds::mv(const std::vector<std::string>& srcs, const std::string& dst, LineCallback emit) {

    if (srcs.empty()) {
        emit("mv: missing file operand\n");
        return;
    }

    bool dstExists = Helpers::fsExists(dst);
    bool dstIsDir = false;

    if (dstExists) {
        File d = Helpers::fsOpen(dst, "r");
        dstIsDir = d.isDirectory();
        d.close();
    }

    // With several sources, dst MUST be a directory
    if (srcs.size() > 1 && !dstIsDir) {
        emit("mv: target '" + Helpers::clearFilename(dst) + "' is not a directory\n");
        return;
    }

    for (const auto& src : srcs) {

        if (!Helpers::fsExists(src)) {
            emit("mv: " + Helpers::clearFilename(src) + ": No such file\n");
            continue;
        }

        File srcFile = Helpers::fsOpen(src, "r");
        if (!srcFile) {
            emit("mv: cannot open " + Helpers::clearFilename(src) + "\n");
            continue;
        }

        bool srcIsDir = srcFile.isDirectory();
        srcFile.close();

        // ---- build finalDst ----
        // If the destination is an existing directory, move INTO it
        // under the original name (like normal mv).
        std::string finalDst = dst;
        if (dstIsDir) {
            finalDst = path_join(dst, base_name(src));
        }

        // Guard: refuse to move a directory into itself or below itself.
        if (srcIsDir && (finalDst == src || finalDst.rfind(src + "/", 0) == 0)) {
            emit("mv: cannot move a directory into itself: " + Helpers::clearFilename(src) + "\n");
            continue;
        }

        // ---- try rename first ----
        // LittleFS.rename can rename directories too, as long as the target
        // path does not exist yet (atomic). This is the fastest path.
        if (Helpers::fsRename(src, finalDst)) {
            continue; // success
        }

        // ---- fallback: copy + remove ----
        if (srcIsDir) {
            // Recursive directory move
            if (copy_recursive(src, finalDst, emit)) {
                if (!remove_recursive(src, emit)) {
                    emit("mv: copied but failed to remove source " + Helpers::clearFilename(src) + "\n");
                }
            } else {
                emit("mv: failed to copy directory " + Helpers::clearFilename(src) + "\n");
            }
        } else {
            // Single-file move
            if (copy_file(src, finalDst, emit)) {
                if (!Helpers::fsRemove(src)) {
                    emit("mv: failed to remove " + Helpers::clearFilename(src) + "\n");
                }
            }
        }
    }
}

void CommonCmds::pwd(const std::string& command, LineCallback emit) {
    emit(command + "\n");
}

/* ---- recursive remove ---- */

bool CommonCmds::remove_recursive(const std::string& path, LineCallback emit) {
    File p = Helpers::fsOpen(path, "r");
    if (!p) {
        emit("rm: cannot remove " + Helpers::clearFilename(path) + ": No such file or directory\n");
        return false;
    }

    // Plain file: remove directly
    if (!p.isDirectory()) {
        p.close();
        if (!Helpers::fsRemove(path)) {
            emit("rm: cannot remove " + Helpers::clearFilename(path) + "\n");
            return false;
        }
        return true;
    }

    // Directory: collect children, remove them, then the directory itself
    std::vector<std::string> children;
    File child = p.openNextFile();
    while (child) {
        children.push_back(base_name(child.name()));
        child = p.openNextFile();
    }
    p.close();

    bool ok = true;
    for (const auto& name : children) {
        if (!remove_recursive(path_join(path, name), emit)) ok = false;
    }

    if (!Helpers::fsRmdir(path)) {
        emit("rm: cannot remove directory " + Helpers::clearFilename(path) + "\n");
        ok = false;
    }
    return ok;
}

void CommonCmds::rm(const std::vector<std::string>& files, bool recursive, LineCallback emit) {
    if (files.empty()) {
        emit("rm: missing file operand\n");
        return;
    }

    for (const auto& filename : files) {
        // Guard against accidentally wiping the root
        if (filename == "/" || filename.empty()) {
            emit("rm: refusing to remove root directory\n");
            continue;
        }

        if (recursive) {
            remove_recursive(filename, emit);
            continue;
        }

        // Without -r: leave directories alone
        File f = Helpers::fsOpen(filename, "r");
        bool isDir = (f && f.isDirectory());
        if (f) f.close();

        if (isDir) {
            emit("rm: cannot remove " + Helpers::clearFilename(filename) + ": Is a directory\n");
            continue;
        }

        if (!Helpers::fsRemove(filename)) {
            emit("rm: cannot remove " + Helpers::clearFilename(filename) + ": No such file or directory\n");
        }
    }
}

void CommonCmds::rmdir(const std::string& command, LineCallback emit) {
    auto dirs = Helpers::parse_parts(command);

    if (dirs.empty()) {
        emit("rmdir: missing file operand\n");
        return;
    }

    for (const auto& dirname : dirs) {
        if (!Helpers::fsRmdir(dirname)) {
            emit("rmdir: cannot remove " + Helpers::clearFilename(dirname) + ": No such file or directory\n");
        }
    }
}

void CommonCmds::touch(const std::string& command, LineCallback emit) {
    auto files = Helpers::parse_parts(command);

    if (files.empty()) {
        emit("touch: missing file operand\n");
        return;
    }

    for (const auto& filename : files) {
        File file = Helpers::fsOpen(filename, "w");
        if (file) {
            file.close();
        } else {
            emit("touch: failed to create " + Helpers::clearFilename(filename) + "\n");
        }
    }   
}

// Glob match supporting * (any sequence) and ? (single char).
bool CommonCmds::wildcard_match(const std::string& pat, const std::string& str) {
    size_t p = 0, s = 0;
    size_t star = std::string::npos, ss = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            p++; s++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++; ss = s;
        } else if (star != std::string::npos) {
            p = star + 1; s = ++ss;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

void CommonCmds::find_recurse(const std::string& dir, const std::string& pattern, bool glob,
                              size_t maxResults, size_t& count, bool& truncated, LineCallback emit) {
    if (truncated) return;

    File d = Helpers::fsOpen(dir, "r");
    if (!d || !d.isDirectory()) return;

    // Buffer ONLY subdirectories (usually few) so recursion
    // does not keep the iterator open; files are streamed.
    std::vector<std::string> subdirs;
    size_t scanned = 0;

    File e = d.openNextFile();
    while (e) {
        std::string base = base_name(e.name());
        bool isDir = e.isDirectory();
        std::string full = path_join(dir, base);
        e.close();

        bool m = glob ? wildcard_match(pattern, base)
                      : (base.find(pattern) != std::string::npos);
        if (m) {
            if (maxResults != 0 && count >= maxResults) {
                truncated = true;
            } else {
                emit(full + "\n");
                count++;
            }
        }

        if (isDir) subdirs.push_back(full);

        if ((++scanned & 0x3F) == 0) delay(1); // watchdog
        if (truncated) { d.close(); return; }

        e = d.openNextFile();
    }
    d.close();

    for (const auto& sd : subdirs) {
        if (truncated) return;
        find_recurse(sd, pattern, glob, maxResults, count, truncated, emit);
    }
}

void CommonCmds::find(const std::string& startDir, const std::string& pattern,
                      size_t maxResults, LineCallback emit) {
    // If the pattern has * or ?, glob; otherwise substring search (more convenient).
    bool glob = pattern.find_first_of("*?") != std::string::npos;

    size_t count = 0;
    bool truncated = false;
    find_recurse(startDir, pattern, glob, maxResults, count, truncated, emit);

    if (truncated) {
        emit("... too many results (use find ... > file)\n");
    } else if (count == 0) {
        emit("find: no matches\n");
    }
}

// Expand a pattern whose wildcards may appear in ANY path segment. The pattern
// is made absolute first, then matched segment by segment against the real
// filesystem. Non-final segments must resolve to directories to keep going.
// Bash-style: a leading dot must be matched explicitly; nothing-matches -> empty.
std::vector<std::string> CommonCmds::expand_path(const std::string& pattern) {
    std::string abs = Helpers::make_absolute(pattern);

    // split into path segments
    std::vector<std::string> segs;
    {
        size_t i = 0;
        while (i < abs.size()) {
            while (i < abs.size() && abs[i] == '/') i++;
            size_t j = i; while (j < abs.size() && abs[j] != '/') j++;
            if (j > i) segs.push_back(abs.substr(i, j - i));
            i = j;
        }
    }
    if (segs.empty()) return {};

    const size_t MAXGLOB = 128;
    std::vector<std::string> current; current.push_back("/"); // start at root dir

    for (size_t s = 0; s < segs.size(); s++) {
        const std::string& seg = segs[s];
        bool last = (s + 1 == segs.size());
        bool isGlob = seg.find_first_of("*?") != std::string::npos;
        std::vector<std::string> next;

        for (size_t b = 0; b < current.size() && next.size() < MAXGLOB; b++) {
            const std::string& dir = current[b];
            std::string join = (dir == "/") ? ("/" + seg) : (dir + "/" + seg);

            if (!isGlob) {
                if (Helpers::fsExists(join)) {
                    if (last) next.push_back(join);
                    else {
                        File t = Helpers::fsOpen(join, "r");
                        bool d = (t && t.isDirectory()); if (t) t.close();
                        if (d) next.push_back(join);
                    }
                }
            } else {
                File dh = Helpers::fsOpen(dir, "r");
                if (dh && dh.isDirectory()) {
                    // collect (name, isDir) first to avoid iterating while opening
                    std::vector<std::pair<std::string, bool>> kids;
                    File e = dh.openNextFile();
                    while (e) {
                        std::string nm = base_name(e.name());
                        bool isd = e.isDirectory();
                        e.close();
                        if (!nm.empty()) kids.push_back(std::make_pair(nm, isd));
                        e = dh.openNextFile();
                    }
                    dh.close();
                    std::sort(kids.begin(), kids.end());
                    bool patDot = (!seg.empty() && seg[0] == '.');
                    for (size_t k = 0; k < kids.size() && next.size() < MAXGLOB; k++) {
                        const std::string& nm = kids[k].first;
                        if (nm[0] == '.' && !patDot) continue;        // hidden
                        if (!wildcard_match(seg, nm)) continue;
                        if (!last && !kids[k].second) continue;        // need a dir to descend
                        next.push_back((dir == "/") ? ("/" + nm) : (dir + "/" + nm));
                    }
                } else if (dh) {
                    dh.close();
                }
            }
        }

        current.swap(next);
        if (current.empty()) break;
    }
    return current;
}

// Back-compat single-token expander (now multi-component aware).
std::vector<std::string> CommonCmds::expand_token(const std::string& tok) {
    if (tok.find_first_of("*?") == std::string::npos) return { tok };
    std::vector<std::string> m = expand_path(tok);
    if (m.empty()) return { tok }; // no match -> keep literal
    return m;
}

std::string CommonCmds::expand_globs(const std::string& cmdline) {
    // Quote-aware tokenizing: each token carries whether it had an UNQUOTED
    // glob char. Only such tokens are expanded; quotes suppress globbing.
    std::vector<std::pair<std::string, bool>> toks = Helpers::tokenize_ex(cmdline);

    std::string out;
    bool first = true;
    for (size_t i = 0; i < toks.size(); i++) {
        const std::string& text = toks[i].first;
        bool hasGlob = toks[i].second;

        std::vector<std::string> pieces;
        if (i == 0 || !hasGlob) {
            pieces.push_back(text);               // command name or literal token
        } else {
            std::vector<std::string> m = expand_path(text);
            if (m.empty()) pieces.push_back(text); // no match -> keep literal
            else pieces = m;
        }

        for (const auto& p : pieces) {
            if (!first) out += " ";
            first = false;
            out += Helpers::requote(p);           // keep tokens with spaces atomic
        }
    }
    return out;
}
