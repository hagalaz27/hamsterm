#pragma once

#include <M5Cardputer.h>
#include <WiFi.h>
#include <vector>
#include <string>

// Interactive telnet / raw-TCP client (modal) with screen scrollback.
//
// Output goes both to the screen (incrementally in live mode) and into the
// outLines buffer, so you can scroll back and see what was above.
//
// Scroll keys come from main.cpp: left = up, right = down.
class TelnetSession {
public:
    explicit TelnetSession(bool rawMode)
        : raw(rawMode) {}

    bool begin(const std::string& host, uint16_t port) {
        if (!client.connect(host.c_str(), port, 5000)) return false;
        client.setNoDelay(true);
        return true;
    }

    void onConnected(const std::string& host, uint16_t port) {
        auto& d = M5Cardputer.Display;
        d.setFont(&fonts::efontCN_14);
        d.setTextSize(1);
        d.setTextColor(COL_TEXT);
        d.fillScreen(COL_BG);
        d.setTextScroll(true);
        d.setCursor(0, 0);
        outLines.clear();
        curLine.clear();
        scrollOffset = 0;
        serverLineStart = true;
        char buf[80];
        snprintf(buf, sizeof(buf), "[connected %s:%u  arrows<>:scroll ^Q:quit]", host.c_str(), port);
        putStr(buf);
        newLine();
    }

    bool connected() { return client.connected() || client.available(); }

    bool poll() {
        if (!client.connected() && !client.available()) return false;
        int n = 0;
        while (client.available() && n < 512) {
            int b = client.read();
            if (b < 0) break;
            processIncoming((uint8_t)b);
            n++;
        }
        return client.connected() || client.available();
    }

    // ---- user input ----
    void onChar(uint8_t c) {
        if (c < 32 || c > 126) return;
        goLive();
        if (lineMode()) { ensureClientLabel(); lineBuf += (char)c; putCh((char)c); }
        else client.write(c);
    }
    void onBackspace() {
        goLive();
        if (lineMode()) {
            if (!lineBuf.empty()) { lineBuf.pop_back(); eraseChar(); }
        } else client.write((uint8_t)0x7f);
    }
    void onEnter() {
        goLive();
        if (lineMode()) {
            std::string out = lineBuf + "\r\n";
            client.write((const uint8_t*)out.c_str(), out.size());
            newLine();
            lineBuf.clear();
            clientLabelShown = false;
            serverLineStart = true;
        } else {
            const uint8_t crlf[2] = {'\r', '\n'};
            client.write(crlf, 2);
        }
    }
    void onCtrl(uint8_t ctrlByte) {
        if (!client.connected()) return;
        goLive();
        client.write(ctrlByte);
        if (lineMode() && ctrlByte == 3) {
            lineBuf.clear(); clientLabelShown = false; newLine(); serverLineStart = true;
        }
    }
    void sendArrow(const char* seq) {
        if (!lineMode() && client.connected())
            client.write((const uint8_t*)seq, strlen(seq));
    }

    // ---- screen scrolling ----
    void scrollUp()   { int m = maxScroll(); if (scrollOffset < m) { scrollOffset++; renderScroll(); } }
    void scrollDown() { if (scrollOffset > 0) { scrollOffset--; if (scrollOffset == 0) renderLive(); else renderScroll(); } }

    void close() { client.stop(); }

private:
    WiFiClient client;
    bool raw;
    bool remoteEcho = false;
    bool lineMode() const { return !remoteEcho; }

    std::string lineBuf;            // what will be sent to the server (line mode)
    std::vector<std::string> outLines; // scrollback line buffer
    std::string curLine;            // current (unfinished) line
    int scrollOffset = 0;           // 0 = bottom (live mode)
    bool clientLabelShown = false;
    bool serverLineStart = true;

    enum { S_DATA, S_IAC, S_OPT, S_SB, S_SB_IAC };
    int tState = S_DATA; uint8_t tCmd = 0; int ansiState = 0;

    static const int CHAR_W = 7, LINE_H = 14;
    static const int VISIBLE_ROWS = 9, VISIBLE_COLS = 34;
    static const size_t MAX_LINES = 200, MAX_LINE_LEN = 512;
    static const uint16_t COL_BG = 0x0000, COL_TEXT = 0xFFFF;
    static const uint16_t COL_BAR = 0xFB60;

    bool live() const { return scrollOffset == 0; }

    void pushLine() {
        outLines.push_back(curLine);
        if (outLines.size() > MAX_LINES) outLines.erase(outLines.begin());
        curLine.clear();
    }

