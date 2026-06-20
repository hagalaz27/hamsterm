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
#include "WiFiCmds.h"
#include "SystemCmds.h"
#include "Editor.h"
#include "Telnet.h"
#include "Ssh.h"
#include "Helpers.h"

class Terminal {
private:
    std::string command = "";
    std::string prompt = ">";
    std::vector<std::string> history;
    int max_history = 100;
    int scroll_offset = 0;
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
        M5Cardputer.Display.setRotation(1);
        M5Cardputer.Display.setTextScroll(true);
        M5Cardputer.Display.fillScreen(BLACK);
        M5Cardputer.Display.setCursor(0, 0);
        M5Cardputer.Display.setFont(&fonts::efontCN_14);
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.print(prompt.c_str());
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

        if (scroll_offset == 0) {
            M5Cardputer.Display.print(prompt.c_str());
            M5Cardputer.Display.print(command.c_str());
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

        // Redraw the whole command line (more reliable than manual geometry)
        int y = M5Cardputer.Display.getCursorY();
        M5Cardputer.Display.fillRect(0, y, M5Cardputer.Display.width(), 14, BLACK);
        M5Cardputer.Display.setCursor(0, y);
        command += suffix;
        M5Cardputer.Display.print(prompt.c_str());
        M5Cardputer.Display.print(command.c_str());
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
        redraw_input();
    }

    // --- Editor ---
    bool editor_active() const { return editorActive; }

