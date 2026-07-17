#pragma once

#include <M5Cardputer.h>
#include <vector>
#include <string>
#include <set>
#include <map>
#include <memory>
#include <cstdlib>
#include <cerrno>
#include "CommonCmds.h"
#include "NetworkCmds.h"
#include "AirCmds.h"
#include "HwCmds.h"
#include "TextCmds.h"
#include "WiFiCmds.h"
#include "SystemCmds.h"
#include "HttpdCmds.h"
#include "Editor.h"
#include "Telnet.h"
#include "Ssh.h"
#include "Helpers.h"

class Terminal {
private:
    std::string command = "";
    size_t cursorPos = 0;                  // edit cursor within `command` (0..len)
    int inputStartY = 0;                   // screen Y of the input line's first row
    int scriptDepth = 0;                   // nesting depth of running scripts (sh)
    int chainDepth = 0;                    // depth of &&/|| chain expansion
    bool capturing = false;                // command substitution: route output to a buffer
    std::string captureBuf;                // buffer for the current $(...) capture
    std::string pipeInput;                 // stdin for a filter command (left side of a pipe)
    bool hasPipeInput = false;             // whether pipeInput is meaningful for this command
    bool scriptInterrupted = false;        // Ctrl+C during a script aborts the rest of it
    static const size_t CAPTURE_CAP = 4096; // max bytes kept from a $(...) command
    using CaptureFn = std::function<std::string(const std::string&)>;
    std::vector<std::string> scriptArgs;   // positional params: [0]=$0 (name), [1..]=$1..
    std::string prompt = ">";
    std::vector<std::string> history;
    int max_history = 100;
    int scroll_offset = 0;
    bool liveCommand = false; // true while a long-running command (air w) draws output
    int lines_on_screen = 10;
    int commandNumber = -1;
    std::string previousValue = "";

    // FIX: tab-completion now remembers which part of the token the user
    // typed (tabPrefix) and how many candidates actually match,
    // so it doesn't glue the filename onto already-typed characters.
    std::string tabPrefix = "";
    std::vector<std::string> tabMatches;

    // --- Command history (recall with up/down arrows) ---
    std::vector<std::string> cmdHistory;   // entered commands
    int historyIndex = 0;                  // position; == cmdHistory.size() -> the live line
    std::string savedLine = "";            // unfinished input saved when entering history
    int max_cmd_history = 50;

    // --- Editor ---
    Editor* editor = nullptr;
    bool editorActive = false;

    // --- Telnet ---
    TelnetSession* telnet = nullptr;
    bool telnetActive = false;

    // --- SSH ---
    SshSession* ssh = nullptr;
    bool sshActive = false;
    bool sshWaitingForPass = false;
    std::string sshUser, sshHost;
    uint16_t sshPort = 22;

    // --- User variables (expand as $NAME / ${NAME}) ---
    std::map<std::string, std::string> vars;

public:
    Terminal() {
        load_vars(); // restore user variables saved from a previous session
        sync_tz();   // apply the saved TZ (if any) so date shows local time
        M5Cardputer.Display.setRotation(1);
        M5Cardputer.Display.setTextScroll(true);
        M5Cardputer.Display.fillScreen(BLACK);
        M5Cardputer.Display.setCursor(0, 0);
        M5Cardputer.Display.setFont(&fonts::efontCN_14);
        M5Cardputer.Display.setTextSize(1);
        run_profile();  // auto-run commands from /.profile first (if present)
        print_prompt(); // then show the prompt, after any script output
    }

    // Run commands from /.profile (internal filesystem) at startup, one per
    // line, exactly as if each were typed at the prompt. Lines that are empty
    // or begin with '#' are skipped. Handy for setting variables, cd-ing to a
    // working dir, or connecting to Wi-Fi on boot.
    // ---- Script control flow: if / then / elif / else / fi, loops ----
    enum class ScriptKw { NONE, IF, THEN, ELIF, ELSE, FI,
                          WHILE, UNTIL, FOR, DO, DONE };

