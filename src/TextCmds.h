#pragma once

#include <string>
#include <vector>
#include <utility>
#include "Helpers.h"

// Text-processing commands. They all work the same way: they read either the
// files named on the command line, or - when no file is given - the text piped
// in from the previous stage of a pipeline. `input` carries that piped text and
// `hasInput` says whether a pipe actually fed this command (needed to tell
// "no input at all" from "an empty pipe").
//
// tr is the exception to the file rule: like POSIX tr, its operands are
// character sets, so it only ever reads the pipe.
class TextCmds {
public:
    // grep <pattern> [file...]      - lines containing the literal substring
    static void grep(const std::string& args, const std::string& input,
                     bool hasInput, LineCallback emit);

    // cut -f LIST [-d C] [-s] [file...] | cut -c LIST [file...]
    static void cut(const std::string& args, const std::string& input,
                    bool hasInput, LineCallback emit);

    // tr [-d] [-s] SET1 [SET2]      - translate/delete/squeeze characters
    static void tr(const std::string& args, const std::string& input,
                   bool hasInput, LineCallback emit);

    // wc [-l] [-w] [-c] [file...]   - count lines, words, bytes
    static void wc(const std::string& args, const std::string& input,
                   bool hasInput, LineCallback emit);

    // head/tail [-n N | -N] [file...]  - first/last N lines (default 10)
    static void head(const std::string& args, const std::string& input,
                     bool hasInput, LineCallback emit);
    static void tail(const std::string& args, const std::string& input,
                     bool hasInput, LineCallback emit);

    // sort [-r] [-n] [-u] [-f] [-k N] [-t C] [file...]
    // Holds every line in RAM, so the input is capped (SORT_MAX_LINES).
    static void sort(const std::string& args, const std::string& input,
                     bool hasInput, LineCallback emit);

    // uniq [-c] [-d] [-u] [-i] [file...]  - collapse *adjacent* duplicates
    static void uniq(const std::string& args, const std::string& input,
                     bool hasInput, LineCallback emit);
};