    void start_editor(const std::string& path) {
        editor = new Editor(path);
        if (!editor->ok()) {
            print(editor->error() + std::string("\n"));
            delete editor;
            editor = nullptr;
            M5Cardputer.Display.print(prompt.c_str());
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
            M5Cardputer.Display.print(prompt.c_str());
            return;
        }
        print("Connecting to " + host + ":" + std::to_string(port) + "...\n");
        telnet = new TelnetSession(raw); // raw = nc (no telnet negotiation)
        if (!telnet->begin(host, port)) {
            print("telnet: connection failed\n");
            delete telnet;
            telnet = nullptr;
            M5Cardputer.Display.print(prompt.c_str());
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
        if (positional.empty()) { print("Usage: ssh [user@]host [port]\n"); M5Cardputer.Display.print(prompt.c_str()); return; }

        std::string target = positional[0];
        size_t at = target.find('@');
        if (at != std::string::npos) { user = target.substr(0, at); host = target.substr(at + 1); }
        else host = target;
        // optional bare port as second positional
        if (positional.size() >= 2) { long v; if (parse_uint(positional[1], v) && v >= 1 && v <= 65535) port = (uint16_t)v; }

        if (host.empty()) { print("ssh: no host\n"); M5Cardputer.Display.print(prompt.c_str()); return; }
        if (user.empty()) { print("ssh: no user (use user@host or -l user)\n"); M5Cardputer.Display.print(prompt.c_str()); return; }
        if (WiFi.status() != WL_CONNECTED) { print("Not connected to WiFi\n"); M5Cardputer.Display.print(prompt.c_str()); return; }

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
            delete ssh; ssh = nullptr;
            M5Cardputer.Display.print(prompt.c_str());
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
    void ssh_disconnect()         { if (sshActive) ssh_teardown(); }

    void handle_keypress(uint8_t key) {
        // Any normal key resets the tab-completion state
        commandNumber = -1;
        previousValue = "";
        tabPrefix = "";
        tabMatches.clear();

        // Cardputer key codes
        if (key == 181) {            // scroll up (Fn+,)
            scroll_up();
            return;
        }
        if (key == 182) {            // scroll down (Fn+/)
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

        if (key == 8) { // Backspace
            if (command.length() > 0) {
                command.pop_back();
                historyIndex = (int)cmdHistory.size(); // editing detaches from history
                int x = M5Cardputer.Display.getCursorX();
                int y = M5Cardputer.Display.getCursorY();
                M5Cardputer.Display.fillRect(x - 7, y, 7, 14, BLACK);
                M5Cardputer.Display.setCursor(x - 7, y);
            }
        }
        else if (key == 10 || key == 13) { // Enter
            M5Cardputer.Display.println();
            execute_command();
        }
        else if (key >= 32 && key <= 126) {
            char c = (char)key;
            command += c;
            historyIndex = (int)cmdHistory.size(); // editing detaches from history
            M5Cardputer.Display.print(c);
        }
    }

    void print(const std::string& text) {
        add_to_history(text);
        M5Cardputer.Display.print(text.c_str());
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

        size_t pos = body.find(">>");
        if (pos != std::string::npos) {
            text = body.substr(0, pos);
            file = body.substr(pos + 2);
            append = true;
        } else {
            pos = body.find('>');
            if (pos != std::string::npos) {
                text = body.substr(0, pos);
                file = body.substr(pos + 1);
            } else {
                text = body;
            }
        }

        if (!file.empty()) {
            file.erase(0, file.find_first_not_of(" "));
            size_t space = file.find(' ');
            if (space != std::string::npos) file.erase(space);
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

    // Expand $NAME and ${NAME} against the given variables. Undefined names
    // expand to an empty string (like a typical shell). "\$" yields a literal $.
    static std::string expand_vars(const std::string& in,
                                   const std::map<std::string, std::string>& vars) {
        std::string out;
        out.reserve(in.size());
        size_t i = 0;
        while (i < in.size()) {
            char c = in[i];
            if (c == '\\' && i + 1 < in.size() && in[i + 1] == '$') {
                out += '$'; i += 2; continue; // escaped: \$ -> $
            }
            if (c == '$') {
                size_t j = i + 1;
                bool braced = false;
                if (j < in.size() && in[j] == '{') { braced = true; j++; }
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
            if (!valid_var_name(name)) { emit("set: invalid name: " + name + "\n"); return true; }
            vars[name] = value;
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
                vars[name] = cmd.substr(eq + 1);
                save_vars();
                return true;
            }
        }
        return false;
    }

    void execute_command() {
        std::string cmd = command;
        Helpers::trim(cmd);
        command = "";

        // By default output goes to the screen.
        LineCallback emit = [this](const std::string& s) {
            this->print(s);
        };

        if (cmd.empty()) {
            emit(prompt);
            return;
        }

        // Echo the entered command into the screen history (shown as typed,
        // including "> file").
        add_to_history(prompt + cmd);

        if (WiFiCmds::waitingForPass) {
            WiFiCmds::wifi_connect_with_pass(cmd, emit);
            M5Cardputer.Display.print(prompt.c_str());
            return;
        }

        if (sshWaitingForPass) {
            // The entered line is the SSH password (consumed, not run/stored).
            ssh_connect_with_pass(cmd);
            return; // ssh_connect_with_pass prints the prompt on failure
        }

        // Record the command in history (no consecutive duplicates).
        // Passwords (handled above) never reach here.
        if (cmdHistory.empty() || cmdHistory.back() != cmd) {
            cmdHistory.push_back(cmd);
            while ((int)cmdHistory.size() > max_cmd_history) {
                cmdHistory.erase(cmdHistory.begin());
            }
        }
        historyIndex = (int)cmdHistory.size();
        savedLine = "";

        // Expand $NAME / ${NAME} from user variables before anything else, so
        // they work in every command (and in redirect targets, paths, etc.).
        cmd = expand_vars(cmd, vars);

        // Variable management (set / unset / NAME=value) is handled here,
        // before the pipeline and glob machinery.
        if (handle_var_command(cmd, emit)) {
            M5Cardputer.Display.print(prompt.c_str());
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
        std::shared_ptr<std::string> grepBuf;
        bool hasGrep = false;
        std::string grepPattern;

        // 1) Extract "| grep PATTERN"
        size_t pipePos = cmd.find('|');
        if (pipePos != std::string::npos) {
            std::string right = cmd.substr(pipePos + 1);
            Helpers::trim(right);
            if (right.rfind("grep", 0) == 0 &&
                (right.size() == 4 || right[4] == ' ')) {
                hasGrep = true;
                grepPattern = (right.size() > 4) ? right.substr(5) : ""; // "PATTERN [> file]"
                std::string left = cmd.substr(0, pipePos);
                Helpers::trim(left);
                cmd = left; // run only the left command
            }
        }

        // 2) Determine the redirection target
        std::string redirectTarget;
        bool redirectAppend = false;

        if (hasGrep) {
            // '>' on the grep side; the pattern is whatever precedes it
            std::string patText, file;
            bool app;
            parse_redirect(grepPattern, patText, file, app);
            Helpers::trim(patText);
            grepPattern = patText;
            redirectTarget = file;
            redirectAppend = app;
        } else {
            bool selfHandles = (cmd.rfind("cat ", 0) == 0 ||
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
                M5Cardputer.Display.print(prompt.c_str());
                return;
            }
            sink = [&redirectFile](const std::string& s) {
                redirectFile.print(s.c_str());
            };
        }

        // 4) Build the active emit: grep filter over sink, or sink directly
        if (hasGrep) {
            grepBuf = std::make_shared<std::string>();
            std::string pat = grepPattern;
            emit = [sink, grepBuf, pat](const std::string& s) {
                *grepBuf += s;
                size_t nl;
                while ((nl = grepBuf->find('\n')) != std::string::npos) {
                    std::string line = grepBuf->substr(0, nl);
                    grepBuf->erase(0, nl + 1);
                    if (pat.empty() || line.find(pat) != std::string::npos) {
                        sink(line + "\n");
                    }
                }
            };
        } else {
            emit = sink;
        }

        // 5) Empty left command (e.g. "> file" or "| grep x" with no command)
        if (cmd.empty()) {
            M5Cardputer.Display.print(prompt.c_str());
            return;
        }

        // 6) Shell-style glob expansion of * and ? for every command except
        // find (which does its own recursive pattern matching). Runs after the
        // pipe and redirection have been stripped, so only the command name and
        // its file arguments remain. A pattern that matches nothing is left
        // literal, exactly like a typical shell.
        if (cmd.find_first_of("*?") != std::string::npos &&
            cmd != "find" && cmd.rfind("find ", 0) != 0) {
            cmd = CommonCmds::expand_globs(cmd);
        }

        try {
            if (cmd.substr(0, 4) == "cat ") {
                std::string args, file;
                bool append;
                parse_redirect(cmd.substr(4), args, file, append);

                auto files = Helpers::parse_parts(args);
                if (files.empty()) {
                    emit("cat: missing file operand\n");
                    return;
                }
                CommonCmds::cat(files, file, append, emit);
            }
            else if (cmd.substr(0, 3) == "cd ") {
                CommonCmds::cd(cmd.substr(3), emit);
            }
            else if (cmd.rfind("head ", 0) == 0 || cmd.rfind("tail ", 0) == 0) {
                // head/tail [-n N | -N] <file>...   (default 10 lines)
                bool isHead = (cmd.rfind("head ", 0) == 0);
                auto toks = split_ws(cmd.substr(5));
                size_t n = 10;
                std::vector<std::string> names;

                for (size_t i = 0; i < toks.size(); ++i) {
                    const std::string& t = toks[i];
                    if (t == "-n" && i + 1 < toks.size()) {
                        long v;
                        if (parse_uint(toks[++i], v) && v >= 1) {
                            if (v > 100) v = 100; // upper bound
                            n = (size_t)v;
                        }
                    } else if (t.size() > 1 && t[0] == '-') {
                        long v;
                        if (parse_uint(t.substr(1), v) && v >= 1) {
                            if (v > 100) v = 100;
                            n = (size_t)v;
                        }
                    } else {
                        names.push_back(t); // keep every file, not just the last
                    }
                }

                if (names.empty()) {
                    emit(std::string("Usage: ") + (isHead ? "head" : "tail") + " [-n N] <file>...\n");
                    return;
                }

                bool multi = names.size() > 1;
                for (size_t i = 0; i < names.size(); ++i) {
                    if (multi) {
                        if (i) emit("\n");
                        emit("==> " + names[i] + " <==\n"); // GNU-style header
                    }
                    std::string file = Helpers::make_absolute(names[i]);
                    if (isHead) CommonCmds::head(file, n, emit);
                    else        CommonCmds::tail(file, n, emit);
                }
            }
            else if (cmd.rfind("find ", 0) == 0) {
                // find <name|pattern>          - search from the current directory
                // find <path> <name|pattern>   - search from the given directory
                // Pattern: * and ? are glob; without them, substring search.
                auto toks = split_ws(cmd.substr(5));
                if (toks.empty()) {
                    emit("Usage: find [path] <name|pattern>\n");
                    return;
                }
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
                auto toks = split_ws(cmd.substr(7));
                if (toks.empty()) {
                    emit("Usage: telnet <host> [port]\n");
                    return;
                }
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
                    return;
                }
                long p;
                if (!parse_uint(toks[1], p) || p < 1 || p > 65535) {
                    emit("nc: invalid port\n");
                    return;
                }
                start_telnet(toks[0], (uint16_t)p, true);
                return;
            }
            else if (cmd.rfind("edit ", 0) == 0 || cmd.rfind("ed ", 0) == 0) {
                bool isEdit = (cmd.rfind("edit ", 0) == 0);
                auto toks = split_ws(cmd.substr(isEdit ? 5 : 3));
                if (toks.empty()) {
                    emit("Usage: edit <file>\n");
                    return;
                }
                start_editor(Helpers::make_absolute(toks[0]));
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
            else if (cmd.substr(0, 5) == "echo ") {
                std::string text, file;
                bool append;
                parse_redirect(cmd.substr(5), text, file, append);
                CommonCmds::echo(text, file, append, emit);
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
                auto dirs = Helpers::parse_parts(cmd.substr(6));
                if (dirs.empty()) {
                    emit("mkdir: missing file operand\n");
                    return;
                }
                CommonCmds::mkdir(dirs, emit);
            }
            else if (cmd.substr(0, 3) == "mv ") {
                auto files = Helpers::parse_parts(cmd.substr(3));
                if (files.size() < 2) {
                    emit("Usage: mv <source...> <destination>\n");
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
                    if (!haveS) emit("Usage: ap -s <ssid> [-p <password>] start\n");
                    else WiFiCmds::ap_start(ssid, pass, emit);
                } else {
                    emit("Usage: ap -s <ssid> [-p <password>] start | ap stop | ap status\n");
                }
            }
            else if (cmd == "net s") {
                NetworkCmds::net_scan(emit);
            }
            else if (cmd.rfind("ping ", 0) == 0) {
                // ping <host|ip|url> [count]
                //   ping https://www.google.com
                //   ping 192.168.0.185
                //   ping google.com 8
                auto toks = split_ws(cmd.substr(5));
                if (toks.empty()) {
                    emit("Usage: ping <host|ip|url> [count]\n");
                    return;
                }
                uint8_t count = 4;
                if (toks.size() >= 2) {
                    long c;
                    if (parse_uint(toks[1], c) && c >= 1 && c <= 30) {
                        count = static_cast<uint8_t>(c);
                    }
                }
                NetworkCmds::ping(toks[0], count, emit);
            }
            else if (cmd.rfind("net s p ", 0) == 0) {
                // net s p <ip> <port|range>...
                //   individual ports:  net s p 192.168.1.1 21 22 80 8080
                //   range:             net s p 192.168.1.1 21-80
                //   can be mixed:      net s p 192.168.1.1 22 80 1000-1010
                auto tokens = split_ws(cmd.substr(8));

                if (tokens.size() < 2) {
                    emit("Usage: net s p <ip> <port|a-b>...\n");
                    return;
                }

                IPAddress ip;
                if (!ip.fromString(String(tokens[0].c_str()))) {
                    emit("Invalid IP address\n");
                    return;
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
                for (size_t i = 1; i < tokens.size() && !truncated; ++i) {
                    const std::string& tok = tokens[i];
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

                if (bad) return;
                if (ports.empty()) {
                    emit("No valid ports\n");
                    return;
                }
                if (truncated) {
                    emit("Note: too many ports, scanning first 1024\n");
                }

                NetworkCmds::net_scan_ports(ip, ports, emit);
            }
            else {
                emit("Unknown command: " + cmd + "\n");
            }
        }
        catch (...) {
            emit("Execution error\n");
        }

        // Flush the last grep line without a trailing '\n'
        if (hasGrep && grepBuf && !grepBuf->empty()) {
            if (grepPattern.empty() || grepBuf->find(grepPattern) != std::string::npos) {
                sink(*grepBuf + "\n");
            }
        }

        if (!WiFiCmds::waitingForPass) {
            M5Cardputer.Display.print(prompt.c_str());
        }
    }

    // --- small parsing utilities ---

    // Splits a string on spaces, returning non-empty tokens (no make_absolute).
    static std::vector<std::string> split_ws(const std::string& s) {
        std::vector<std::string> out;
        size_t pos = 0;
        while (true) {
            size_t sp = s.find(' ', pos);
            std::string tok = (sp == std::string::npos)
                ? s.substr(pos) : s.substr(pos, sp - pos);
            if (!tok.empty()) out.push_back(tok);
            if (sp == std::string::npos) break;
            pos = sp + 1;
        }
        return out;
    }

    // Safe unsigned-integer parse without relying on exceptions.
    static bool parse_uint(const std::string& s, long& out) {
        if (s.empty()) return false;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
        }
        errno = 0;
        char* end = nullptr;
        long v = strtol(s.c_str(), &end, 10);
        if (errno != 0 || end == s.c_str() || *end != '\0') return false;
        out = v;
        return true;
    }
};