    void putCh(char c) {
        // Soft-wrap the scrollback at the screen width, matching the hardware
        // wrap of live output, so a scrolled long line looks exactly like it
        // did live (instead of being truncated to one row).
        if (curLine.size() >= (size_t)VISIBLE_COLS) pushLine();
        curLine += c;
        if (live()) M5Cardputer.Display.print(c);
    }
    void putStr(const char* s) { for (const char* p = s; *p; ++p) putCh(*p); }

    void newLine() {
        pushLine();
        if (live()) M5Cardputer.Display.println();
    }
    void newlineIfNeeded() { if (!curLine.empty()) newLine(); }

    // Start of a client (locally-edited) line: just make sure we are on a
    // fresh screen line, so input and server output don't run together.
    void ensureClientLabel() {
        if (clientLabelShown) return;
        newlineIfNeeded();
        clientLabelShown = true;
    }
    void eraseChar() {
        if (!curLine.empty()) curLine.pop_back();
        if (live()) {
            auto& d = M5Cardputer.Display;
            int x = d.getCursorX(), y = d.getCursorY();
            if (x >= CHAR_W) { d.fillRect(x - CHAR_W, y, CHAR_W, LINE_H, COL_BG); d.setCursor(x - CHAR_W, y); }
        }
    }

    // ---- scrollback rendering (wraps long lines, same as live output) ----
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
    // bottom. Each logical line is split into VISIBLE_COLS-wide segments, so a
    // long line shows in full across several rows - exactly like live output.
    // When liveTail is set, the last segment of curLine gets no trailing
    // newline so live (hardware-scrolled) printing continues from there.
    void renderWindow(int offset, bool liveTail) {
        auto& d = M5Cardputer.Display;
        d.setTextScroll(false);
        d.fillScreen(COL_BG);
        d.setCursor(0, 0);

        int total = totalPhys();
        int start = total - VISIBLE_ROWS - offset;
        if (start < 0) start = 0;

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
        M5Cardputer.Display.fillRect(0, 132, 240, 3, COL_BAR); // 'scrolled' indicator
    }
    void renderLive() {
        renderWindow(0, true);
        M5Cardputer.Display.setTextScroll(true); // resume live (hardware) scrolling
    }
    void goLive() { if (scrollOffset != 0) { scrollOffset = 0; renderLive(); } }

    // ---- incoming parsing (IAC + ANSI) ----
    void reply(uint8_t verb, uint8_t option) { uint8_t b[3] = {255, verb, option}; client.write(b, 3); }
    void handleOption(uint8_t cmd, uint8_t opt) {
        if (opt == 1) {
            if (cmd == 251) { reply(253, 1); remoteEcho = true; }
            else if (cmd == 252) { reply(254, 1); remoteEcho = false; }
            else if (cmd == 253) reply(252, 1);
            else if (cmd == 254) reply(252, 1);
        } else if (opt == 3) {
            if (cmd == 251) reply(253, 3);
            else if (cmd == 253) reply(251, 3);
            else if (cmd == 252) reply(254, 3);
            else if (cmd == 254) reply(252, 3);
        } else {
            if (cmd == 251) reply(254, opt);
            else if (cmd == 253) reply(252, opt);
        }
    }
    void processIncoming(uint8_t b) {
        if (raw) { putByte(b); return; }
        switch (tState) {
            case S_DATA: if (b == 255) tState = S_IAC; else putByte(b); break;
            case S_IAC:
                if (b == 255) { putByte(255); tState = S_DATA; }
                else if (b >= 251 && b <= 254) { tCmd = b; tState = S_OPT; }
                else if (b == 250) tState = S_SB;
                else tState = S_DATA;
                break;
            case S_OPT: handleOption(tCmd, b); tState = S_DATA; break;
            case S_SB: if (b == 255) tState = S_SB_IAC; break;
            case S_SB_IAC: tState = (b == 240) ? S_DATA : S_SB; break;
        }
    }
    void putByte(uint8_t b) {
        if (ansiState == 1) { ansiState = (b == '[') ? 2 : 0; return; }
        if (ansiState == 2) { if (b >= 0x40 && b <= 0x7E) ansiState = 0; return; }
        if (b == 27) { ansiState = 1; return; }
        if (b == '\n') { newLine(); serverLineStart = true; return; }
        if (b == '\r') return;
        if (b == 8 || b == 0x7f) { eraseChar(); return; } // server's destructive backspace (echoing/char-mode servers)
        if (serverLineStart) {
            newlineIfNeeded();
            serverLineStart = false;
        }
        if (b == '\t') putStr("  ");
        else if (b >= 32 && b <= 126) putCh((char)b);
    }
};
