#pragma once

#include <M5Cardputer.h>
#include <LittleFS.h>
#include <vector>
#include <string>
#include <functional>
#include <time.h>

using LineCallback = std::function<void(const std::string&)>;

struct Entry {
    std::string name;
    size_t size;
};

class CommonCmds {
public:
    // lifecycle
    static void begin();

    // filesystem
    static void cat(const std::vector<std::string>& files, const std::string& outputFile, bool append, LineCallback emit);
    static void head(const std::string& file, size_t n, LineCallback emit);
    static void tail(const std::string& file, size_t n, LineCallback emit);
    static void cd(const std::string& command, LineCallback emit);
    static void clear();
    static void cp(const std::vector<std::string>& srcs, const std::string& dst, bool recursive, LineCallback emit);
    static void date(const std::string& outputFile, bool append, LineCallback emit);
    static void echo(const std::string& text, const std::string& outputFile, bool append, LineCallback emit);
    static void ls(const std::string& currentDir, bool showAll, size_t maxShown, LineCallback emit);
    // List one operand: a directory (its contents) or a file (its entry).
    static void ls_target(const std::string& absPath, const std::string& name,
                          bool showAll, size_t maxShown, bool withHeader, LineCallback emit);
    static void mkdir(const std::vector<std::string>& dirs, LineCallback emit);
    static void mv(const std::vector<std::string>& srcs, const std::string& dst, LineCallback emit);
    static void rm(const std::vector<std::string>& files, bool recursive, LineCallback emit);
    static void rmdir(const std::string& command, LineCallback emit);
    static void pwd(const std::string& command, LineCallback emit);
    static void touch(const std::string& command, LineCallback emit);
    static void find(const std::string& startDir, const std::string& pattern, size_t maxResults, LineCallback emit);

    // Shell-style expansion of * and ? wildcards in a command line, against the
    // filesystem. Used so every command (not just find) understands globs.
    static std::string expand_globs(const std::string& cmdline);

private:
    static std::vector<std::string> expand_token(const std::string& tok);
    // Multi-component glob: expand a (possibly absolute) pattern whose '*'/'?'
    // may appear in ANY path segment (e.g. "a*/b*/c.log"). Returns matching
    // absolute paths, or empty if nothing matches.
    static std::vector<std::string> expand_path(const std::string& absPattern);
    // Internal helpers for recursive operations
    static std::string path_join(const std::string& a, const std::string& b);
    static std::string base_name(const std::string& path);
    static bool copy_file(const std::string& src, const std::string& dst, LineCallback emit);
    static bool copy_recursive(const std::string& src, const std::string& dst, LineCallback emit);
    static bool remove_recursive(const std::string& path, LineCallback emit);
    static bool wildcard_match(const std::string& pat, const std::string& str);
    static void find_recurse(const std::string& dir, const std::string& pattern, bool glob,
                             size_t maxResults, size_t& count, bool& truncated, LineCallback emit);
};
