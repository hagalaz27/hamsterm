#pragma once

#include <string>
#include <functional>
#include <stdint.h>

using LineCallback = std::function<void(const std::string&)>;

// Hardware indicators: the built-in speaker (beep) and RGB LED (led). Both
// blink/beep loops are blocking but can be stopped with Ctrl+C. The LED also
// has a "hold" mode (led <color> with no counts) that returns immediately.
class HwCmds {
public:
    // Beep `count` times at ~4 kHz: each tone `durMs`, `gapMs` of silence
    // between them. Blocking; Ctrl+C stops early.
    static void beep(uint16_t count, uint16_t durMs, uint16_t gapMs, LineCallback emit);

    // Set the LED to a colour and hold it (or turn it off with "off"). Returns
    // immediately - use this for status indication.
    static void led_set(const std::string& color, LineCallback emit);

    // Blink the LED `count` times (on `durMs`, off `gapMs`), then leave it off.
    // Blocking; Ctrl+C stops early.
    static void led_blink(const std::string& color, uint16_t count,
                          uint16_t durMs, uint16_t gapMs, LineCallback emit);
};
