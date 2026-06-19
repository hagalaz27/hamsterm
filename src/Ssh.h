#pragma once

#include <M5Cardputer.h>
#include <vector>
#include <string>

extern "C" {
#include <libssh/libssh.h>
}

// Interactive SSH client (modal), built on LibSSH-ESP32 (ewpa port of libssh).
//
// IMPORTANT (device-side notes):
//  * Requires the LibSSH-ESP32 library (added to platformio.ini lib_deps) and
//    libssh_begin() called once at startup (see main.cpp).
//  * The crypto handshake needs a large stack; the loop task stack is raised to
//    ~50 KB in main.cpp (SET_LOOP_TASK_STACK_SIZE). On the Cardputer (no PSRAM)
//    this is RAM-tight: expect a single session and reduced free heap.
//  * Host key is NOT verified (typical for a microcontroller client). Password
//    authentication only; no key-based auth in this version.
//
// The shell runs in char mode (the server echoes), so keystrokes are sent
// immediately and server output is shown as it arrives - like telnet char mode.
class SshSession {
public:
    // Blocking: DNS + TCP + key exchange + auth + shell. Returns false on error
    // (call error() for the message). Can take several seconds.
    bool begin(const std::string& host, const std::string& user,
               const std::string& password, uint16_t port) {
        session = ssh_new();
        if (!session) { err = "ssh: out of memory"; return false; }

        ssh_options_set(session, SSH_OPTIONS_HOST, host.c_str());
        unsigned int p = port;
        ssh_options_set(session, SSH_OPTIONS_PORT, &p);
        ssh_options_set(session, SSH_OPTIONS_USER, user.c_str());
        long timeout = 15; // seconds
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
        ssh_options_set(session, SSH_OPTIONS_COMPRESSION, "no");

        if (ssh_connect(session) != SSH_OK) {
            err = std::string("ssh: ") + ssh_get_error(session);
            cleanup();
            return false;
        }
        // Host key is intentionally not verified here.

        if (ssh_userauth_password(session, NULL, password.c_str()) != SSH_AUTH_SUCCESS) {
            err = std::string("ssh: auth failed: ") + ssh_get_error(session);
            cleanup();
            return false;
        }

        channel = ssh_channel_new(session);
        if (!channel || ssh_channel_open_session(channel) != SSH_OK) {
            err = "ssh: cannot open channel";
            cleanup();
            return false;
        }
        ssh_channel_request_pty(channel);
        ssh_channel_change_pty_size(channel, VISIBLE_COLS, VISIBLE_ROWS);
        if (ssh_channel_request_shell(channel) != SSH_OK) {
            err = "ssh: cannot start shell";
            cleanup();
            return false;
        }
        return true;
    }

    const std::string& error() const { return err; }

    void onConnected(const std::string& user, const std::string& host) {
        auto& d = M5Cardputer.Display;
        d.setFont(&fonts::efontCN_14);
        d.setTextSize(1);
        d.setTextColor(COL_TEXT);
        d.fillScreen(COL_BG);
        d.setTextScroll(true);
        d.setCursor(0, 0);
        outLines.clear(); curLine.clear(); scrollOffset = 0;
        char buf[96];
        snprintf(buf, sizeof(buf), "[ssh %s@%s  arrows<>:scroll ^Q:quit]", user.c_str(), host.c_str());
        putStr(buf); newLine();
    }

    // Read available output. Returns false when the session has closed.
    bool poll() {
        if (!channel || ssh_channel_is_closed(channel)) return false;
        char buf[256];
        for (int pass = 0; pass < 2; ++pass) {     // 0 = stdout, 1 = stderr
            int n = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), pass);
            if (n == SSH_ERROR) return false;
            for (int i = 0; i < n; ++i) putByte((uint8_t)buf[i]);
        }
        if (ssh_channel_is_eof(channel)) return false;
        return true;
    }

    void onChar(uint8_t c) {
        goLive();
        if (channel && ssh_channel_is_open(channel)) {
            char b = (char)c;
            ssh_channel_write(channel, &b, 1);
        }
    }
    void onEnter()      { uint8_t r = '\r'; if (channel) ssh_channel_write(channel, &r, 1); goLive(); }
    void onBackspace()  { uint8_t b = 0x7f; if (channel) ssh_channel_write(channel, &b, 1); goLive(); }
    void onCtrl(uint8_t ctrlByte) { if (channel) ssh_channel_write(channel, &ctrlByte, 1); goLive(); }
    void sendStr(const char* s) {
        if (channel && ssh_channel_is_open(channel)) ssh_channel_write(channel, s, strlen(s));
    }

    void scrollUp()   { int m = maxScroll(); if (scrollOffset < m) { scrollOffset++; renderScroll(); } }
    void scrollDown() { if (scrollOffset > 0) { scrollOffset--; if (scrollOffset == 0) renderLive(); else renderScroll(); } }

    void close() { cleanup(); }

