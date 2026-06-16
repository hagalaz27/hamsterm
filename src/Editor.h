#pragma once

#include <M5Cardputer.h>
#include <vector>
#include <string>
#include "Helpers.h"

// Simple full-screen text editor for the Cardputer.
// Model: an in-memory line buffer. Key codes are delivered from main.cpp.
class Editor {
public:
    // Key codes sent by main.cpp while in editor mode
    static const int K_UP = 200, K_DOWN = 201, K_LEFT = 202, K_RIGHT = 203;
    static const int K_SAVE = 210, K_SAVEEXIT = 211, K_QUIT = 212;

    explicit Editor(const std::string& path) : filePath(path) {
        loadOk = load();
    }

    bool ok() const { return loadOk; }
    const std::string& error() const { return loadError; }

    // Handle a key. Returns true when the editor should close.
    bool handleKey(int key) {
        statusMsg.clear();

        switch (key) {
            case K_SAVE:     save();         break;
            case K_SAVEEXIT: save();         return true;
            case K_QUIT:                     return true;
            case K_UP:       moveUp();       break;
            case K_DOWN:     moveDown();     break;
            case K_LEFT:     moveLeft();     break;
            case K_RIGHT:    moveRight();    break;
            case 8:          backspace();    break;
            case 13:
            case 10:         newline();      break;
            default:
                if (key >= 32 && key <= 126) insertChar((char)key);
                break;
        }

        adjustScroll();
        render();
        return false;
    }

    void render() {
        auto& d = M5Cardputer.Display;
        d.fillScreen(COL_BG);
        d.setFont(&fonts::efontCN_14);
        d.setTextSize(1);

        // --- top status bar ---
        d.fillRect(0, 0, 240, 14, COL_BAR);
        d.setTextColor(COL_FG, COL_BAR);
        d.setCursor(2, 1);
        std::string nm = baseName(filePath);
        if (nm.size() > 14) nm = nm.substr(0, 14);
        char st[48];
        snprintf(st, sizeof(st), "%s%s L%d:%d",
                 nm.c_str(), modified ? "*" : " ", cursorRow + 1, cursorCol + 1);
        d.print(st);

        // --- content ---
        d.setTextColor(COL_FG, COL_BG);
        for (int r = 0; r < VISIBLE_ROWS; r++) {
            int li = topRow + r;
            if (li >= (int)lines.size()) break;
            const std::string& ln = lines[li];
            std::string vis;
            if (leftCol < (int)ln.size()) vis = ln.substr(leftCol, VISIBLE_COLS);
            d.setCursor(0, CONTENT_Y0 + r * LINE_H);
            d.print(vis.c_str());
        }

        // --- cursor (caret) ---
        int cx = (cursorCol - leftCol) * CHAR_W;
        int cy = CONTENT_Y0 + (cursorRow - topRow) * LINE_H;
        d.fillRect(cx, cy, 2, LINE_H, COL_CURSOR);

        // --- bottom hint bar ---
        d.fillRect(0, HINT_Y, 240, 14, COL_BAR);
        d.setTextColor(COL_FG, COL_BAR);
        d.setCursor(2, HINT_Y + 1);
        if (!statusMsg.empty()) d.print(statusMsg.c_str());
        else d.print("^S save ^X exit ^Q quit");
    }

private:
    std::string filePath;
    std::vector<std::string> lines;
    int cursorRow = 0, cursorCol = 0;
    int topRow = 0, leftCol = 0;
    bool modified = false;
    bool loadOk = true;
    std::string loadError;
    std::string statusMsg;

    static const int CHAR_W = 7, LINE_H = 14;
    static const int VISIBLE_ROWS = 7, VISIBLE_COLS = 34;
    static const int CONTENT_Y0 = 15, HINT_Y = 119;

    // Colors (numeric RGB565 - no dependency on macro names)
    static const uint16_t COL_BG = 0x0000;     // black
    static const uint16_t COL_FG = 0xFFFF;     // white
    static const uint16_t COL_BAR = 0x000F;    // navy
    static const uint16_t COL_CURSOR = 0x07E0; // green

