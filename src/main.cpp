#include <M5Cardputer.h>
#include "Terminal.h"
#include "CommonCmds.h"
#include "Version.h"

// libssh (LibSSH-ESP32) one-time init helper.
#include "libssh_esp32.h"

// The SSH key exchange needs a deep stack, far more than the default 8 KB loop
// task. Raise the loop task stack so the ssh command (which runs in loop) has
// room. Costs ~50 KB of DRAM for the whole runtime (visible in `free`).
SET_LOOP_TASK_STACK_SIZE(51200);

Terminal* terminal;

void setup() {
    // Init M5 config
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    Serial.begin(115200);
    //while(!Serial);

    // Display setup
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);

    // Splash message
    M5Cardputer.Display.setFont(&fonts::efontCN_16);
    M5Cardputer.Display.setTextColor(WHITE);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(80, 55);
    M5Cardputer.Display.println(">hamsTerm");

    M5Cardputer.Display.setCursor(82, 120);
    M5Cardputer.Display.println("v. " HAMSTERM_VERSION);

    // FIX: was sleep(2). newlib's sleep() works in seconds and under
    // FreeRTOS its behavior is non-obvious; the Arduino-idiomatic call is delay().
    delay(2000);

    CommonCmds::begin();
    libssh_begin(); // initialize LibSSH-ESP32 once
    terminal = new Terminal();
}