private:
    ssh_session session = nullptr;
    ssh_channel channel = nullptr;
    std::string err;

    std::vector<std::string> outLines;
    std::string curLine;
    int scrollOffset = 0;
    int ansiState = 0;

    static const int CHAR_W = 7, LINE_H = 14;
    static const int VISIBLE_ROWS = 9, VISIBLE_COLS = 34;
    static const size_t MAX_LINES = 200, MAX_LINE_LEN = 512;
    static const uint16_t COL_BG = 0x0000, COL_TEXT = 0xFFFF, COL_BAR = 0xFB60;

    void cleanup() {
        if (channel) {
            if (ssh_channel_is_open(channel)) ssh_channel_close(channel);
            ssh_channel_free(channel);
            channel = nullptr;
        }
        if (session) {
            ssh_disconnect(session);
            ssh_free(session);
            session = nullptr;
        }
    }

    bool live() const { return scrollOffset == 0; }
    void pushLine() {
        outLines.push_back(curLine);
        if (outLines.size() > MAX_LINES) outLines.erase(outLines.begin());
        curLine.clear();
    }
    void putCh(char c) { if (curLine.size() >= (size_t)VISIBLE_COLS) pushLine(); curLine += c; if (live()) M5Cardputer.Display.print(c); }
    void putStr(const char* s) { for (const char* p = s; *p; ++p) putCh(*p); }
    void newLine() { pushLine(); if (live()) M5Cardputer.Display.println(); }

    void eraseBack() {
        if (!curLine.empty()) curLine.pop_back();
        if (live()) {
            auto& d = M5Cardputer.Display;
            int x = d.getCursorX(), y = d.getCursorY();
            if (x >= CHAR_W) { d.fillRect(x - CHAR_W, y, CHAR_W, LINE_H, COL_BG); d.setCursor(x - CHAR_W, y); }
        }
    }

    void putByte(uint8_t b) {
        if (ansiState == 1) { ansiState = (b == '[') ? 2 : 0; return; }
        if (ansiState == 2) { if (b >= 0x40 && b <= 0x7E) ansiState = 0; return; }
        if (b == 27) { ansiState = 1; return; }
        if (b == '\n') { newLine(); return; }
        if (b == '\r') return;
        if (b == 8 || b == 0x7f) { eraseBack(); return; } // server's destructive backspace
        if (b == '\t') putStr("  ");
        else if (b >= 32 && b <= 126) putCh((char)b);
    }

    int physRowsOf(const std::string& s) const {
        if (s.empty()) return 1;
        return (int)((s.size() + VISIBLE_COLS - 1) / VISIBLE_COLS);
    }
    int totalPhys() const {
        int t = 0;
        for (const auto& l : outLines) t += physRowsOf(l);
        if (!curLine.empty()) t += physRowsOf(curLine);
        return t;
    }
    int maxScroll() const { int m = totalPhys() - VISIBLE_ROWS; return m > 0 ? m : 0; }

    // Draw VISIBLE_ROWS physical (wrapped) rows ending 'offset' rows from the
    // bottom, splitting each logical line into VISIBLE_COLS-wide segments so
    // long lines show in full - same as live output.
    void renderWindow(int offset, bool liveTail) {
        auto& d = M5Cardputer.Display;
        d.setTextScroll(false); d.fillScreen(COL_BG); d.setCursor(0, 0);

        int total = totalPhys();
        int start = total - VISIBLE_ROWS - offset; if (start < 0) start = 0;

        int logicalCount = (int)outLines.size() + (curLine.empty() ? 0 : 1);
        int phys = 0, drawn = 0;
        for (int li = 0; li < logicalCount && drawn < VISIBLE_ROWS; ++li) {
            const std::string& L = (li < (int)outLines.size()) ? outLines[li] : curLine;
            int rows = physRowsOf(L);
            for (int seg = 0; seg < rows && drawn < VISIBLE_ROWS; ++seg) {
                if (phys >= start) {
                    d.print(L.substr((size_t)seg * VISIBLE_COLS, VISIBLE_COLS).c_str());
                    bool lastSeg = (li == logicalCount - 1) && (seg == rows - 1);
                    if (!(liveTail && lastSeg && !curLine.empty())) d.println();
                    drawn++;
                }
                phys++;
            }
        }
    }
    void renderScroll() {
        renderWindow(scrollOffset, false);
        M5Cardputer.Display.fillRect(0, 132, 240, 3, COL_BAR);
    }
    void renderLive() {
        renderWindow(0, true);
        M5Cardputer.Display.setTextScroll(true);
    }
    void goLive() { if (scrollOffset != 0) { scrollOffset = 0; renderLive(); } }
};
