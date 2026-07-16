#include "TextCmds.h"
#include <cstdlib>
#include <cstdio>

namespace {

// Input for a filter command: the concatenation of the named files (resolved
// against the current directory), or the piped input when no file is given.
// Returns false (and emits an error) if a file can't be read.
bool read_input(const std::vector<std::string>& files, const char* name,
                const std::string& piped, std::string& out, LineCallback emit) {
    if (files.empty()) { out = piped; return true; }
    out.clear();
    for (const auto& f : files) {
        std::string abs = Helpers::make_absolute(f);
        if (!Helpers::fsExists(abs)) {
            emit(std::string(name) + ": " + Helpers::clearFilename(f) +
                 ": No such file or directory\n");
            return false;
        }
        File fp = Helpers::fsOpen(abs, "r");
        if (!fp || fp.isDirectory()) {
            if (fp) fp.close();
            emit(std::string(name) + ": " + Helpers::clearFilename(f) + ": cannot read\n");
            return false;
        }
        size_t bytes = 0;
        while (fp.available()) {
            out += (char)fp.read();
            if ((++bytes & 0x3FF) == 0) delay(1); // watchdog
        }
        fp.close();
    }
    return true;
}

// ---- cut helpers ----

// Parse a cut LIST like "1,3,5-7,10-" into (lo,hi) ranges. hi == -1 means
// "open" (to end of line). Sets ok=false on a malformed entry.
std::vector<std::pair<int,int>> parse_cut_list(const std::string& s, bool& ok) {
    std::vector<std::pair<int,int>> out;
    ok = true;
    size_t start = 0;
    while (true) {
        size_t comma = s.find(',', start);
        std::string part = (comma == std::string::npos) ? s.substr(start)
                                                        : s.substr(start, comma - start);
        if (!part.empty()) {
            size_t dash = part.find('-');
            int lo, hi;
            if (dash == std::string::npos) {
                lo = hi = atoi(part.c_str());
                if (lo < 1) ok = false;
            } else {
                std::string L = part.substr(0, dash), R = part.substr(dash + 1);
                lo = L.empty() ? 1 : atoi(L.c_str());
                hi = R.empty() ? -1 : atoi(R.c_str());
                if (lo < 1 || (hi != -1 && hi < lo)) ok = false;
            }
            out.push_back(std::make_pair(lo, hi));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

bool cut_selected(const std::vector<std::pair<int,int>>& r, int idx) {
    for (const auto& p : r)
        if (idx >= p.first && (p.second == -1 || idx <= p.second)) return true;
    return false;
}

// ---- tr helpers ----

// Expand a tr SET: process escapes (\n \t \r \\) and ranges (a-z, 0-9) into
// an explicit character string.
std::string tr_expand_set(const std::string& s) {
    std::string raw;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'n') raw += '\n';
            else if (n == 't') raw += '\t';
            else if (n == 'r') raw += '\r';
            else if (n == '\\') raw += '\\';
            else raw += n;
            ++i;
        } else {
            raw += s[i];
        }
    }
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (i + 2 < raw.size() && raw[i + 1] == '-') {
            unsigned char lo = (unsigned char)raw[i], hi = (unsigned char)raw[i + 2];
            if (lo <= hi) { for (int c = lo; c <= hi; ++c) out += (char)c; i += 2; continue; }
        }
        out += raw[i];
    }
    return out;
}

// ---- wc helpers ----

// Count lines/words/bytes of one chunk of text.
void wc_count(const std::string& s, long& lines, long& words, long& bytes) {
    lines = 0; words = 0; bytes = (long)s.size();
    bool inWord = false;
    for (char c : s) {
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') inWord = false;
        else if (!inWord) { inWord = true; words++; }
    }
    // A final line without a trailing newline still counts as a line.
    if (!s.empty() && s.back() != '\n') lines++;
}

// Format one wc result row honouring the selected counters.
std::string wc_row(long l, long w, long b, bool sl, bool sw, bool sb,
                   const std::string& label) {
    char buf[64];
    std::string out;
    if (sl) { snprintf(buf, sizeof(buf), "%8ld", l); out += buf; }
    if (sw) { snprintf(buf, sizeof(buf), "%8ld", w); out += buf; }
    if (sb) { snprintf(buf, sizeof(buf), "%8ld", b); out += buf; }
    if (!label.empty()) out += " " + label;
    out += "\n";
    return out;
}

// ---- head/tail file readers (streaming: never load the whole file) ----