    static std::string baseName(const std::string& p) {
        size_t s = p.find_last_of('/');
        return (s == std::string::npos) ? p : p.substr(s + 1);
    }

    bool load() {
        lines.clear();
        File f = Helpers::fsOpen(filePath, "r");
        if (!f) {                       // new file does not exist yet - empty buffer
            lines.push_back("");
            return true;
        }
        if (f.isDirectory()) {
            f.close();
            loadError = "edit: " + baseName(filePath) + ": is a directory";
            return false;
        }

        // The file must fit entirely in memory: otherwise saving
        // would TRUNCATE the original. So we refuse to open large files.
        const size_t MAXB = 24576; // 24 KB
        const size_t MAXL = 600;
        if (f.size() > MAXB) {
            f.close();
            loadError = "edit: file too large (max 24KB)";
            return false;
        }

        std::string line;
        while (f.available()) {
            char c = (char)f.read();
            if (c == '\r') continue;
            if (c == '\n') {
                lines.push_back(line);
                line.clear();
                if (lines.size() > MAXL) {
                    f.close();
                    loadError = "edit: too many lines (max 600)";
                    return false;
                }
            } else {
                line += c;
            }
        }
        if (!line.empty()) lines.push_back(line);
        f.close();
        if (lines.empty()) lines.push_back("");
        return true;
    }

    void save() {
        File f = Helpers::fsOpen(filePath, "w");
        if (!f) {
            statusMsg = "save FAILED";
            return;
        }
        for (size_t i = 0; i < lines.size(); i++) {
            f.print(lines[i].c_str());
            f.print("\n");
        }
        f.close();
        modified = false;
        statusMsg = "saved";
    }

    // --- editing ---
    void insertChar(char c) {
        lines[cursorRow].insert(cursorCol, 1, c);
        cursorCol++;
        modified = true;
    }

    void backspace() {
        if (cursorCol > 0) {
            lines[cursorRow].erase(cursorCol - 1, 1);
            cursorCol--;
            modified = true;
        } else if (cursorRow > 0) {
            cursorCol = (int)lines[cursorRow - 1].size();
            lines[cursorRow - 1] += lines[cursorRow];
            lines.erase(lines.begin() + cursorRow);
            cursorRow--;
            modified = true;
        }
    }

    void newline() {
        std::string rest = lines[cursorRow].substr(cursorCol);
        lines[cursorRow].erase(cursorCol);
        lines.insert(lines.begin() + cursorRow + 1, rest);
        cursorRow++;
        cursorCol = 0;
        modified = true;
    }

    // --- cursor movement ---
    void clampCol() {
        if (cursorCol > (int)lines[cursorRow].size())
            cursorCol = (int)lines[cursorRow].size();
    }
    void moveUp()    { if (cursorRow > 0) { cursorRow--; clampCol(); } }
    void moveDown()  { if (cursorRow < (int)lines.size() - 1) { cursorRow++; clampCol(); } }
    void moveLeft()  {
        if (cursorCol > 0) cursorCol--;
        else if (cursorRow > 0) { cursorRow--; cursorCol = (int)lines[cursorRow].size(); }
    }
    void moveRight() {
        if (cursorCol < (int)lines[cursorRow].size()) cursorCol++;
        else if (cursorRow < (int)lines.size() - 1) { cursorRow++; cursorCol = 0; }
    }

    void adjustScroll() {
        if (cursorRow < topRow) topRow = cursorRow;
        if (cursorRow >= topRow + VISIBLE_ROWS) topRow = cursorRow - VISIBLE_ROWS + 1;
        if (cursorCol < leftCol) leftCol = cursorCol;
        if (cursorCol >= leftCol + VISIBLE_COLS) leftCol = cursorCol - VISIBLE_COLS + 1;
    }
};