void loop() {
    M5Cardputer.update();

    // ===== SSH mode: pump the channel every iteration + route input =====
    if (terminal->ssh_active()) {
        terminal->ssh_poll();

        if (terminal->ssh_active() &&
            M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto& kb = M5Cardputer.Keyboard;
            Keyboard_Class::KeysState status = kb.keysState();

            if (status.ctrl) {
                for (auto c : status.word) {
                    char lc = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
                    if (lc == 'q') { terminal->ssh_disconnect(); return; }
                    if (lc >= 'a' && lc <= 'z') { terminal->ssh_on_ctrl((uint8_t)(lc - 'a' + 1)); return; }
                }
            }
            if (kb.isKeyPressed(KEY_FN)) {
                if (kb.isKeyPressed(';')) { terminal->ssh_arrow("\x1b[A"); return; } // up    -> server
                if (kb.isKeyPressed('.')) { terminal->ssh_arrow("\x1b[B"); return; } // down  -> server
                if (kb.isKeyPressed(',')) { terminal->ssh_scroll_up();   return; }   // left  -> scroll
                if (kb.isKeyPressed('/')) { terminal->ssh_scroll_down(); return; }   // right -> scroll
                return;
            }
            if (status.del)        terminal->ssh_on_backspace();
            else if (status.enter) terminal->ssh_on_enter();
            else if (!status.word.empty()) {
                for (auto c : status.word) terminal->ssh_on_char((uint8_t)c);
            }
        }
        delay(5);
        return;
    }

    // ===== Telnet mode: poll the socket every iteration + feed session input =====
    if (terminal->telnet_active()) {
        terminal->telnet_poll(); // read the socket and draw to screen

        if (terminal->telnet_active() &&
            M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto& kb = M5Cardputer.Keyboard;
            Keyboard_Class::KeysState status = kb.keysState();

            // Ctrl: Ctrl+Q quits; other Ctrl+letter -> control byte (Ctrl+C etc.)
            if (status.ctrl) {
                for (auto c : status.word) {
                    char lc = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
                    if (lc == 'q') { terminal->telnet_disconnect(); return; }
                    if (lc >= 'a' && lc <= 'z') {
                        terminal->telnet_on_ctrl((uint8_t)(lc - 'a' + 1)); // Ctrl+A=1..Ctrl+Z=26
                        return;
                    }
                }
            }

            // Arrows: left/right scroll the screen (like the terminal);
            // up/down send ANSI cursor sequences to the server (for char mode).
            if (kb.isKeyPressed(KEY_FN)) {
                if (kb.isKeyPressed(';')) { terminal->telnet_arrow("\x1b[A"); return; } // up -> server
                if (kb.isKeyPressed('.')) { terminal->telnet_arrow("\x1b[B"); return; } // down -> server
                if (kb.isKeyPressed(',')) { terminal->telnet_scroll_up();   return; }   // left -> scroll up
                if (kb.isKeyPressed('/')) { terminal->telnet_scroll_down(); return; }   // right -> scroll down
                return;
            }

            if (status.del) {
                terminal->telnet_on_backspace();
            } else if (status.enter) {
                terminal->telnet_on_enter();
            } else if (!status.word.empty()) {
                for (auto c : status.word) terminal->telnet_on_char((uint8_t)c);
            }
        }
        delay(5);
        return;
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {

        // ===== Editor mode: input goes to the editor =====
        if (terminal->editor_active()) {
            auto& kb = M5Cardputer.Keyboard;
            Keyboard_Class::KeysState status = kb.keysState();

            // Ctrl shortcuts: save / save+exit / quit without saving
            if (status.ctrl) {
                for (auto c : status.word) {
                    char lc = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
                    if (lc == 's') { terminal->editor_key(Editor::K_SAVE);     return; }
                    if (lc == 'x') { terminal->editor_key(Editor::K_SAVEEXIT); return; }
                    if (lc == 'q') { terminal->editor_key(Editor::K_QUIT);     return; }
                }
            }

            // Arrows (Fn + ; . , /)
            if (kb.isKeyPressed(KEY_FN)) {
                if (kb.isKeyPressed(';')) { terminal->editor_key(Editor::K_UP);    return; }
                if (kb.isKeyPressed('.')) { terminal->editor_key(Editor::K_DOWN);  return; }
                if (kb.isKeyPressed(',')) { terminal->editor_key(Editor::K_LEFT);  return; }
                if (kb.isKeyPressed('/')) { terminal->editor_key(Editor::K_RIGHT); return; }
                return; // ignore other Fn combos in the editor
            }

            if (status.del) {
                terminal->editor_key(8);
            } else if (status.enter) {
                terminal->editor_key(13);
            } else if (!status.word.empty()) {
                for (auto c : status.word) {
                    terminal->editor_key((unsigned char)c);
                }
            }
            delay(10);
            return;
        }

        // ===== Normal terminal mode =====
        // 1. First check the physical arrow keys (Fn + ; . , /)
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_FN)) {
            // Cardputer 'arrow' layout:
            //   Fn + ; = UP, Fn + . = DOWN, Fn + , = LEFT, Fn + / = RIGHT
            //
            // UP/DOWN -> command history (bash-like).
            // LEFT/RIGHT -> screen scrolling.
            // (To put scrolling back on UP/DOWN, swap the codes:
            //  181/182 = scroll, 183/184 = history.)
            if (M5Cardputer.Keyboard.isKeyPressed(';')) {
                terminal->handle_keypress(183); // history back (previous)
                return;
            }
            if (M5Cardputer.Keyboard.isKeyPressed('.')) {
                terminal->handle_keypress(184); // history forward (next)
                return;
            }
            if (M5Cardputer.Keyboard.isKeyPressed(',')) {
                terminal->handle_keypress(181); // scroll up
                return;
            }
            if (M5Cardputer.Keyboard.isKeyPressed('/')) {
                terminal->handle_keypress(182); // scroll down
                return;
            }
        }

        // 2. Not an arrow key -> use the standard key state
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

        if (status.tab) { // Tab pressed
            terminal->handle_tab();
            return; // skip remaining handling
        }

        if (status.del) {
            terminal->handle_keypress(8);
        } else if (status.enter) {
            terminal->handle_keypress(13);
        } else if (!status.word.empty()) {
            for (auto c : status.word) {
                terminal->handle_keypress(c);
            }
        }
    }
    delay(10);
}