    // Classify a (flattened) logical line by its first word.
    static ScriptKw script_kw(const std::string& line) {
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) return ScriptKw::NONE;
        size_t b = line.find_first_of(" \t", a);
        std::string w = line.substr(a, (b == std::string::npos ? line.size() : b) - a);
        if (w == "if")    return ScriptKw::IF;
        if (w == "then")  return ScriptKw::THEN;
        if (w == "elif")  return ScriptKw::ELIF;
        if (w == "else")  return ScriptKw::ELSE;
        if (w == "fi")    return ScriptKw::FI;
        if (w == "while") return ScriptKw::WHILE;
        if (w == "until") return ScriptKw::UNTIL;
        if (w == "for")   return ScriptKw::FOR;
        if (w == "do")    return ScriptKw::DO;
        if (w == "done")  return ScriptKw::DONE;
        return ScriptKw::NONE;
    }

    // Block openers increase nesting; closers decrease it. Used by all the
    // delimiter scanners so if..fi and while/for/until..done nest correctly.
    static bool kw_is_opener(ScriptKw k) {
        return k == ScriptKw::IF || k == ScriptKw::WHILE ||
               k == ScriptKw::FOR || k == ScriptKw::UNTIL;
    }
    static bool kw_is_closer(ScriptKw k) {
        return k == ScriptKw::FI || k == ScriptKw::DONE;
    }

    // Text after the first word (the condition for if/elif), trimmed.
    static std::string after_keyword(const std::string& line) {
        size_t a = line.find_first_not_of(" \t");
        size_t b = (a == std::string::npos) ? std::string::npos
                                            : line.find_first_of(" \t", a);
        if (b == std::string::npos) return "";
        std::string rest = line.substr(b);
        Helpers::trim(rest);
        return rest;
    }

    // Split a line on top-level ';' (command separator), honoring quotes.
    // Split a command line on top-level '|' (single pipe), honouring quotes and
    // parentheses so '|' inside quotes or $( ) is not a pipe. '||' is left intact
    // (it is the OR operator, already handled by the chain splitter).
    static std::vector<std::string> split_pipes(const std::string& in) {
        std::vector<std::string> out;
        std::string cur; char quote = 0; int paren = 0;
        for (size_t i = 0; i < in.size(); ++i) {
            char c = in[i];
            if (quote) {
                // An escaped quote must not end the quoted text, or `tr -d "\""`
                // would leave the rest of the line (including any |) quoted.
                if (Helpers::escaped_pair(in, i, quote)) {
                    cur += c; cur += in[i + 1]; ++i; continue;
                }
                cur += c; if (c == quote) quote = 0; continue;
            }
            if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
            if (c == '(') { paren++; cur += c; continue; }
            if (c == ')') { if (paren > 0) paren--; cur += c; continue; }
            if (c == '|' && paren == 0) {
                if (i + 1 < in.size() && in[i + 1] == '|') { cur += "||"; ++i; continue; }
                out.push_back(cur); cur.clear(); continue;
            }
            cur += c;
        }
        out.push_back(cur);
        return out;
    }


    static std::vector<std::string> split_semicolons(const std::string& in) {
        std::vector<std::string> out;
        std::string cur; char quote = 0; int paren = 0;
        for (size_t i = 0; i < in.size(); ++i) {
            char c = in[i];
            if (quote) {
                if (Helpers::escaped_pair(in, i, quote)) {
                    cur += c; cur += in[i + 1]; ++i; continue; // escaped, keep verbatim
                }
                cur += c; if (c == quote) quote = 0; continue;
            }
            if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
            if (c == '(') { paren++; cur += c; continue; }
            if (c == ')') { if (paren > 0) paren--; cur += c; continue; }
            if (c == ';' && paren == 0) { out.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        out.push_back(cur);
        return out;
    }

    // Turn raw file/input lines into normalized logical lines: split on ';',
    // drop blanks and '#' comments, and separate an inline command that follows
    // a leading 'then'/'else' keyword (so every keyword sits on its own line).
    static std::vector<std::string> flatten_script(const std::vector<std::string>& raw) {
        std::vector<std::string> out;
        for (const auto& rl : raw) {
            for (auto seg : split_semicolons(rl)) {
                Helpers::trim(seg);
                if (seg.empty() || seg[0] == '#') continue;
                ScriptKw k = script_kw(seg);
                if (k == ScriptKw::THEN || k == ScriptKw::ELSE ||
                    k == ScriptKw::DO) {
                    out.push_back(k == ScriptKw::THEN ? "then"
                                : k == ScriptKw::ELSE ? "else" : "do");
                    std::string rest = after_keyword(seg);
                    if (!rest.empty()) out.push_back(rest);
                } else {
                    out.push_back(seg);
                }
            }
        }
        return out;
    }

    // From `from`, find the next elif/else/fi that belongs to the current if
    // (depth 0), skipping nested if..fi and while/for..done. Returns (size_t)-1
    // if none found or a mismatched closer (done) appears at depth 0.
    static size_t find_if_delim(const std::vector<std::string>& L,
                                size_t from, size_t hi) {
        int depth = 0;
        for (size_t i = from; i < hi; ++i) {
            ScriptKw k = script_kw(L[i]);
            if (kw_is_opener(k)) depth++;
            else if (kw_is_closer(k)) {
                if (depth == 0) return (k == ScriptKw::FI) ? i : (size_t)-1;
                depth--;
            } else if (depth == 0 && (k == ScriptKw::ELSE || k == ScriptKw::ELIF)) {
                return i;
            }
        }
        return (size_t)-1;
    }

    // Find the 'do' that opens a while/for body (depth 0). Returns (size_t)-1 if
    // a closer is hit first or none is found.
    static size_t find_do(const std::vector<std::string>& L,
                          size_t from, size_t hi) {
        int depth = 0;
        for (size_t i = from; i < hi; ++i) {
            ScriptKw k = script_kw(L[i]);
            if (kw_is_opener(k)) depth++;
            else if (kw_is_closer(k)) {
                if (depth == 0) return (size_t)-1; // closer before do
                depth--;
            } else if (k == ScriptKw::DO && depth == 0) {
                return i;
            }
        }
        return (size_t)-1;
    }

    // Find the matching 'done' for a loop body starting at `from` (just after do),
    // skipping nested constructs. Returns (size_t)-1 if none; the caller checks
    // the returned line really is DONE (not a mismatched FI).
    static size_t find_done(const std::vector<std::string>& L,
                            size_t from, size_t hi) {
        int depth = 0;
        for (size_t i = from; i < hi; ++i) {
            ScriptKw k = script_kw(L[i]);
            if (kw_is_opener(k)) depth++;
            else if (kw_is_closer(k)) {
                if (depth == 0) return i;
                depth--;
            }
        }
        return (size_t)-1;
    }

    // Execute a single (already flattened) line as a command.
    void run_one(const std::string& ln, bool verbose) {
        if (verbose) print(prompt + ln + "\n");
        command = ln;
        execute_command();
    }

    // Run condition lines; the condition is true iff the LAST one exits 0.
    bool eval_condition(const std::vector<std::string>& condLines, bool verbose) {
        if (condLines.empty()) { Helpers::cmd_status = 2; return false; }
        for (const auto& ln : condLines) run_one(ln, verbose);
        return Helpers::cmd_status == 0;
    }

    // Execute one if-construct starting at L[ifIdx] (keyword IF). Returns the
    // index just past the matching 'fi'. Sets $?=2 on a syntax error.
    size_t run_if(const std::vector<std::string>& L, size_t ifIdx,
                  size_t hi, bool verbose) {
        size_t cur = ifIdx;     // points at IF (first pass) or ELIF (later passes)
        bool taken = false;     // has some branch already run?
        while (true) {
            // Collect the condition: inline part of if/elif + lines until 'then'.
            std::vector<std::string> condLines;
            std::string inl = after_keyword(L[cur]);
            if (!inl.empty()) condLines.push_back(inl);
            size_t i = cur + 1;
            while (i < hi && script_kw(L[i]) != ScriptKw::THEN) {
                ScriptKw k = script_kw(L[i]);
                if (k == ScriptKw::FI || k == ScriptKw::ELSE || k == ScriptKw::ELIF) {
                    print("sh: syntax error: expected 'then'\n");
                    Helpers::cmd_status = 2; return hi;
                }
                condLines.push_back(L[i]); i++;
            }
            if (i >= hi) {
                print("sh: syntax error: 'if' without 'then'\n");
                Helpers::cmd_status = 2; return hi;
            }
            size_t thenIdx = i;
            size_t delim = find_if_delim(L, thenIdx + 1, hi);
            if (delim == (size_t)-1) {
                print("sh: syntax error: missing 'fi'\n");
                Helpers::cmd_status = 2; return hi;
            }

            if (!taken && eval_condition(condLines, verbose)) {
                run_block(L, thenIdx + 1, delim, verbose);
                taken = true;
            }

            ScriptKw dk = script_kw(L[delim]);
            if (dk == ScriptKw::FI) return delim + 1;
            if (dk == ScriptKw::ELIF) { cur = delim; continue; }
            // ELSE: body runs to the matching 'fi'.
            size_t fiIdx = find_if_delim(L, delim + 1, hi);
            if (fiIdx == (size_t)-1 || script_kw(L[fiIdx]) != ScriptKw::FI) {
                print("sh: syntax error: missing 'fi'\n");
                Helpers::cmd_status = 2; return hi;
            }
            if (!taken) run_block(L, delim + 1, fiIdx, verbose);
            return fiIdx + 1;
        }
    }

    // Poll the keyboard for Ctrl+C so a running loop can be aborted (there is no
    // other way to interrupt a script). Returns true when the break is pressed.
    bool loop_break_pressed() {
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

    // while COND ; do BODY ; done   (until = run while COND is FALSE)
    // Returns the index just past 'done'. Sets $?=2 on a syntax error.
    size_t run_while(const std::vector<std::string>& L, size_t idx,
                     size_t hi, bool verbose, bool until) {
        size_t doIdx = find_do(L, idx + 1, hi);
        if (doIdx == (size_t)-1) {
            print("sh: syntax error: expected 'do'\n");
            Helpers::cmd_status = 2; return hi;
        }
        size_t doneIdx = find_done(L, doIdx + 1, hi);
        if (doneIdx == (size_t)-1 || script_kw(L[doneIdx]) != ScriptKw::DONE) {
            print("sh: syntax error: missing 'done'\n");
            Helpers::cmd_status = 2; return hi;
        }
        // Condition lines: inline part of the while line + lines up to 'do'.
        std::vector<std::string> condLines;
        std::string inl = after_keyword(L[idx]);
        if (!inl.empty()) condLines.push_back(inl);
        for (size_t k = idx + 1; k < doIdx; ++k) condLines.push_back(L[k]);

        bool aborted = false;
        while (true) {
            if (loop_break_pressed()) { aborted = true; break; }
            bool c = eval_condition(condLines, verbose);
            bool go = until ? !c : c;
            if (!go) break;
            run_block(L, doIdx + 1, doneIdx, verbose);
        }
        if (aborted) { print("^C\n"); Helpers::cmd_status = 130; scriptInterrupted = true; }
        return doneIdx + 1;
    }

    // for VAR in LIST ; do BODY ; done
    // Returns the index just past 'done'. Sets $?=2 on a syntax error.
    size_t run_for(const std::vector<std::string>& L, size_t idx,
                   size_t hi, bool verbose) {
        // Header after 'for': "VAR in LIST..."
        std::string hdr = after_keyword(L[idx]);
        auto htoks = Helpers::tokenize(hdr);
        if (htoks.size() < 2 || htoks[1] != "in") {
            print("sh: syntax error: for VAR in LIST\n");
            Helpers::cmd_status = 2; return hi;
        }
        std::string var = htoks[0];
        // Reconstruct the raw list text after the word 'in' (keep quoting/globs).
        size_t inPos = hdr.find(" in");
        std::string listRaw = (inPos == std::string::npos)
                                ? "" : hdr.substr(inPos + 3);
        Helpers::trim(listRaw);

        size_t doIdx = find_do(L, idx + 1, hi);
        if (doIdx == (size_t)-1) {
            print("sh: syntax error: expected 'do'\n");
            Helpers::cmd_status = 2; return hi;
        }
        size_t doneIdx = find_done(L, doIdx + 1, hi);
        if (doneIdx == (size_t)-1 || script_kw(L[doneIdx]) != ScriptKw::DONE) {
            print("sh: syntax error: missing 'done'\n");
            Helpers::cmd_status = 2; return hi;
        }

        // Expand variables then globs, then split into words. The "_ " prefix
        // gives expand_globs a dummy command at index 0 (it never globs token 0).
        std::string le = expand_vars(listRaw, vars, scriptArgs, Helpers::cmd_status,
                                     [this](const std::string& c) { return run_capture(c); });
        Helpers::cmd_status = 0;
        le = CommonCmds::expand_braces(le);   // {1..5} / {0..10..2}
        std::string lg = CommonCmds::expand_globs("_ " + le);
        if (lg.rfind("_ ", 0) == 0) lg = lg.substr(2);
        else if (lg == "_") lg = "";
        std::vector<std::string> words = Helpers::tokenize(lg);

        bool aborted = false;
        for (const auto& w : words) {
            if (loop_break_pressed()) { aborted = true; break; }
            vars[var] = w;                 // set loop variable
            run_block(L, doIdx + 1, doneIdx, verbose);
        }
        if (aborted) { print("^C\n"); Helpers::cmd_status = 130; scriptInterrupted = true; }
        return doneIdx + 1;
    }

    // Interpret a block of flattened lines L[lo..hi): plain lines run as commands,
    // if..fi constructs are handled by run_if. A stray then/elif/else/fi is a
    // syntax error.
    void run_block(const std::vector<std::string>& L, size_t lo, size_t hi,
                   bool verbose) {
        size_t pc = lo;
        while (pc < hi) {
            if (scriptInterrupted) return; // Ctrl+C aborts the rest of the script
            ScriptKw kw = script_kw(L[pc]);
            if (kw == ScriptKw::IF) {
                pc = run_if(L, pc, hi, verbose);
            } else if (kw == ScriptKw::WHILE || kw == ScriptKw::UNTIL) {
                pc = run_while(L, pc, hi, verbose, kw == ScriptKw::UNTIL);
            } else if (kw == ScriptKw::FOR) {
                pc = run_for(L, pc, hi, verbose);
            } else if (kw == ScriptKw::THEN || kw == ScriptKw::ELSE ||
                       kw == ScriptKw::ELIF || kw == ScriptKw::FI ||
                       kw == ScriptKw::DO   || kw == ScriptKw::DONE) {
                print(std::string("sh: syntax error near '") + L[pc] + "'\n");
                Helpers::cmd_status = 2;
                return;
            } else {
                run_one(L[pc], verbose);
                pc++;
            }
        }
    }

    void run_profile() {
        run_script("/.profile", false, {"/.profile"}); // boot, silent, $0=/.profile
    }

    // Run a script file: execute each line as a command, skipping blank lines
    // and '#' comments. When verbose, each command is echoed before it runs
    // (like `sh -v`); otherwise only the commands' own output is shown. `args`
    // are the positional parameters ([0]=$0 script name, [1..]=$1..$N), exposed
    // to the script via $1, $@, $#, etc. Returns false if the file can't be
    // opened. A depth guard stops a script that calls `sh` on itself from
    // overflowing the stack; positional params are saved/restored so a nested
    // script doesn't clobber its caller's arguments.
    bool run_script(const std::string& path, bool verbose,
                    const std::vector<std::string>& args) {
        if (scriptDepth >= 8) {
            print("sh: scripts nested too deep\n");
            return true; // treat as handled, just refuse to go deeper
        }
        File f = Helpers::fsOpen(path.c_str(), "r");
        if (!f) return false;
        if (f.isDirectory()) { f.close(); return false; }

        std::vector<std::string> savedArgs = scriptArgs; // save caller's params
        scriptArgs = args;
        if (scriptDepth == 0) scriptInterrupted = false; // fresh start for a top-level script
        scriptDepth++;

        // Read the whole file into raw lines, then interpret as a block so that
        // multi-line constructs (if..fi, later while/for) can look ahead.
        std::vector<std::string> rawLines;
        std::string line;
        while (f.available()) {
            int c = f.read();
            if (c < 0) break;
            if (c == '\n') { rawLines.push_back(line); line.clear(); }
            else if (c != '\r' && line.size() < 512) { line += (char)c; }
        }
        if (!line.empty()) rawLines.push_back(line);
        f.close();

        auto lines = flatten_script(rawLines);
        run_block(lines, 0, lines.size(), verbose);

        scriptDepth--;
        scriptArgs = savedArgs; // restore caller's params
        return true;
    }

    void add_to_history(const std::string& text) {
        const size_t maxWidth = 34;
        std::string line;
        size_t charCount = 0;

        for (size_t i = 0; i < text.size(); ++i) {
            unsigned char c = text[i];

            // Break on \n
            if (c == '\n') {
                history.push_back(line);
                line.clear();
                charCount = 0;
                continue;
            }

            // New visible UTF-8 character (lead byte)
            bool isLead = ((c & 0xC0) != 0x80);

            // FIX: wrap by width BEFORE appending the new character,
            // and only on a character boundary. The old code lost the count
            // of the current character and produced an extra char on the wrapped line.
            if (isLead && charCount >= maxWidth) {
                history.push_back(line);
                line.clear();
                charCount = 0;
            }

            line += c;
            if (isLead) charCount++;
        }

        if (!line.empty()) {
            history.push_back(line);
        }

        // History cap
        while ((int)history.size() > max_history) {
            history.erase(history.begin());
        }
    }

    void refresh_screen() {
        M5Cardputer.Display.fillScreen(BLACK);
        M5Cardputer.Display.setCursor(0, 0);

        int total_lines = history.size();
        int start_index = std::max(0, total_lines - lines_on_screen - scroll_offset);
        int end_index = (scroll_offset > 0) ? total_lines - scroll_offset : total_lines;

        for (int i = start_index; i < end_index; i++) {
            M5Cardputer.Display.println(history[i].c_str());
        }

        if (scroll_offset == 0 && !liveCommand) {
            M5Cardputer.Display.print(prompt.c_str());
            M5Cardputer.Display.print(command.c_str());
            // Derive the input's first row by counting back from the END cursor
            // (same method as redraw_input_inplace). Capturing it BEFORE printing
            // is wrong when the prompt+command printing scrolls the screen.
            const int lineH = 14;
            int cpr = M5Cardputer.Display.width() / 7; if (cpr < 1) cpr = 1;
            int newLen = (int)(prompt.length() + command.length());
            int endY = M5Cardputer.Display.getCursorY();
            int endRows = (newLen >= 1) ? (newLen - 1) / cpr : 0; // see redraw_input_inplace
            inputStartY = endY - endRows * lineH;
            if (inputStartY < 0) inputStartY = 0;
            draw_caret();
        }
    }

    void scroll_up() {
        if (scroll_offset < (int)history.size() - lines_on_screen) {
            scroll_offset++;
            refresh_screen();
        }
    }

    void scroll_down() {
        if (scroll_offset > 0) {
            scroll_offset--;
            refresh_screen();
        }
    }

    // Returns the start position of the last token (after the last space)
    size_t last_token_start() const {
        size_t sp = command.find_last_of(' ');
        return (sp == std::string::npos) ? 0 : sp + 1;
    }

    void handle_tab() {
        // FIX: tab-completion fully rewritten.
        //  - respects the already-typed prefix of the last token;
        //  - inserts only matching candidates;
        //  - cycles through matches correctly.
        if (Helpers::lastOutput.empty()) return;

        if (scroll_offset > 0) {
            scroll_offset = 0;
            refresh_screen();
        }

        // New completion cycle: rebuild the list of matches
        if (commandNumber == -1) {
            size_t start = last_token_start();
            tabPrefix = command.substr(start);
            tabMatches.clear();

            for (const auto& cand : Helpers::lastOutput) {
                // In different esp32-arduino versions File::name() returns either
                // the basename or a full slashed path - normalize it.
                std::string base = cand;
                size_t slash = base.find_last_of('/');
                if (slash != std::string::npos) base = base.substr(slash + 1);

                if (base.size() >= tabPrefix.size() &&
                    base.compare(0, tabPrefix.size(), tabPrefix) == 0) {
                    tabMatches.push_back(base);
                }
            }
            if (tabMatches.empty()) return; // nothing to insert
            commandNumber = 0;
        } else {
            // next candidate, wrapping around
            commandNumber = (commandNumber + 1) % (int)tabMatches.size();
        }

        // Length of the input currently on screen (with the previous candidate),
        // captured BEFORE we modify it - needed to clear exactly those rows.
        int oldLen = (int)(prompt.length() + command.length());

        // Erase what was inserted last time (previousValue),
        // keeping the user-typed prefix.
        if (!previousValue.empty() &&
            command.size() >= previousValue.size()) {
            command.erase(command.size() - previousValue.size());
        }

        const std::string& match = tabMatches[commandNumber];
        // Append only the tail of the match after the prefix
        std::string suffix = match.substr(tabPrefix.size());
        previousValue = suffix;

        // Repaint only the input rows in place (no full-screen clear) so that
        // cycling candidates with Tab doesn't flicker. Wrap-aware.
        command += suffix;
        cursorPos = command.length();
        render_input(oldLen);
    }

    // Redraw "prompt + command" in place, clearing exactly the display rows the
    // input occupies - no full-screen fill, so it doesn't flicker. oldLen is the
    // prompt+command length that was on screen before the change (so we clear
    // the old rows too when the new input is shorter). Assumes the cursor is at
    // the end of the currently displayed input.
    void redraw_input_inplace(int oldLen) {
        const int lineH = 14;
        int cpr = M5Cardputer.Display.width() / 7; // chars per row (~34)
        if (cpr < 1) cpr = 1;

        int endY   = M5Cardputer.Display.getCursorY();
        // Row offset of the END cursor. The display does not advance to the next
        // row until the next char is printed, so a line of exactly N*cpr chars
        // leaves the cursor on row (len-1)/cpr, not len/cpr. Using len/cpr here
        // put startY one row too high on exactly-full lines and left an artifact.
        int endRows = (oldLen >= 1) ? (oldLen - 1) / cpr : 0;
        int startY = endY - endRows * lineH; // first row of the input
        if (startY < 0) startY = 0;

        int newLen  = (int)(prompt.length() + command.length());
        int oldRows = (oldLen + cpr - 1) / cpr; if (oldRows < 1) oldRows = 1;
        int newRows = (newLen + cpr - 1) / cpr; if (newRows < 1) newRows = 1;

        // If the new input would run past the bottom of the screen it scrolls,
        // and in-place geometry no longer holds - fall back to a full repaint.
        if (startY + newRows * lineH > M5Cardputer.Display.height()) {
            refresh_screen();
            return;
        }

        int rows = (oldRows > newRows) ? oldRows : newRows;

        M5Cardputer.Display.fillRect(0, startY, M5Cardputer.Display.width(), rows * lineH, BLACK);
        M5Cardputer.Display.setCursor(0, startY);
        M5Cardputer.Display.print(prompt.c_str());
        M5Cardputer.Display.print(command.c_str());
        inputStartY = startY;
    }

    // Draw the edit cursor (an underline) at cursorPos within the input line,
    // accounting for wrapping. Uses inputStartY as the input's first row.
    void draw_caret() {
        const int lineH = 14;
        int cpr = M5Cardputer.Display.width() / 7;
        if (cpr < 1) cpr = 1;
        int idx  = (int)(prompt.length() + cursorPos); // always >= 1 (prompt)
        // Match the display's no-wrap-until-next-char behaviour so the caret
        // never lands on a row beyond the drawn text (which would not be cleared
        // on the next repaint and would leave a stray underline).
        int crow = (idx - 1) / cpr;
        int ccol = (idx - 1) % cpr + 1;
        int cx = ccol * 7;
        int cy = inputStartY + crow * lineH;
        M5Cardputer.Display.fillRect(cx, cy + lineH - 2, 7, 2, WHITE);
    }

    // Repaint the input line in place and draw the caret. oldLen is the
    // prompt+command length previously on screen (so old rows get cleared).
    void render_input(int oldLen) {
        redraw_input_inplace(oldLen); // sets inputStartY (or refresh_screen does)
        draw_caret();
    }

    // Print a fresh prompt for input and show the edit caret right after it.
    // Used at every point that returns the user to the command line. While a
    // script is running (scriptDepth>0) this is a no-op: the per-line commands
    // must not draw prompts; only the outer `sh ...` command prints one when the
    // script finishes (by which point scriptDepth is back to 0).
    void print_prompt() {
        if (scriptDepth > 0 || chainDepth > 0) return;
        // If the previous output stopped mid-row, close that row first: the
        // scrollback stores it as a complete line and refresh_screen() draws
        // every entry with println(), so the prompt must start on a fresh row.
        //
        // Ask the display where the cursor really is instead of tracking it in a
        // flag: plenty of code draws straight to the display (the Enter echo,
        // read_line, the editor, telnet), so a shadow flag drifts out of sync
        // and then adds a newline when the row is already empty - which showed
        // up as a random blank line after silent commands like mkdir/touch.
        if (scroll_offset == 0 && M5Cardputer.Display.getCursorX() != 0) {
            M5Cardputer.Display.println();
        }
        M5Cardputer.Display.print(prompt.c_str());
        inputStartY = M5Cardputer.Display.getCursorY();
        draw_caret();
    }

    // Redraw the input line (full screen repaint - reliable for
    // any command length).
    void redraw_input() {
        if (scroll_offset > 0) scroll_offset = 0;
        refresh_screen();
    }

    // Up arrow - previous (older) command.
    void history_prev() {
        if (cmdHistory.empty()) return;
        if (historyIndex == (int)cmdHistory.size()) {
            savedLine = command; // save the unfinished input
        }
        if (historyIndex > 0) {
            historyIndex--;
            command = cmdHistory[historyIndex];
            cursorPos = command.length();
            redraw_input();
        }
    }

    // Down arrow - next (newer) command; past the end restore the input.
    void history_next() {
        if (historyIndex >= (int)cmdHistory.size()) return; // already on the live line
        historyIndex++;
        if (historyIndex >= (int)cmdHistory.size()) {
            historyIndex = (int)cmdHistory.size();
            command = savedLine;
        } else {
            command = cmdHistory[historyIndex];
        }
        cursorPos = command.length();
        redraw_input();
    }

    // --- Editor ---
    bool editor_active() const { return editorActive; }

    void start_editor(const std::string& path) {
        editor = new Editor(path);
        if (!editor->ok()) {
            print(editor->error() + std::string("\n"));
            Helpers::cmd_status = 1;
            delete editor;
            editor = nullptr;
            return;
        }
        editorActive = true;
        editor->render();
    }

    void editor_key(int key) {
        if (!editorActive || !editor) return;
        bool wantExit = editor->handleKey(key);
        if (wantExit) {
            delete editor;
            editor = nullptr;
            editorActive = false;
            // Restore the terminal screen
            M5Cardputer.Display.setFont(&fonts::efontCN_14);
            M5Cardputer.Display.setTextColor(WHITE);
            M5Cardputer.Display.setTextSize(1);
            scroll_offset = 0;
            refresh_screen();
        }
    }

    // --- Telnet ---
    bool telnet_active() const { return telnetActive; }

    void start_telnet(const std::string& host, uint16_t port, bool raw) {
        if (WiFi.status() != WL_CONNECTED) {
            print("Not connected to WiFi\n");
            Helpers::cmd_status = 1;
            return;
        }
        print("Connecting to " + host + ":" + std::to_string(port) + "...\n");
        telnet = new TelnetSession(raw); // raw = nc (no telnet negotiation)
        if (!telnet->begin(host, port)) {
            print("telnet: connection failed\n");
            Helpers::cmd_status = 1;
            delete telnet;
            telnet = nullptr;
            return;
        }
        telnetActive = true;
        telnet->onConnected(host, port);
    }

    void telnet_teardown() {
        if (telnet) { telnet->close(); delete telnet; telnet = nullptr; }
        telnetActive = false;
        M5Cardputer.Display.setFont(&fonts::efontCN_14);
        M5Cardputer.Display.setTextColor(WHITE);
        M5Cardputer.Display.setTextSize(1);
        add_to_history("[telnet disconnected]");
        scroll_offset = 0;
        refresh_screen();
    }

    void telnet_poll() {
        if (!telnetActive || !telnet) return;
        if (!telnet->poll()) {       // connection closed
            telnet_teardown();
        }
    }

    void telnet_on_char(uint8_t c)   { if (telnetActive && telnet) telnet->onChar(c); }
    void telnet_on_enter()           { if (telnetActive && telnet) telnet->onEnter(); }
    void telnet_on_backspace()       { if (telnetActive && telnet) telnet->onBackspace(); }
    void telnet_on_ctrl(uint8_t b)   { if (telnetActive && telnet) telnet->onCtrl(b); }
    void telnet_arrow(const char* s) { if (telnetActive && telnet) telnet->sendArrow(s); }
    void telnet_scroll_up()   { if (telnetActive && telnet) telnet->scrollUp(); }
    void telnet_scroll_down() { if (telnetActive && telnet) telnet->scrollDown(); }
    void telnet_disconnect() {
        if (telnetActive) telnet_teardown();
    }

    // --- SSH ---
    bool ssh_active() const { return sshActive; }
    bool ssh_waiting_pass() const { return sshWaitingForPass; }

    // Parse "ssh [user@]host [port]" (or "ssh host -l user [port]") and, if a
    // user/host are known, prompt for the password. Returns having printed the
    // prompt or an error.
    void start_ssh(const std::string& argstr) {
        auto toks = split_ws(argstr);
        std::string user, host;
        uint16_t port = 22;
        std::vector<std::string> positional;
        for (size_t i = 0; i < toks.size(); ++i) {
            if (toks[i] == "-l" && i + 1 < toks.size()) { user = toks[++i]; }
            else if (toks[i] == "-p" && i + 1 < toks.size()) {
                long v; if (parse_uint(toks[++i], v) && v >= 1 && v <= 65535) port = (uint16_t)v;
            } else positional.push_back(toks[i]);
        }
        if (positional.empty()) { print("Usage: ssh [user@]host [port]\n"); Helpers::cmd_status = 1; return; }

        std::string target = positional[0];
        size_t at = target.find('@');
        if (at != std::string::npos) { user = target.substr(0, at); host = target.substr(at + 1); }
        else host = target;
        // optional bare port as second positional
        if (positional.size() >= 2) { long v; if (parse_uint(positional[1], v) && v >= 1 && v <= 65535) port = (uint16_t)v; }

        if (host.empty()) { print("ssh: no host\n"); Helpers::cmd_status = 1; return; }
        if (user.empty()) { print("ssh: no user (use user@host or -l user)\n"); Helpers::cmd_status = 1; return; }
        if (WiFi.status() != WL_CONNECTED) { print("Not connected to WiFi\n"); Helpers::cmd_status = 1; return; }

        sshUser = user; sshHost = host; sshPort = port;
        sshWaitingForPass = true;
        print("Password: ");
    }

    // Called with the typed password to perform the (blocking) connection.
    void ssh_connect_with_pass(const std::string& password) {
        sshWaitingForPass = false;
        print("\nConnecting to " + sshHost + "...\n");
        ssh = new SshSession();
        if (!ssh->begin(sshHost, sshUser, password, sshPort)) {
            print(ssh->error() + std::string("\n"));
            Helpers::cmd_status = 1;
            delete ssh; ssh = nullptr;
            print_prompt();
            return;
        }
        sshActive = true;
        ssh->onConnected(sshUser, sshHost);
    }

    void ssh_teardown() {
        if (ssh) { ssh->close(); delete ssh; ssh = nullptr; }
        sshActive = false;
        M5Cardputer.Display.setFont(&fonts::efontCN_14);
        M5Cardputer.Display.setTextColor(WHITE);
        M5Cardputer.Display.setTextSize(1);
        add_to_history("[ssh disconnected]");
        scroll_offset = 0;
        refresh_screen();
    }

    void ssh_poll() {
        if (!sshActive || !ssh) return;
        if (!ssh->poll()) ssh_teardown();
    }
    void ssh_on_char(uint8_t c)   { if (sshActive && ssh) ssh->onChar(c); }
    void ssh_on_enter()           { if (sshActive && ssh) ssh->onEnter(); }
    void ssh_on_backspace()       { if (sshActive && ssh) ssh->onBackspace(); }
    void ssh_on_ctrl(uint8_t b)   { if (sshActive && ssh) ssh->onCtrl(b); }
    void ssh_arrow(const char* s) { if (sshActive && ssh) ssh->sendStr(s); }
    void ssh_scroll_up()          { if (sshActive && ssh) ssh->scrollUp(); }
    void ssh_scroll_down()        { if (sshActive && ssh) ssh->scrollDown(); }

    // Erase the last echoed character from the display (handles a wrap back to
    // the previous row). We clear the cell with a filled rectangle rather than
    // printing a space: a space glyph paints no pixels, so it wouldn't actually
    // rub out the character underneath.
    void read_erase_char() {
        const int cw = 7, lineH = 14;
        int cx = M5Cardputer.Display.getCursorX();
        int cy = M5Cardputer.Display.getCursorY();
        int x, y;
        if (cx >= cw) {
            x = cx - cw; y = cy;
        } else if (cy >= lineH) {
            int cpr = M5Cardputer.Display.width() / cw; if (cpr < 1) cpr = 1;
            x = (cpr - 1) * cw; y = cy - lineH;
        } else {
            return; // nothing before the cursor
        }
        M5Cardputer.Display.fillRect(x, y, cw, lineH, BLACK);
        M5Cardputer.Display.setCursor(x, y);
    }

    // Read one line from the keyboard (for the `read` command). Echoes input,
    // supports Backspace. Returns false if cancelled with Ctrl+C. The prompt is
    // drawn live; the whole line is added to the scrollback when Enter is hit.
    bool read_line(const std::string& promptText, std::string& out) {
        // Let the key that launched us (usually Enter) release first.
        while (M5Cardputer.Keyboard.isPressed()) { M5Cardputer.update(); delay(10); }

        if (!promptText.empty()) M5Cardputer.Display.print(promptText.c_str());

        std::string input;
        bool cancelled = false;
        while (true) {
            M5Cardputer.update();
            auto& kb = M5Cardputer.Keyboard;
            if (kb.isChange() && kb.isPressed()) {
                Keyboard_Class::KeysState st = kb.keysState();
                if (st.ctrl) {
                    bool ctrlC = false;
                    for (auto c : st.word) {
                        char lc = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
                        if (lc == 'c') ctrlC = true;
                    }
                    if (ctrlC) { cancelled = true; break; }
                }
                if (st.enter) break;
                if (st.del) {
                    if (!input.empty()) { input.pop_back(); read_erase_char(); }
                } else if (!st.word.empty()) {
                    for (auto c : st.word) {
                        input += (char)c;
                        char one[2] = { (char)c, 0 };
                        M5Cardputer.Display.print(one);
                    }
                }
            }
            delay(10);
        }
        M5Cardputer.Display.println();
        add_to_history(promptText + input + "\n"); // record the whole line once
        out = input;
        return !cancelled;
    }











    // read [-p prompt] [name...]  - read a line into variables (last gets the
    // remainder). No names -> $REPLY. Ctrl+C cancels (aborts a script).
    void do_read(const std::string& argStr, LineCallback emit) {
        auto toks = split_ws(argStr);
        std::string promptText;
        std::vector<std::string> names;
        for (size_t i = 0; i < toks.size(); ++i) {
            if (toks[i] == "-p" && i + 1 < toks.size()) { promptText = toks[i + 1]; ++i; }
            else if (!toks[i].empty() && toks[i][0] == '-') { /* unknown flag: ignore */ }
            else names.push_back(toks[i]);
        }
        if (names.empty()) names.push_back("REPLY");
        for (const auto& n : names) {
            if (!valid_var_name(n)) {
                emit("read: invalid name: " + n + "\n");
                Helpers::cmd_status = 1; return;
            }
        }

        std::string input;
        if (!read_line(promptText, input)) { // Ctrl+C
            Helpers::cmd_status = 1;
            scriptInterrupted = true;
            return;
        }

        // Split on whitespace (no quote processing - input is literal) and assign;
        // the last name receives the remaining words joined by single spaces.
        std::vector<std::string> words;
        { std::string cur;
          for (char c : input) {
              if (c == ' ' || c == '\t') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
              else cur += c;
          }
          if (!cur.empty()) words.push_back(cur);
        }
        for (size_t i = 0; i < names.size(); ++i) {
            if (i + 1 == names.size()) {
                std::string rest;
                for (size_t j = i; j < words.size(); ++j) { if (j > i) rest += " "; rest += words[j]; }
                vars[names[i]] = rest;
            } else {
                vars[names[i]] = (i < words.size()) ? words[i] : "";
            }
        }
        save_vars();
        Helpers::cmd_status = 0;
    }

    // Input poll used by long-running "air" commands: Ctrl+;/. scroll the
    // scrollback, Ctrl+C requests a stop. Returns true when Ctrl+C is pressed.
    bool air_poll() {
        M5Cardputer.update();
        auto& kb = M5Cardputer.Keyboard;
        if (kb.isChange() && kb.isPressed()) {
            Keyboard_Class::KeysState st = kb.keysState();
            if (st.ctrl) {
                if (kb.isKeyPressed(':')) { scroll_up();   return false; } // Ctrl+; (up)
                if (kb.isKeyPressed('>')) { scroll_down(); return false; } // Ctrl+. (down)
                for (auto c : st.word) {
                    char lc = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
                    if (lc == 'c') {
                        if (scroll_offset != 0) { scroll_offset = 0; refresh_screen(); }
                        return true;
                    }
                }
            }
        }
        return false;
    }
    void ssh_disconnect()         { if (sshActive) ssh_teardown(); }

    void handle_keypress(uint8_t key) {
        // Any normal key resets the tab-completion state
        commandNumber = -1;
        previousValue = "";
        tabPrefix = "";
        tabMatches.clear();

        // Cardputer key codes
        if (key == 181) {            // scroll output up (Ctrl+;)
            scroll_up();
            return;
        }
        if (key == 182) {            // scroll output down (Ctrl+.)
            scroll_down();
            return;
        }
        if (key == 183) {            // history back (Fn+; - 'up')
            history_prev();
            return;
        }
        if (key == 184) {            // history forward (Fn+. - 'down')
            history_next();
            return;
        }

        if (scroll_offset > 0) {
            scroll_offset = 0;
            refresh_screen();
        }

        if (key == 185) {            // cursor left (Fn+, - 'left')
            if (cursorPos > 0) {
                cursorPos--;
                render_input((int)(prompt.length() + command.length()));
            }
            return;
        }
        if (key == 186) {            // cursor right (Fn+/ - 'right')
            if (cursorPos < command.length()) {
                cursorPos++;
                render_input((int)(prompt.length() + command.length()));
            }
            return;
        }

        if (key == 8) { // Backspace - delete the char before the cursor
            if (cursorPos > 0) {
                int oldLen = (int)(prompt.length() + command.length());
                command.erase(cursorPos - 1, 1);
                cursorPos--;
                historyIndex = (int)cmdHistory.size(); // editing detaches from history
                render_input(oldLen);
            }
        }
        else if (key == 10 || key == 13) { // Enter
            // Repaint the input line without the caret so the underline doesn't
            // linger on the entered line, then run the command.
            redraw_input_inplace((int)(prompt.length() + command.length()));
            M5Cardputer.Display.println();
            execute_command();
        }
        else if (key >= 32 && key <= 126) { // insert at the cursor
            int oldLen = (int)(prompt.length() + command.length());
            command.insert(cursorPos, 1, (char)key);
            cursorPos++;
            historyIndex = (int)cmdHistory.size(); // editing detaches from history
            render_input(oldLen);
        }
    }

    void print(const std::string& text) {
        // Command substitution: collect output into a buffer (capped) instead
        // of drawing it, and don't touch the scrollback.
        if (capturing) {
            if (captureBuf.size() < CAPTURE_CAP)
                captureBuf.append(text, 0, CAPTURE_CAP - captureBuf.size());
            return;
        }
        add_to_history(text);
        // When the user has scrolled up (offset > 0), keep buffering into history
        // but don't paint over their scrolled view; scrolling back down redraws.
        if (scroll_offset == 0) M5Cardputer.Display.print(text.c_str());
    }

    // Generic parsing of output redirection (> and >>).
    // FIX: extracted from duplicated cat/echo/date code.
    // text  - the part before the redirection operator (for echo/cat args);
    // file  - target file (absolute path) or empty;
    // append - true for >>.
    void parse_redirect(const std::string& body, std::string& text,
                        std::string& file, bool& append) {
        text = "";
        file = "";
        append = false;

        // Find the redirection operator OUTSIDE quotes, so a '>' inside a quoted
        // filename is not treated as redirection.
        size_t pos = Helpers::find_unquoted(body, '>');
        if (pos != std::string::npos) {
            text = body.substr(0, pos);
            if (pos + 1 < body.size() && body[pos + 1] == '>') { append = true; file = body.substr(pos + 2); }
            else                                                { file = body.substr(pos + 1); }
        } else {
            text = body;
        }

        if (!file.empty()) {
            // The target is the first (possibly quoted) token; strip the quotes.
            file = Helpers::strip_quotes(file);
            if (!file.empty()) file = Helpers::make_absolute(file);
        }
    }

    // --- User variables ---------------------------------------------------

    static bool valid_var_name(const std::string& s) {
        if (s.empty()) return false;
        char c0 = s[0];
        if (!(c0 == '_' || (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')))
            return false;
        for (char c : s) {
            bool ok = (c == '_' || (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
            if (!ok) return false;
        }
        return true;
    }

    // ---- Integer arithmetic for $(( ))  (+ - * / %, parens, unary, vars) ----
    static void arith_skipws(const std::string& s, size_t& p) {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    }
    // Read a bare identifier's integer value from vars (undefined / non-numeric
    // -> 0), parsing a leading optional sign and digits.
    static long arith_var(const std::map<std::string, std::string>& vars,
                          const std::string& name) {
        auto it = vars.find(name);
        if (it == vars.end()) return 0;
        const std::string& v = it->second;
        size_t q = 0; while (q < v.size() && (v[q] == ' ' || v[q] == '\t')) q++;
        bool neg = false;
        if (q < v.size() && (v[q] == '-' || v[q] == '+')) { neg = (v[q] == '-'); q++; }
        long r = 0; bool any = false;
        while (q < v.size() && v[q] >= '0' && v[q] <= '9') { r = r * 10 + (v[q] - '0'); q++; any = true; }
        if (!any) return 0;
        return neg ? -r : r;
    }
    static long arith_factor(const std::string& s, size_t& p,
                             const std::map<std::string, std::string>& vars, bool& err) {
        arith_skipws(s, p);
        if (p >= s.size()) { err = true; return 0; }
        char c = s[p];
        if (c == '+') { p++; return arith_factor(s, p, vars, err); }
        if (c == '-') { p++; return -arith_factor(s, p, vars, err); }
        if (c == '(') {
            p++;
            long v = arith_expr(s, p, vars, err);
            arith_skipws(s, p);
            if (p < s.size() && s[p] == ')') p++; else err = true;
            return v;
        }
        if (c >= '0' && c <= '9') {
            long v = 0;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') { v = v * 10 + (s[p] - '0'); p++; }
            return v;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
            size_t st = p;
            while (p < s.size() && ((s[p] >= 'A' && s[p] <= 'Z') ||
                   (s[p] >= 'a' && s[p] <= 'z') || (s[p] >= '0' && s[p] <= '9') ||
                   s[p] == '_')) p++;
            return arith_var(vars, s.substr(st, p - st));
        }
        err = true; return 0;
    }
    static long arith_term(const std::string& s, size_t& p,
                           const std::map<std::string, std::string>& vars, bool& err) {
        long v = arith_factor(s, p, vars, err);
        for (;;) {
            arith_skipws(s, p);
            if (p < s.size() && (s[p] == '*' || s[p] == '/' || s[p] == '%')) {
                char op = s[p++];
                long r = arith_factor(s, p, vars, err);
                if ((op == '/' || op == '%') && r == 0) { err = true; v = 0; }
                else if (op == '*') v *= r;
                else if (op == '/') v /= r;
                else v %= r;
            } else break;
        }
        return v;
    }
    static long arith_expr(const std::string& s, size_t& p,
                           const std::map<std::string, std::string>& vars, bool& err) {
        long v = arith_term(s, p, vars, err);
        for (;;) {
            arith_skipws(s, p);
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
                char op = s[p++];
                long r = arith_term(s, p, vars, err);
                v = (op == '+') ? v + r : v - r;
            } else break;
        }
        return v;
    }
    static long eval_arith(const std::string& s,
                           const std::map<std::string, std::string>& vars, bool& err) {
        size_t p = 0;
        long v = arith_expr(s, p, vars, err);
        arith_skipws(s, p);
        if (p != s.size()) err = true; // trailing junk
        return v;
    }

    // Run a command with its output captured into a string (for $(...) command
    // substitution). Re-entrant (supports nesting). Trailing newlines trimmed,
    // like a shell. Output is capped at CAPTURE_CAP bytes.
    std::string run_capture(const std::string& innerCmd, bool trimTrailing = true) {
        if (scriptDepth >= 8) return std::string(); // refuse pathologically deep $()
        bool savedCapturing = capturing;
        std::string savedBuf = captureBuf;
        std::string savedCommand = command;

        capturing = true;
        captureBuf.clear();
        command = innerCmd;
        scriptDepth++;              // silence echo/history/prompt of the inner command
        execute_command();
        scriptDepth--;

        std::string result = captureBuf;

        command = savedCommand;
        captureBuf = savedBuf;
        capturing = savedCapturing;

        if (Helpers::cmd_status == 130) scriptInterrupted = true; // Ctrl+C inside $()

        if (trimTrailing) {
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
        }
        return result;
    }

    // Expand $NAME and ${NAME} against the given variables. Undefined names
    // expand to an empty string (like a typical shell). "\$" yields a literal $.
    static std::string expand_vars(const std::string& in,
                                   const std::map<std::string, std::string>& vars,
                                   const std::vector<std::string>& args,
                                   int status,
                                   const CaptureFn& cap) {
        std::string out;
        out.reserve(in.size());
        size_t i = 0;
        while (i < in.size()) {
            char c = in[i];
            if (c == '\\' && i + 1 < in.size() && in[i + 1] == '$') {
                out += '$'; i += 2; continue; // escaped: \$ -> $
            }
            if (c == '$') {
                // $(( EXPR )) - arithmetic expansion (checked before ${...}).
                if (in.compare(i, 3, "$((") == 0) {
                    size_t k = i + 3; int depth = 0; bool closed = false;
                    while (k < in.size()) {
                        char d = in[k];
                        if (d == '(') depth++;
                        else if (d == ')') {
                            if (depth == 0) { closed = true; break; }
                            depth--;
                        }
                        k++;
                    }
                    if (closed && k + 1 < in.size() && in[k + 1] == ')') {
                        std::string expr = in.substr(i + 3, k - (i + 3));
                        std::string inner = expand_vars(expr, vars, args, status, cap);
                        bool aerr = false;
                        long v = eval_arith(inner, vars, aerr);
                        out += std::to_string(v); // malformed/div0 -> 0
                        i = k + 2; // skip the closing "))"
                        continue;
                    }
                    // not a well-formed $(( )) -> fall through to literal handling
                }
                // $( CMD ) - command substitution (not $(( )), handled above).
                if (i + 1 < in.size() && in[i + 1] == '(' &&
                    (i + 2 >= in.size() || in[i + 2] != '(')) {
                    size_t k = i + 2; int depth = 0; char q = 0; bool closed = false;
                    while (k < in.size()) {
                        char d = in[k];
                        if (q && Helpers::escaped_pair(in, k, q)) { k += 2; continue; }
                        if (q) { if (d == q) q = 0; }
                        else if (d == '"' || d == '\'') q = d;
                        else if (d == '(') depth++;
                        else if (d == ')') { if (depth == 0) { closed = true; break; } depth--; }
                        k++;
                    }
                    if (closed) {
                        std::string innerCmd = in.substr(i + 2, k - (i + 2));
                        if (cap) out += cap(innerCmd); // run + capture (trailing \n trimmed)
                        i = k + 1; // skip the ')'
                        continue;
                    }
                    // unclosed -> fall through to literal '$'
                }
                size_t j = i + 1;
                bool braced = false;
                if (j < in.size() && in[j] == '{') { braced = true; j++; }

                // $? - exit status of the last command.
                if (j < in.size() && in[j] == '?') {
                    size_t after = j + 1;
                    if (braced) {
                        if (after < in.size() && in[after] == '}') after++;
                        else { out += c; i++; continue; } // malformed ${?
                    }
                    out += std::to_string(status);
                    i = after; continue;
                }

                // Special params: $# (arg count), $@ / $* (all args, space-joined).
                if (j < in.size() && (in[j] == '#' || in[j] == '@' || in[j] == '*')) {
                    char sp = in[j];
                    size_t after = j + 1;
                    if (braced) {
                        if (after < in.size() && in[after] == '}') after++;
                        else { out += c; i++; continue; } // malformed ${#
                    }
                    if (sp == '#') {
                        size_t n = args.empty() ? 0 : args.size() - 1;
                        out += std::to_string(n);
                    } else { // @ or * : args 1..N joined by spaces
                        for (size_t k = 1; k < args.size(); ++k) {
                            if (k > 1) out += ' ';
                            out += args[k];
                        }
                    }
                    i = after; continue;
                }

                // Positional params: $0,$1,...  (single digit unless braced ${12}).
                if (j < in.size() && in[j] >= '0' && in[j] <= '9') {
                    size_t idx = 0;
                    if (braced) {
                        size_t ds = j;
                        while (j < in.size() && in[j] >= '0' && in[j] <= '9') {
                            idx = idx * 10 + (size_t)(in[j] - '0'); j++;
                        }
                        if (j < in.size() && in[j] == '}' && j > ds) {
                            if (idx < args.size()) out += args[idx];
                            i = j + 1; continue;
                        }
                        out += c; i++; continue; // malformed ${N
                    } else {
                        idx = (size_t)(in[j] - '0'); // exactly one digit
                        if (idx < args.size()) out += args[idx];
                        i = j + 1; continue;
                    }
                }

                // Named variables: $NAME / ${NAME}
                size_t nameStart = j;
                std::string name;
                while (j < in.size()) {
                    char d = in[j];
                    bool first = (j == nameStart);
                    bool ok = (d == '_' || (d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') ||
                               (!first && d >= '0' && d <= '9'));
                    if (!ok) break;
                    name += d; j++;
                }
                if (braced) {
                    if (j < in.size() && in[j] == '}' && !name.empty()) {
                        auto it = vars.find(name);
                        if (it != vars.end()) out += it->second;
                        i = j + 1; continue;
                    }
                    out += c; i++; continue; // malformed ${...} -> literal $
                } else if (!name.empty()) {
                    auto it = vars.find(name);
                    if (it != vars.end()) out += it->second;
                    i = j; continue;
                }
                out += c; i++; continue;     // lone $ -> literal
            }
            out += c; i++;
        }
        return out;
    }

    // Variables persist in a hidden file on the internal filesystem so they
    // survive a reboot.
    void load_vars() {
        File f = Helpers::fsOpen("/.environment", "r");
        if (!f) return;
        std::string line;
        auto handleLine = [this](std::string ln) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            if (ln.empty() || ln[0] == '#') return;
            size_t eq = ln.find('=');
            if (eq == std::string::npos || eq == 0) return;
            std::string name = ln.substr(0, eq);
            if (valid_var_name(name)) vars[name] = ln.substr(eq + 1);
        };
        while (f.available()) {
            char c = (char)f.read();
            if (c == '\n') { handleLine(line); line.clear(); }
            else line += c;
        }
        if (!line.empty()) handleLine(line);
        f.close();
    }

    // Push the current TZ variable into the C library so date/getLocalTime show
    // local time. No TZ (or empty) means UTC. Cheap - safe to call on any change.
    void sync_tz() {
        auto it = vars.find("TZ");
        Helpers::tzString = (it != vars.end() && !it->second.empty()) ? it->second : "UTC0";
        Helpers::applyTimezone();
    }

    void save_vars() {
        File f = Helpers::fsOpen("/.environment", "w");
        if (!f) return;
        for (const auto& kv : vars) {
            f.print(kv.first.c_str());
            f.print("=");
            f.print(kv.second.c_str());
            f.print("\n");
        }
        f.close();
        sync_tz(); // a variable changed - keep the timezone in sync (covers TZ=, set, unset, read)
    }

    // Strip one layer of surrounding matching quotes from a value:
    //   name="Alex"  -> Alex      age='25' -> 25      age=25 -> 25
    // Only strips when the whole value is wrapped in the same quote char.
    static std::string unquote_value(std::string v) {
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) {
            v = v.substr(1, v.size() - 2);
        }
        return v;
    }

    // Handle set / unset / NAME=value. Returns true if the line was a variable
    // command (and was handled here).
    bool handle_var_command(const std::string& cmd, LineCallback emit) {
        if (cmd == "set") {
            if (vars.empty()) { emit("(no variables)\n"); return true; }
            for (const auto& kv : vars) emit(kv.first + "=" + kv.second + "\n");
            return true;
        }
        if (cmd.rfind("set ", 0) == 0) {
            std::string rest = cmd.substr(4);
            Helpers::trim(rest);
            size_t sp = rest.find(' ');
            std::string name = (sp == std::string::npos) ? rest : rest.substr(0, sp);
            std::string value = (sp == std::string::npos) ? "" : rest.substr(sp + 1);
            Helpers::trim(value);
            if (!valid_var_name(name)) { emit("set: invalid name: " + name + "\n"); Helpers::cmd_status = 1; return true; }
            vars[name] = unquote_value(value);
            save_vars();
            return true;
        }
        if (cmd.rfind("unset ", 0) == 0) {
            auto toks = split_ws(cmd.substr(6));
            for (const auto& t : toks) vars.erase(t);
            save_vars();
            return true;
        }
        // NAME=value  (value is the rest of the line, so paths with no spaces work)
        size_t eq = cmd.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string name = cmd.substr(0, eq);
            if (name.find(' ') == std::string::npos && valid_var_name(name)) {
                vars[name] = unquote_value(cmd.substr(eq + 1));
                save_vars();
                return true;
            }
        }
        return false;
    }

    // ---- test / [ ] condition evaluation ----
    // File predicates (operand is resolved relative to the current directory).
    static bool test_exists(const std::string& p) {
        File f = Helpers::fsOpen(Helpers::make_absolute(p));
        bool e = (bool)f; if (f) f.close(); return e;
    }
    static bool test_isdir(const std::string& p) {
        File f = Helpers::fsOpen(Helpers::make_absolute(p));
        bool e = f && f.isDirectory(); if (f) f.close(); return e;
    }
    static bool test_isfile(const std::string& p) {
        File f = Helpers::fsOpen(Helpers::make_absolute(p));
        bool e = f && !f.isDirectory(); if (f) f.close(); return e;
    }
    static bool test_nonempty(const std::string& p) {
        File f = Helpers::fsOpen(Helpers::make_absolute(p));
        bool e = f && !f.isDirectory() && f.size() > 0; if (f) f.close(); return e;
    }
    // Parse a (possibly signed) integer; returns false if not a valid integer.
    static bool test_int(const std::string& s, long& out) {
        if (s.empty()) return false;
        size_t i = 0; bool neg = false;
        if (s[0] == '+' || s[0] == '-') { neg = (s[0] == '-'); i = 1; }
        if (i >= s.size()) return false;
        long v = 0;
        for (; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            v = v * 10 + (s[i] - '0');
        }
        out = neg ? -v : v; return true;
    }

    // Evaluate a test/[ ] expression. Returns 0 (true), 1 (false), 2 (error).
    // Supports: file tests -e/-f/-d/-s, string -z/-n and =/!=, numeric
    // -eq/-ne/-lt/-le/-gt/-ge, a lone non-empty string, and a leading ! negation.
    static int eval_test(std::vector<std::string> a, LineCallback emit) {
        bool negate = false;
        if (!a.empty() && a[0] == "!") { negate = true; a.erase(a.begin()); }

        int r;
        if (a.empty()) {
            r = 1;                                  // no expression -> false
        } else if (a.size() == 1) {
            r = a[0].empty() ? 1 : 0;               // non-empty string is true
        } else if (a.size() == 2) {
            const std::string& op = a[0];
            const std::string& x = a[1];
            if      (op == "-z") r = x.empty() ? 0 : 1;
            else if (op == "-n") r = x.empty() ? 1 : 0;
            else if (op == "-e") r = test_exists(x)   ? 0 : 1;
            else if (op == "-f") r = test_isfile(x)   ? 0 : 1;
            else if (op == "-d") r = test_isdir(x)    ? 0 : 1;
            else if (op == "-s") r = test_nonempty(x) ? 0 : 1;
            else { emit("test: unknown operator: " + op + "\n"); r = 2; }
        } else if (a.size() == 3) {
            const std::string& x  = a[0];
            const std::string& op = a[1];
            const std::string& y  = a[2];
            if      (op == "=" || op == "==") r = (x == y) ? 0 : 1;
            else if (op == "!=")              r = (x != y) ? 0 : 1;
            else if (op == "-eq" || op == "-ne" || op == "-lt" ||
                     op == "-le" || op == "-gt" || op == "-ge") {
                long lx, ly;
                if (!test_int(x, lx) || !test_int(y, ly)) {
                    emit("test: integer expected\n"); r = 2;
                } else {
                    bool t;
                    if      (op == "-eq") t = (lx == ly);
                    else if (op == "-ne") t = (lx != ly);
                    else if (op == "-lt") t = (lx <  ly);
                    else if (op == "-le") t = (lx <= ly);
                    else if (op == "-gt") t = (lx >  ly);
                    else                  t = (lx >= ly); // -ge
                    r = t ? 0 : 1;
                }
            } else { emit("test: unknown operator: " + op + "\n"); r = 2; }
        } else {
            emit("test: too many arguments\n"); r = 2;
        }

        if (negate && r != 2) r = (r == 0) ? 1 : 0;
        return r;
    }

    // One segment of an &&/|| chain, with the operator that PRECEDES it.
    enum class ChainOp { NONE, AND, OR };
    struct ChainSeg { ChainOp op; std::string text; };

    // Split a command line at top-level && and || operators, honoring quotes
    // (so "echo a && b" inside quotes is not split) and leaving single | (pipes)
    // intact within a segment (|| has lower precedence than |). Each segment
    // carries the operator that came before it; the first segment's op is NONE.
    static std::vector<ChainSeg> split_and_or(const std::string& in) {
        std::vector<ChainSeg> out;
        std::string cur;
        ChainOp pending = ChainOp::NONE;
        char quote = 0;
        size_t i = 0;
        while (i < in.size()) {
            char c = in[i];
            if (quote) {
                if (Helpers::escaped_pair(in, i, quote)) { // \" or \\ : keep both
                    cur += c; cur += in[i + 1]; i += 2; continue;
                }
                cur += c;
                if (c == quote) quote = 0;
                i++; continue;
            }
            if (c == '"' || c == '\'') { quote = c; cur += c; i++; continue; }
            if ((c == '&' && i + 1 < in.size() && in[i + 1] == '&') ||
                (c == '|' && i + 1 < in.size() && in[i + 1] == '|')) {
                std::string seg = cur; Helpers::trim(seg);
                out.push_back({pending, seg});
                pending = (c == '&') ? ChainOp::AND : ChainOp::OR;
                cur.clear();
                i += 2; continue;
            }
            cur += c; i++;
        }
        std::string seg = cur; Helpers::trim(seg);
        out.push_back({pending, seg});
        return out;
    }

    // Usage hint for a command name typed with no arguments. Returns "" if the
    // bare word isn't a known command that requires arguments. Used so that e.g.
    // typing just "cat" shows its usage (and fails) instead of "Unknown command".
    // Usage text for commands that need an argument: printed by the final else
    // of the dispatch when one of them is typed bare. Commands whose dispatch
    // accepts the bare form (wc/sort/uniq read the pipe; grep/cut/tr/head/tail
    // live in TextCmds) print their own usage and are deliberately not listed.
    static std::string usage_for(const std::string& c) {
        if (c == "cat")    return "Usage: cat <file>...\n";
        if (c == "find")   return "Usage: find [path] <name|pattern>\n";
        if (c == "ssh")    return "Usage: ssh [user@]host [port]\n";
        if (c == "telnet") return "Usage: telnet <host> [port]\n";
        if (c == "nc")     return "Usage: nc <host> <port>\n";
        if (c == "edit" || c == "ed") return "Usage: edit <file>\n";
        if (c == "cp")     return "Usage: cp [-r] <source...> <destination>\n";
        if (c == "mkdir")  return "Usage: mkdir <dir>...\n";
        if (c == "mv")     return "Usage: mv <source...> <destination>\n";
        if (c == "rm")     return "Usage: rm [-r] <file>...\n";
        if (c == "rmdir")  return "Usage: rmdir <dir>...\n";
        if (c == "touch")  return "Usage: touch <file>...\n";
        if (c == "unset")  return "Usage: unset <NAME>\n";
        if (c == "ping")   return "Usage: ping [-c N] <host|ip|url>\n";
        if (c == "air" || c == "air s") return "Usage: air s [seconds]\n";
        if (c == "air w") return "Usage: air w <ssid|bssid>\n";
        if (c == "led")   return "Usage: led <r|g|b|y|c|m|w|off> [count] [dur] [gap]\n";
        if (c == "wget")   return "Usage: wget <url> [-o <path>]\n";
        if (c == "wf c")   return "Usage: wf c <ssid> [-p <password>]\n";
        if (c == "net s p")return "Usage: net s p <host|ip|url> [port|a-b]...\n";
        return "";
    }

    void execute_command() {
        std::string cmd = command;
        Helpers::trim(cmd);
        command = "";
        cursorPos = 0;

        // By default output goes to the screen.
        LineCallback emit = [this](const std::string& s) {
            this->print(s);
        };

        if (cmd.empty()) {
            emit(prompt);
            inputStartY = M5Cardputer.Display.getCursorY();
            draw_caret();
            return;
        }

        // Echo the entered command into the screen scrollback (shown as typed,
        // including "> file"). Skipped while a script is running: the script's
        // commands must not fill the scrollback or the recall history - only
        // their output (and, in -v mode, the echo from run_script) is kept.
        if (scriptDepth == 0 && chainDepth == 0) {
            add_to_history(prompt + cmd);
        }

        if (WiFiCmds::waitingForPass) {
            WiFiCmds::wifi_connect_with_pass(cmd, emit);
            print_prompt();
            return;
        }

        if (sshWaitingForPass) {
            // The entered line is the SSH password (consumed, not run/stored).
            ssh_connect_with_pass(cmd);
            return; // ssh_connect_with_pass prints the prompt on failure
        }

        // Record the command in the recall history (no consecutive duplicates).
        // Passwords (handled above) never reach here. Script commands are not
        // recorded - only the `sh ...` line the user typed is.
        if (scriptDepth == 0 && chainDepth == 0 && (cmdHistory.empty() || cmdHistory.back() != cmd)) {
            cmdHistory.push_back(cmd);
            while ((int)cmdHistory.size() > max_cmd_history) {
                cmdHistory.erase(cmdHistory.begin());
            }
        }
        historyIndex = (int)cmdHistory.size();
        savedLine = "";

        // Inline compound statements typed at the prompt: if/while/until/for on a
        // single line (e.g. `for x in a b c; do echo $x; done`). Multi-line blocks
        // come through scripts (run_block); here we handle the single-line form.
        // Run it like a mini-script (silent body, one prompt after).
        {
            ScriptKw k0 = script_kw(cmd);
            bool compound = (k0 == ScriptKw::IF || k0 == ScriptKw::WHILE ||
                             k0 == ScriptKw::UNTIL || k0 == ScriptKw::FOR);
            // A line with a top-level ';' is several statements - run it through
            // the same block interpreter so `a; b`, `x=$(cmd); echo $x`, and
            // one-line if/for all work at the prompt, not just in scripts.
            bool multiStmt = compound || split_semicolons(cmd).size() > 1;
            if (multiStmt) {
                scriptInterrupted = false;
                scriptDepth++;
                auto lines = flatten_script(std::vector<std::string>{cmd});
                run_block(lines, 0, lines.size(), false);
                scriptDepth--;
                scriptInterrupted = false;
                print_prompt();
                return;
            }
        }

        // --- &&/|| short-circuit chains ---
        // Split the line at top-level && / || and run the segments with bash
        // short-circuit semantics driven by $? (Helpers::cmd_status). Done before
        // expansion so each segment expands its own $? independently, and works
        // the same at the prompt and inside scripts. The whole line was already
        // echoed/recorded above; segments run with chainDepth>0 so they don't
        // re-echo, re-record, or print their own prompts.
        if (chainDepth == 0) {
            auto segs = split_and_or(cmd);
            if (segs.size() > 1) {
                chainDepth++;
                bool prevOk = true;
                for (size_t i = 0; i < segs.size(); ++i) {
                    if (segs[i].text.empty()) continue; // skip dangling operators
                    if (i > 0) {
                        if (segs[i].op == ChainOp::AND && !prevOk) continue;
                        if (segs[i].op == ChainOp::OR  &&  prevOk) continue;
                    }
                    command = segs[i].text;
                    execute_command();                    // run one segment
                    prevOk = (Helpers::cmd_status == 0);
                }
                chainDepth--;
                print_prompt();
                return;
            }
        }

        // Expand $NAME / ${NAME}, positional args and $? (last exit status)
        // before anything else. $? must read the PREVIOUS command's status, so
        // we expand first and only then reset it to 0 (success) for this command;
        // a command that fails sets Helpers::cmd_status back to non-zero.
        cmd = expand_vars(cmd, vars, scriptArgs, Helpers::cmd_status,
                          [this](const std::string& c) { return run_capture(c); });
        Helpers::cmd_status = 0;

        // Variable management (set / unset / NAME=value) is handled here,
        // before the pipeline and glob machinery.
        if (handle_var_command(cmd, emit)) {
            print_prompt();
            return;
        }

        // --- Pipeline: <command> [| grep PATTERN] [> file | >> file] ---
        // Redirection and grep are layered on top of the emit callback:
        //   sink   - where output ultimately goes (screen or file);
        //   grep   - a per-line substring filter on top of sink;
        //   emit   - what the command receives.
        // cat/echo/date parse '>' themselves, BUT only when there is no
        // pipe (no '|'). With '| grep' the redirection
        // applies to grep, and the left command just feeds the filter.
        File redirectFile; // RAII: closed/flushed when the function returns

        // 1) General pipeline a | b | c: run every stage but the last with its
        // output captured and handed to the next stage as input (pipeInput). The
        // last stage then runs normally below - reading that input if it is a
        // filter (grep/tr/cut/...) and writing to the screen or a redirect.
        {
            std::vector<std::string> segs = split_pipes(cmd);
            if (segs.size() > 1) {
                std::string data; bool have = false;
                for (size_t i = 0; i + 1 < segs.size(); ++i) {
                    std::string seg = segs[i]; Helpers::trim(seg);
                    pipeInput = data; hasPipeInput = have;
                    data = run_capture(seg, false); // keep newlines between stages
                    have = true;
                }
                pipeInput = data; hasPipeInput = have;
                cmd = segs.back(); Helpers::trim(cmd);
            }
        }

        // 2) Determine the redirection target (a few commands parse '>' themselves)
        std::string redirectTarget;
        bool redirectAppend = false;
        {
            bool selfHandles = (cmd.rfind("cat ", 0) == 0 ||
                                cmd == "echo" ||
                                cmd.rfind("echo ", 0) == 0 ||
                                cmd == "date" ||
                                cmd.rfind("date ", 0) == 0 ||
                                cmd.rfind("date>", 0) == 0);
            if (!selfHandles) {
                std::string body, file;
                bool app;
                parse_redirect(cmd, body, file, app);
                if (!file.empty()) {
                    Helpers::trim(body);
                    cmd = body;
                    redirectTarget = file;
                    redirectAppend = app;
                }
            }
        }

        // 3) Open the file (if needed) and build the sink
        LineCallback sink = emit; // screen by default
        if (!redirectTarget.empty()) {
            redirectFile = Helpers::fsOpen(redirectTarget, redirectAppend ? "a" : "w");
            if (!redirectFile) {
                emit("Cannot write to " + Helpers::clearFilename(redirectTarget) + "\n");
                Helpers::cmd_status = 1; // so `... > bad || fallback` runs the fallback
                pipeInput.clear();
                hasPipeInput = false;
                print_prompt();
                return;
            }
            sink = [&redirectFile](const std::string& s) {
                redirectFile.print(s.c_str());
            };
        }

        // 4) Output goes straight to the sink (filters are ordinary commands now).
        emit = sink;

        // 5) Empty left command (e.g. "> file" or "| grep x" with no command)
        if (cmd.empty()) {
            print_prompt();
            return;
        }

        // 6) Shell-style glob expansion of * and ? for every command except
        // find (which does its own recursive pattern matching). Runs after the
        // pipe and redirection have been stripped, so only the command name and
        // its file arguments remain. A pattern that matches nothing is left
        // literal, exactly like a typical shell.
        // Numeric brace ranges {1..5} / {0..10..2} expand before globbing, so
        // `echo {1..3}` and `touch f{1..3}.txt` work like a shell.
        if (cmd.find('{') != std::string::npos) {
            cmd = CommonCmds::expand_braces(cmd);
        }
        if (cmd.find_first_of("*?") != std::string::npos &&
            cmd != "find" && cmd.rfind("find ", 0) != 0 &&
            cmd.rfind("wget ", 0) != 0) {
            cmd = CommonCmds::expand_globs(cmd);
        }

        try {
          // Run the command dispatch inside a lambda: an early `return` from an
          // error branch then falls through to the shared tail below (grep flush,
          // prompt) instead of skipping it. Mode-entering commands (telnet/ssh/
          // edit) set their *Active flag, which the tail checks before printing a
          // prompt, so they still take over the screen without one.
          [&]() {
            if (cmd.substr(0, 4) == "cat ") {
                std::string args, file;
                bool append;
                parse_redirect(cmd.substr(4), args, file, append);

                auto files = Helpers::parse_parts(args);
                if (files.empty()) {
                    emit("cat: missing file operand\n");
                    Helpers::cmd_status = 1;
                    return;
                }
                CommonCmds::cat(files, file, append, emit);
            }
            else if (cmd.substr(0, 3) == "cd ") {
                CommonCmds::cd(cmd.substr(3), emit);
            }
            else if (cmd == "cd") {
                CommonCmds::cd("/", emit); // bare cd -> home (root)
            }
            else if (cmd == "head" || cmd.rfind("head ", 0) == 0) {
                TextCmds::head(cmd.size() > 5 ? cmd.substr(5) : std::string(),
                               pipeInput, hasPipeInput, emit);
            }
            else if (cmd == "tail" || cmd.rfind("tail ", 0) == 0) {
                TextCmds::tail(cmd.size() > 5 ? cmd.substr(5) : std::string(),
                               pipeInput, hasPipeInput, emit);
            }
            else if (cmd.rfind("find ", 0) == 0) {
                // find <name|pattern>          - search from the current directory
                // find <path> <name|pattern>   - search from the given directory
                // Pattern: * and ? are glob; without them, substring search.
                // Bare "find" is handled by usage_for(); reaching here means the
                // command had arguments, so split_ws yields at least one token.
                auto toks = split_ws(cmd.substr(5));
                std::string start, pattern;
                if (toks.size() == 1) {
                    start = Helpers::currentDir;
                    pattern = toks[0];
                } else {
                    start = Helpers::make_absolute(toks[0]);
                    pattern = toks[1];
                }
                size_t cap = redirectTarget.empty() ? 500 : 0; // screen = capped, file = everything
                CommonCmds::find(start, pattern, cap, emit);
            }
            else if (cmd.rfind("ssh ", 0) == 0) {
                // ssh [user@]host [port]   (password auth, prompts for password)
                start_ssh(cmd.substr(4));
                return; // prompt/handoff managed inside start_ssh
            }
            else if (cmd.rfind("telnet ", 0) == 0) {
                // telnet <host> [port]   (default port 23)
                // Bare "telnet" -> usage_for(); here there is always a host token.
                auto toks = split_ws(cmd.substr(7));
                uint16_t port = 23;
                if (toks.size() >= 2) {
                    long p;
                    if (parse_uint(toks[1], p) && p >= 1 && p <= 65535) port = (uint16_t)p;
                }
                start_telnet(toks[0], port, false);
                return; // the session took over the screen
            }
            else if (cmd.rfind("nc ", 0) == 0) {
                // nc <host> <port>  - raw TCP without telnet option handling
                auto toks = split_ws(cmd.substr(3));
                if (toks.size() < 2) {
                    emit("Usage: nc <host> <port>\n");
                    Helpers::cmd_status = 1;
                    return;
                }
                long p;
                if (!parse_uint(toks[1], p) || p < 1 || p > 65535) {
                    emit("nc: invalid port\n");
                    Helpers::cmd_status = 1;
                    return;
                }
                start_telnet(toks[0], (uint16_t)p, true);
                return;
            }
            else if (cmd.rfind("edit ", 0) == 0 || cmd.rfind("ed ", 0) == 0) {
                bool isEdit = (cmd.rfind("edit ", 0) == 0);
                // Bare "edit"/"ed" -> usage_for(); here there is always a file token.
                auto toks = split_ws(cmd.substr(isEdit ? 5 : 3));
                std::string target = Helpers::make_absolute(toks[0]);
                // The editor cannot create directories, so refuse a path whose
                // parent directory doesn't exist (otherwise the file silently
                // fails to save). Files directly under root are always allowed.
                size_t slash = target.find_last_of('/');
                std::string parent = (slash == std::string::npos || slash == 0)
                                     ? "/" : target.substr(0, slash);
                if (parent != "/") {
                    File pd = Helpers::fsOpen(parent);
                    bool ok = pd && pd.isDirectory();
                    if (pd) pd.close();
                    if (!ok) {
                        emit("edit: " + parent + ": No such directory\n");
                        Helpers::cmd_status = 1;
                        return;
                    }
                }
                start_editor(target);
                return; // editor took over the screen, don't print the prompt
            }
            else if (cmd == "help") {
                SystemCmds::help(emit);
            }
            else if (cmd == "sysinfo") {
                SystemCmds::sysinfo(emit);
            }
            else if (cmd == "free") {
                SystemCmds::freemem(emit);
            }
            else if (cmd == "df") {
                SystemCmds::df(emit);
            }
            else if (cmd == "battery" || cmd == "bat") {
                SystemCmds::battery(emit);
            }
            else if (cmd.substr(0, 3) == "cp ") {
                // Parse the -r/-R/--recursive flag separately from the paths.
                bool recursive = false;
                std::vector<std::string> files;
                for (const auto& t : split_ws(cmd.substr(3))) {
                    if (t == "-r" || t == "-R" || t == "--recursive")
                        recursive = true;
                    else
                        files.push_back(Helpers::make_absolute(t));
                }
                if (files.size() < 2) {
                    emit("Usage: cp [-r] <source...> <destination>\n");
                    Helpers::cmd_status = 1;
                    return;
                }
                std::string dst = files.back();
                files.pop_back();
                CommonCmds::cp(files, dst, recursive, emit);
            }
            // FIX: was cmd.substr(0,4)=="date" - that also matched "datexyz".
            // Now require an exact command or a boundary (space / > ).
            else if (cmd == "date" || cmd.rfind("date ", 0) == 0 ||
                     cmd.rfind("date>", 0) == 0) {
                std::string textIgnored, file;
                bool append;
                // the body after the word "date"
                std::string body = (cmd.size() > 4) ? cmd.substr(4) : "";
                parse_redirect(body, textIgnored, file, append);
                CommonCmds::date(file, append, emit);
            }
            else if (cmd == "echo" || cmd.substr(0, 5) == "echo ") {
                std::string text, file;
                bool append;
                parse_redirect(cmd.size() > 5 ? cmd.substr(5) : "", text, file, append);
                // If the text uses quotes, collapse them (quote-aware): the
                // quoted runs keep their spaces, the quote chars are removed.
                if (text.find('"') != std::string::npos || text.find('\'') != std::string::npos) {
                    std::string joined;
                    for (const auto& p : Helpers::tokenize(text)) {
                        if (!joined.empty()) joined += " ";
                        joined += p;
                    }
                    text = joined;
                }
                CommonCmds::echo(text, file, append, emit);
            }
            else if (cmd == "read" || cmd.rfind("read ", 0) == 0) {
                do_read(cmd == "read" ? std::string() : cmd.substr(5), emit);
            }
            else if (cmd == "grep" || cmd.rfind("grep ", 0) == 0) {
                TextCmds::grep(cmd == "grep" ? std::string() : cmd.substr(5),
                               pipeInput, hasPipeInput, emit);
            }
            else if (cmd == "cut" || cmd.rfind("cut ", 0) == 0) {
                TextCmds::cut(cmd == "cut" ? std::string() : cmd.substr(4),
                              pipeInput, hasPipeInput, emit);
            }
            else if (cmd == "tr" || cmd.rfind("tr ", 0) == 0) {
                TextCmds::tr(cmd == "tr" ? std::string() : cmd.substr(3),
                             pipeInput, hasPipeInput, emit);
            }
            else if (cmd == "wc" || cmd.rfind("wc ", 0) == 0) {
                TextCmds::wc(cmd == "wc" ? std::string() : cmd.substr(3),
                             pipeInput, hasPipeInput, emit);
            }
            else if (cmd == "sort" || cmd.rfind("sort ", 0) == 0) {
                TextCmds::sort(cmd == "sort" ? std::string() : cmd.substr(5),
                               pipeInput, hasPipeInput, emit);
            }
            else if (cmd == "uniq" || cmd.rfind("uniq ", 0) == 0) {
                TextCmds::uniq(cmd == "uniq" ? std::string() : cmd.substr(5),
                               pipeInput, hasPipeInput, emit);
            }
            else if (cmd == "test" || cmd.rfind("test ", 0) == 0 ||
                     cmd == "[" || cmd.rfind("[ ", 0) == 0) {
                // test EXPR  /  [ EXPR ]  - evaluate a condition, set $? only.
                bool bracket = (cmd == "[" || cmd.rfind("[ ", 0) == 0);
                std::string rest = bracket ? (cmd.size() > 1 ? cmd.substr(1) : "")
                                           : (cmd.size() > 4 ? cmd.substr(4) : "");
                auto args = split_ws(rest); // quote-aware tokens
                if (bracket) {
                    if (args.empty() || args.back() != "]") {
                        emit("[: missing ']'\n");
                        Helpers::cmd_status = 2;
                    } else {
                        args.pop_back(); // drop the closing ]
                        Helpers::cmd_status = eval_test(args, emit);
                    }
                } else {
                    Helpers::cmd_status = eval_test(args, emit);
                }
            }
            else if (cmd == "ls" || cmd.rfind("ls ", 0) == 0) {
                // ls [-a] [path...]   -a shows hidden (dot) names.
                // No path -> current directory. A path may be a directory
                // (its contents are listed) or a file (its entry is shown).
                // Cap on-screen lines so a huge directory doesn't hang in
                // endless scrolling; when redirecting to a file print all.
                auto toks = split_ws(cmd.size() > 2 ? cmd.substr(2) : "");
                bool showAll = false;
                std::vector<std::string> paths;
                for (const auto& t : toks) {
                    if (t == "-a" || t == "-la" || t == "-al") showAll = true;
                    else paths.push_back(t);
                }

                size_t cap = redirectTarget.empty() ? 500 : 0;
                if (paths.empty()) {
                    CommonCmds::ls(Helpers::currentDir, showAll, cap, emit);
                } else {
                    bool multi = paths.size() > 1;
                    for (const auto& pth : paths) {
                        CommonCmds::ls_target(Helpers::make_absolute(pth), pth,
                                              showAll, cap, multi, emit);
                    }
                }
            }
            else if (cmd == "mount") {
                Helpers::mountSD(emit);
            }
            else if (cmd == "umount" || cmd == "unmount") {
                Helpers::umountSD(emit);
            }
            else if (cmd.substr(0, 6) == "mkdir ") {
                // Bare "mkdir" -> usage_for(); here parse_parts yields >=1 dir.
                auto dirs = Helpers::parse_parts(cmd.substr(6));
                CommonCmds::mkdir(dirs, emit);
            }
            else if (cmd.substr(0, 3) == "mv ") {
                auto files = Helpers::parse_parts(cmd.substr(3));
                if (files.size() < 2) {
                    emit("Usage: mv <source...> <destination>\n");
                    Helpers::cmd_status = 1;
                    return;
                }
                std::string dst = files.back();
                files.pop_back();
                CommonCmds::mv(files, dst, emit);
            }
            else if (cmd == "pwd") {
                CommonCmds::pwd(Helpers::currentDir, emit);
            }
            else if (cmd.substr(0, 3) == "rm ") {
                // Parse the -r/-R/--recursive flag separately from the paths.
                bool recursive = false;
                std::vector<std::string> files;
                for (const auto& t : split_ws(cmd.substr(3))) {
                    if (t == "-r" || t == "-R" || t == "--recursive")
                        recursive = true;
                    else
                        files.push_back(Helpers::make_absolute(t));
                }
                if (files.empty()) { // e.g. "rm -r" with no path given
                    emit("rm: missing operand\n");
                    Helpers::cmd_status = 1;
                    return;
                }
                CommonCmds::rm(files, recursive, emit);
            }
            else if (cmd.substr(0, 6) == "rmdir ") {
                CommonCmds::rmdir(cmd.substr(6), emit);
            }
            else if (cmd.substr(0, 6) == "touch ") {
                CommonCmds::touch(cmd.substr(6), emit);
            }
            else if (cmd == "wf s") {
                WiFiCmds::wifi_scan(emit);
            }
            else if (cmd.substr(0, 5) == "wf c ") {
                WiFiCmds::wifi_connect(cmd.substr(5), emit);
            }
            else if (cmd == "wf dc") {
                WiFiCmds::wifi_disconnect(emit);
            }
            else if (cmd == "ap" || cmd.rfind("ap ", 0) == 0) {
                // ap -s <ssid> [-p <pw>] start | ap stop | ap status
                auto toks = split_ws(cmd.size() > 2 ? cmd.substr(2) : "");
                std::string ssid, pass, action;
                bool haveS = false;
                for (size_t i = 0; i < toks.size(); ++i) {
                    if (toks[i] == "-s" && i + 1 < toks.size()) { ssid = toks[++i]; haveS = true; }
                    else if (toks[i] == "-p" && i + 1 < toks.size()) { pass = toks[++i]; }
                    else if (toks[i] == "start" || toks[i] == "stop" || toks[i] == "status") action = toks[i];
                }
                if (action == "stop") WiFiCmds::ap_stop(emit);
                else if (action == "status" || (action.empty() && !haveS)) WiFiCmds::ap_status(emit);
                else if (action == "start") {
                    if (!haveS) { emit("Usage: ap -s <ssid> [-p <password>] start\n"); Helpers::cmd_status = 1; }
                    else WiFiCmds::ap_start(ssid, pass, emit);
                } else {
                    emit("Usage: ap -s <ssid> [-p <password>] start | ap stop | ap status\n");
                    Helpers::cmd_status = 1;
                }
            }
            else if (cmd == "net s") {
                NetworkCmds::net_scan(emit);
            }
            else if (cmd.rfind("wget ", 0) == 0) {
                NetworkCmds::wget(cmd.substr(5), emit);
            }
            else if (cmd == "wget") {
                emit(usage_for("wget"));
                Helpers::cmd_status = 1;
            }
            else if (cmd == "httpd" || cmd.rfind("httpd ", 0) == 0) {
                HttpdCmds::httpd(cmd.size() > 5 ? cmd.substr(6) : "", emit);
            }
            else if (cmd == "reboot") {
                emit("Rebooting...\n");
                delay(300);
                ESP.restart();
            }
            else if (cmd == "clear") {
                // Wipe the screen and the scrollback, then the tail prints a
                // fresh prompt at the top.
                history.clear();
                scroll_offset = 0;
                M5Cardputer.Display.fillScreen(BLACK);
                M5Cardputer.Display.setCursor(0, 0);
                Helpers::cmd_status = 0;
            }
            else if (cmd == "sleep" || cmd.rfind("sleep ", 0) == 0) {
                // sleep <seconds> - pause (Ctrl+C interrupts and aborts a script).
                std::string arg = (cmd == "sleep") ? std::string() : cmd.substr(6);
                Helpers::trim(arg);
                long secs;
                if (arg.empty() || !parse_uint(arg, secs) || secs < 0 || secs > 86400) {
                    emit("Usage: sleep <seconds>\n");
                    Helpers::cmd_status = 1;
                    return;
                }
                bool interrupted = false;
                uint32_t endAt = millis() + (uint32_t)secs * 1000;
                while ((int32_t)(endAt - millis()) > 0) {
                    if (loop_break_pressed()) { interrupted = true; break; }
                    delay(20);
                }
                if (interrupted) {
                    emit("^C\n");
                    Helpers::cmd_status = 130;
                    scriptInterrupted = true;
                } else {
                    Helpers::cmd_status = 0;
                }
            }
            else if (cmd == "sh" || cmd.rfind("sh ", 0) == 0) {
                // sh [-v] <file> [args...] - run a script: each line executed as a
                // command, blank lines and '#' comments skipped. Silent by default;
                // -v echoes each command. Tokens after <file> become positional
                // params $1..$N (also $@, $#); $0 is the script name as given.
                std::string rest = (cmd.size() > 3) ? cmd.substr(3) : "";
                bool verbose = false;
                bool haveFile = false;
                std::string file;
                std::vector<std::string> args; // [0]=$0 (file), [1..]=positional
                for (const auto& t : split_ws(rest)) {     // raw tokens
                    if (!haveFile) {
                        if (t == "-v") { verbose = true; continue; } // flags precede file
                        file = t; haveFile = true;
                        args.push_back(t);                  // $0 = name as given
                    } else {
                        args.push_back(t);                  // $1, $2, ...
                    }
                }
                if (!haveFile) {
                    emit("Usage: sh [-v] <file> [args...]\n");
                    Helpers::cmd_status = 1;
                } else {
                    std::string path = Helpers::make_absolute(file);
                    if (!run_script(path, verbose, args)) {
                        emit("sh: cannot open " + path + "\n");
                        Helpers::cmd_status = 1;
                    }
                }
            }
            else if (cmd.rfind("ping ", 0) == 0) {
                // ping [-c N] <host|ip|url>   (no -c => ping until Ctrl+C)
                //   ping google.com
                //   ping -c 5 192.168.0.1
                //   ping https://example.com -c 10
                auto toks = split_ws(cmd.substr(5));
                std::string target;
                uint32_t count = 0; // 0 = infinite
                for (size_t i = 0; i < toks.size(); ++i) {
                    if (toks[i] == "-c" && i + 1 < toks.size()) {
                        long c;
                        if (parse_uint(toks[i + 1], c) && c >= 1 && c <= 1000000)
                            count = (uint32_t)c;
                        ++i; // consume the number
                    } else if (target.empty() && !toks[i].empty() && toks[i][0] != '-') {
                        target = toks[i];
                    }
                }
                if (target.empty()) {
                    emit("Usage: ping [-c N] <host|ip|url>\n");
                    Helpers::cmd_status = 1;
                    return;
                }
                NetworkCmds::ping(target, count, emit);
            }
            else if (cmd == "air s" || cmd.rfind("air s ", 0) == 0) {
                // air s [seconds] - passive 802.11 monitor (default 60s).
                uint32_t secs = 60;
                if (cmd.rfind("air s ", 0) == 0) {
                    std::string arg = cmd.substr(6);
                    Helpers::trim(arg);
                    long s;
                    if (parse_uint(arg, s) && s >= 1 && s <= 300) secs = (uint32_t)s;
                }
                AirCmds::sniff(secs, emit);
            }
            else if (cmd == "air w" || cmd.rfind("air w ", 0) == 0) {
                // air w <ssid|bssid> - live monitor of one AP.
                std::string arg = (cmd.size() > 6) ? cmd.substr(6) : "";
                Helpers::trim(arg);
                if (arg.empty()) {
                    emit("Usage: air w <ssid|bssid>\n");
                    Helpers::cmd_status = 1;
                    return;
                }
                liveCommand = true;
                AirCmds::watch(arg, 60, emit, [this]() { return air_poll(); });
                liveCommand = false;
            }
            else if (cmd == "beep" || cmd.rfind("beep ", 0) == 0) {
                // beep [count] [dur] [gap]   (gap defaults to dur)
                auto toks = split_ws(cmd == "beep" ? std::string() : cmd.substr(5));
                long count = 1, dur = 150, gap = -1, v;
                if (toks.size() >= 1 && parse_uint(toks[0], v) && v >= 1 && v <= 100)   count = v;
                if (toks.size() >= 2 && parse_uint(toks[1], v) && v >= 1 && v <= 5000)  dur = v;
                if (toks.size() >= 3 && parse_uint(toks[2], v) && v >= 0 && v <= 5000)  gap = v;
                if (gap < 0) gap = dur;
                HwCmds::beep((uint16_t)count, (uint16_t)dur, (uint16_t)gap, emit);
                if (Helpers::cmd_status == 130) scriptInterrupted = true;
            }
            else if (cmd == "led" || cmd.rfind("led ", 0) == 0) {
                // led <color> [count] [dur] [gap]   |   led <color>   |   led off
                auto toks = split_ws(cmd == "led" ? std::string() : cmd.substr(4));
                if (toks.empty()) {
                    emit(usage_for("led"));
                    Helpers::cmd_status = 1;
                    return;
                }
                if (toks.size() == 1) {
                    HwCmds::led_set(toks[0], emit); // hold colour (or off)
                } else {
                    long count = 1, dur = 150, gap = -1, v;
                    if (toks.size() >= 2 && parse_uint(toks[1], v) && v >= 1 && v <= 100)  count = v;
                    if (toks.size() >= 3 && parse_uint(toks[2], v) && v >= 1 && v <= 5000) dur = v;
                    if (toks.size() >= 4 && parse_uint(toks[3], v) && v >= 0 && v <= 5000) gap = v;
                    if (gap < 0) gap = dur;
                    HwCmds::led_blink(toks[0], (uint16_t)count, (uint16_t)dur, (uint16_t)gap, emit);
                    if (Helpers::cmd_status == 130) scriptInterrupted = true;
                }
            }
            else if (cmd.rfind("net s p ", 0) == 0) {
                // net s p <host|ip|url> [port|range...]
                //   with ports:    net s p 192.168.1.1 21 22 80 8080
                //   a range:       net s p scanme.nmap.org 21-80
                //   no ports:      net s p example.com   (scans the top 1000)
                auto tokens = split_ws(cmd.substr(8));

                IPAddress ip;
                std::string host;
                int rc = NetworkCmds::resolve_target(tokens[0], ip, host);
                if (rc != 0) {
                    if (rc == 1)      emit("net s p: invalid target\n");
                    else if (rc == 2) emit("Not connected to WiFi\n");
                    else              emit("net s p: cannot resolve " + host + "\n");
                    Helpers::cmd_status = 1;
                    return;
                }

                // Port arguments follow the host; if none are given, scan the
                // built-in list of the ~1000 most common ports.
                std::vector<std::string> portArgs(tokens.begin() + 1, tokens.end());
                if (portArgs.empty()) {
                    portArgs = split_ws(NetworkCmds::default_ports());
                    emit("No ports given, scanning top 1000...\n");
                }

                const size_t MAX_PORTS = 1024;
                std::vector<uint16_t> ports;
                std::set<uint16_t> seen;
                bool truncated = false;

                auto addPort = [&](long p) {
                    if (ports.size() >= MAX_PORTS) { truncated = true; return; }
                    uint16_t pp = static_cast<uint16_t>(p);
                    if (seen.insert(pp).second) ports.push_back(pp);
                };

                bool bad = false;
                for (size_t i = 0; i < portArgs.size() && !truncated; ++i) {
                    const std::string& tok = portArgs[i];
                    size_t dash = tok.find('-');

                    if (dash != std::string::npos) {
                        long a, b;
                        if (!parse_uint(tok.substr(0, dash), a) ||
                            !parse_uint(tok.substr(dash + 1), b) ||
                            a < 1 || a > 65535 || b < 1 || b > 65535 || a > b) {
                            emit("Invalid port range: " + tok + "\n");
                            bad = true; break;
                        }
                        for (long p = a; p <= b && !truncated; ++p) addPort(p);
                    } else {
                        long p;
                        if (!parse_uint(tok, p) || p < 1 || p > 65535) {
                            emit("Invalid port: " + tok + "\n");
                            bad = true; break;
                        }
                        addPort(p);
                    }
                }

                if (bad) { Helpers::cmd_status = 1; return; }
                if (ports.empty()) {
                    emit("No valid ports\n");
                    Helpers::cmd_status = 1;
                    return;
                }
                if (truncated) {
                    emit("Note: too many ports, scanning first 1024\n");
                }

                NetworkCmds::net_scan_ports(ip, ports, emit);
            }
            else {
                std::string usage = usage_for(cmd);
                if (!usage.empty()) {
                    emit(usage); // a known command typed without its arguments
                    Helpers::cmd_status = 1;
                } else {
                    emit("Unknown command: " + cmd + "\n");
                    Helpers::cmd_status = 127; // bash convention for "command not found"
                }
            }
          }(); // end dispatch lambda (invoked immediately)
        }
        catch (...) {
            emit("Execution error\n");
            Helpers::cmd_status = 1;
        }

        // Consumed by this command: clear BOTH the flag and the buffer, or the
        // next bare filter would silently reuse the previous pipeline's data.
        pipeInput.clear();
        hasPipeInput = false;

        // Print the next prompt unless a full-screen mode took over (editor,
        // telnet, ssh) or we are waiting for a password to be typed.
        if (!editorActive && !telnetActive && !sshActive &&
            !sshWaitingForPass && !WiFiCmds::waitingForPass) {
            print_prompt();
        }
    }

    // --- small parsing utilities ---

    // Splits a string on spaces, returning non-empty tokens (no make_absolute).
    static std::vector<std::string> split_ws(const std::string& s) {
        return Helpers::tokenize(s);
    }

    // Safe unsigned-integer parse without relying on exceptions.
    // Thin wrapper so existing call sites keep working (implementation is in Helpers).
    static bool parse_uint(const std::string& s, long& out) {
        return Helpers::parse_uint(s, out);
    }
};