void head_file(const std::string& file, size_t n, LineCallback emit) {
    File f = Helpers::fsOpen(file, "r");
    if (!f || f.isDirectory()) {
        emit("head: " + Helpers::clearFilename(file) + ": No such file\n");
        Helpers::cmd_status = 1;
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

void tail_file(const std::string& file, size_t n, LineCallback emit) {
    if (n == 0) return;
    File f = Helpers::fsOpen(file, "r");
    if (!f || f.isDirectory()) {
        emit("tail: " + Helpers::clearFilename(file) + ": No such file\n");
        Helpers::cmd_status = 1;
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

// Shared body of head and tail.
void head_tail(bool isHead, const std::string& args, const std::string& input,
               bool hasInput, LineCallback emit) {
    auto toks = Helpers::tokenize(args);
    size_t n = 10;
    std::vector<std::string> names;

    for (size_t i = 0; i < toks.size(); ++i) {
        const std::string& t = toks[i];
        if (t == "-n" && i + 1 < toks.size()) {
            long v;
            if (Helpers::parse_uint(toks[++i], v) && v >= 1) {
                if (v > 100) v = 100; // upper bound
                n = (size_t)v;
            }
        } else if (t.size() > 1 && t[0] == '-') {
            long v;
            if (Helpers::parse_uint(t.substr(1), v) && v >= 1) {
                if (v > 100) v = 100;
                n = (size_t)v;
            }
        } else {
            names.push_back(t); // keep every file, not just the last
        }
    }

    if (names.empty()) {
        // No file operand: act as a pipe filter over the piped input.
        if (!hasInput) {
            emit(std::string("Usage: ") + (isHead ? "head" : "tail") + " [-n N] <file>...\n");
            Helpers::cmd_status = 1;
            return;
        }
        auto lines = Helpers::split_lines(input);
        size_t total = lines.size();
        size_t from = 0, to = total;
        if (isHead) { if (total > n) to = n; }
        else        { if (total > n) from = total - n; }
        for (size_t i = from; i < to; ++i) emit(lines[i] + "\n");
        Helpers::cmd_status = 0;
        return;
    }

    bool multi = names.size() > 1;
    for (size_t i = 0; i < names.size(); ++i) {
        if (multi) {
            if (i) emit("\n");
            emit("==> " + names[i] + " <==\n"); // GNU-style header
        }
        std::string file = Helpers::make_absolute(names[i]);
        if (isHead) head_file(file, n, emit);
        else        tail_file(file, n, emit);
    }
}

} // namespace

// ---- public commands ----

void TextCmds::grep(const std::string& args, const std::string& input,
                    bool hasInput, LineCallback emit) {
    (void)hasInput;
    auto toks = Helpers::tokenize(args);
    if (toks.empty()) {
        emit("Usage: grep <pattern> [file...]\n");
        Helpers::cmd_status = 1;
        return;
    }
    std::string pat = toks[0];
    std::vector<std::string> files(toks.begin() + 1, toks.end());
    std::string data;
    if (!read_input(files, "grep", input, data, emit)) {
        Helpers::cmd_status = 1;
        return;
    }
    bool matched = false;
    for (const auto& line : Helpers::split_lines(data)) {
        if (line.find(pat) != std::string::npos) {
            emit(line + "\n");
            matched = true;
        }
    }
    Helpers::cmd_status = matched ? 0 : 1;
}

void TextCmds::cut(const std::string& args, const std::string& input,
                   bool hasInput, LineCallback emit) {
    (void)hasInput;
    auto toks = Helpers::tokenize(args);
    char mode = 0;              // 'f' fields, 'c' characters
    std::string list;
    char delim = ' ';
    bool suppress = false, haveList = false;
    std::vector<std::string> files;
    for (size_t i = 0; i < toks.size(); ++i) {
        const std::string& t = toks[i];
        if (t == "-f") { mode = 'f'; if (i + 1 < toks.size()) { list = toks[++i]; haveList = true; } }
        else if (t.rfind("-f", 0) == 0 && t.size() > 2) { mode = 'f'; list = t.substr(2); haveList = true; }
        else if (t == "-c") { mode = 'c'; if (i + 1 < toks.size()) { list = toks[++i]; haveList = true; } }
        else if (t.rfind("-c", 0) == 0 && t.size() > 2) { mode = 'c'; list = t.substr(2); haveList = true; }
        else if (t == "-d") { if (i + 1 < toks.size()) { std::string d = toks[++i]; delim = d.empty() ? ' ' : d[0]; } }
        else if (t.rfind("-d", 0) == 0 && t.size() > 2) { delim = t[2]; }
        else if (t == "-s") { suppress = true; }
        else { files.push_back(t); } // a file operand (read instead of the pipe)
    }
    if (mode == 0 || !haveList) {
        emit("Usage: cut -f LIST [-d C] [-s] [file] | cut -c LIST [file]   (LIST: 1,3,5-7,10-)\n");
        Helpers::cmd_status = 1;
        return;
    }
    bool okList = true;
    auto ranges = parse_cut_list(list, okList);
    if (!okList || ranges.empty()) {
        emit("cut: invalid list: " + list + "\n");
        Helpers::cmd_status = 1;
        return;
    }
    std::string data;
    if (!read_input(files, "cut", input, data, emit)) {
        Helpers::cmd_status = 1;
        return;
    }
    for (const auto& line : Helpers::split_lines(data)) {
        if (mode == 'c') {
            std::string out;
            for (int p = 1; p <= (int)line.size(); ++p)
                if (cut_selected(ranges, p)) out += line[p - 1];
            emit(out + "\n");
        } else {
            std::vector<std::string> fields;
            size_t start = 0, pos;
            while ((pos = line.find(delim, start)) != std::string::npos) {
                fields.push_back(line.substr(start, pos - start));
                start = pos + 1;
            }
            fields.push_back(line.substr(start));
            if (fields.size() == 1) {          // no delimiter on this line
                if (!suppress) emit(line + "\n");
                continue;
            }
            std::string out; bool first = true;
            for (int i = 1; i <= (int)fields.size(); ++i) {
                if (cut_selected(ranges, i)) {
                    if (!first) out += delim;
                    out += fields[i - 1];
                    first = false;
                }
            }
            emit(out + "\n");
        }
    }
    Helpers::cmd_status = 0;
}

void TextCmds::tr(const std::string& args, const std::string& input,
                  bool hasInput, LineCallback emit) {
    (void)hasInput;
    auto toks = Helpers::tokenize(args);
    bool del = false, squeeze = false;
    std::vector<std::string> sets;
    for (const auto& t : toks) {
        if (t.size() >= 2 && t[0] == '-' &&
            t.find_first_not_of("ds", 1) == std::string::npos) {
            if (t.find('d') != std::string::npos) del = true;
            if (t.find('s') != std::string::npos) squeeze = true;
        } else {
            sets.push_back(t);
        }
    }
    // Operand count check.
    bool ok = true;
    if (!del && !squeeze)      ok = (sets.size() >= 2); // translate needs both
    else                       ok = (sets.size() >= 1); // -d/-s need at least SET1
    if (!ok) {
        emit("Usage: tr [-d] [-s] SET1 [SET2]   (ranges a-z 0-9, escapes \\n \\t)\n");
        Helpers::cmd_status = 1;
        return;
    }

    std::string set1 = sets.size() >= 1 ? tr_expand_set(sets[0]) : std::string();
    std::string set2 = sets.size() >= 2 ? tr_expand_set(sets[1]) : std::string();

    bool translate = (!del && !set2.empty());

    unsigned char table[256];
    for (int i = 0; i < 256; ++i) table[i] = (unsigned char)i;
    if (translate && !set2.empty()) {
        for (size_t j = 0; j < set1.size(); ++j)
            table[(unsigned char)set1[j]] = (unsigned char)set2[j < set2.size() ? j : set2.size() - 1];
    }

    bool delset[256];  for (int i = 0; i < 256; ++i) delset[i] = false;
    if (del) for (unsigned char c : set1) delset[c] = true;

    bool sqset[256];   for (int i = 0; i < 256; ++i) sqset[i] = false;
    if (squeeze) {
        const std::string& S = (translate || del) ? set2 : set1;
        for (unsigned char c : S) sqset[c] = true;
    }

    std::string out;
    unsigned char prev = 0; bool havePrev = false;
    for (unsigned char c : input) {
        if (del && delset[c]) continue;
        unsigned char o = translate ? table[c] : c;
        if (squeeze && sqset[o] && havePrev && prev == o) continue;
        out += (char)o;
        prev = o; havePrev = true;
    }
    emit(out);
    Helpers::cmd_status = 0;
}

void TextCmds::wc(const std::string& args, const std::string& input,
                  bool hasInput, LineCallback emit) {
    (void)hasInput;
    auto toks = Helpers::tokenize(args);
    bool sl = false, sw = false, sb = false;
    std::vector<std::string> files;
    for (const auto& t : toks) {
        if (t.size() >= 2 && t[0] == '-' &&
            t.find_first_not_of("lwc", 1) == std::string::npos) {
            if (t.find('l') != std::string::npos) sl = true;
            if (t.find('w') != std::string::npos) sw = true;
            if (t.find('c') != std::string::npos) sb = true;
        } else {
            files.push_back(t);
        }
    }
    if (!sl && !sw && !sb) { sl = sw = sb = true; } // default: all three

    if (files.empty()) {
        std::string data;
        if (!read_input(files, "wc", input, data, emit)) { Helpers::cmd_status = 1; return; }
        long l, w, b;
        wc_count(data, l, w, b);
        emit(wc_row(l, w, b, sl, sw, sb, ""));
        Helpers::cmd_status = 0;
        return;
    }

    long tl = 0, tw = 0, tb = 0;
    bool anyFail = false;
    for (const auto& f : files) {
        std::vector<std::string> one(1, f);
        std::string data;
        if (!read_input(one, "wc", input, data, emit)) { anyFail = true; continue; }
        long l, w, b;
        wc_count(data, l, w, b);
        tl += l; tw += w; tb += b;
        emit(wc_row(l, w, b, sl, sw, sb, f));
    }
    if (files.size() > 1) emit(wc_row(tl, tw, tb, sl, sw, sb, "total"));
    Helpers::cmd_status = anyFail ? 1 : 0;
}

void TextCmds::head(const std::string& args, const std::string& input,
                    bool hasInput, LineCallback emit) {
    head_tail(true, args, input, hasInput, emit);
}

void TextCmds::tail(const std::string& args, const std::string& input,
                    bool hasInput, LineCallback emit) {
    head_tail(false, args, input, hasInput, emit);
}
