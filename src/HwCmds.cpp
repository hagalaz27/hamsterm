#include "HwCmds.h"
#include "Helpers.h"
#include <M5Cardputer.h>

// Drive the WS2812 through the ESP core's built-in RMT driver rather than
// FastLED's own interrupt-based one (which drops frames on the ESP32-S3 while
// WiFi runs). These defines MUST come before <FastLED.h>.
#define FASTLED_RMT_BUILTIN_DRIVER   1
#define FASTLED_RMT_MAX_CHANNELS     1
#define FASTLED_ESP32_RMT_CHANNEL_0  0
#include <FastLED.h>

// The StampS3 module inside the Cardputer has a single WS2812 RGB LED on GPIO21.
// Change HW_LED_PIN if your unit differs.
//
// IMPORTANT: on the Cardputer the LED and the display share the same power rail
// (GPIO38). The WS2812 only gets enough power to light when the display
// backlight is at full brightness, so we raise the display to 255 while the LED
// is lit and drop it back to the default (127) once the LED is off.
#define HW_LED_PIN     21
#define HW_LED_COUNT   1
#define BEEP_FREQ_HZ   4000
#define DISP_FULL      255
#define DISP_DEFAULT   127

namespace {

CRGB g_leds[HW_LED_COUNT];
bool g_ledInit = false;
bool g_spkInit = false;

void ledEnsure() {
    if (!g_ledInit) {
        FastLED.addLeds<WS2812, HW_LED_PIN, GRB>(g_leds, HW_LED_COUNT);
        FastLED.setBrightness(50); // full is blinding; keep the LED itself gentle
        g_leds[0] = CRGB::Black;
        FastLED.show();
        g_ledInit = true;
    }
}

// Show a colour. Raising the display backlight to full first is what actually
// gives the LED enough power to light (shared rail); dark == let it drop back.
void ledShow(uint8_t r, uint8_t g, uint8_t b) {
    ledEnsure();
    bool lit = (r || g || b);
    if (lit) M5Cardputer.Display.setBrightness(DISP_FULL);
    delay(10);
    g_leds[0] = CRGB(r, g, b);
    FastLED.show();
}

void ledOffAndRestore() {
    ledEnsure();
    g_leds[0] = CRGB::Black;
    FastLED.show();
    M5Cardputer.Display.setBrightness(DISP_DEFAULT);
}

bool g_spkInited = false;
void spkEnsure() {
    if (!g_spkInited) {
        M5Cardputer.Speaker.begin();
        M5Cardputer.Speaker.setVolume(80);
        g_spkInited = true;
    }
}

bool hwBreakPressed() {
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

bool waitMs(uint32_t ms) {
    uint32_t done = 0;
    while (done < ms) {
        if (hwBreakPressed()) return true;
        uint32_t step = ms - done; if (step > 20) step = 20;
        delay(step);
        done += step;
    }
    return false;
}

bool colorRgb(const std::string& in, uint8_t& r, uint8_t& g, uint8_t& b) {
    std::string s;
    for (char c : in) s += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    if (s == "r" || s == "red")     { r = 255; g = 0;   b = 0;   return true; }
    if (s == "g" || s == "green")   { r = 0;   g = 255; b = 0;   return true; }
    if (s == "b" || s == "blue")    { r = 0;   g = 0;   b = 255; return true; }
    if (s == "y" || s == "yellow")  { r = 255; g = 255; b = 0;   return true; }
    if (s == "c" || s == "cyan")    { r = 0;   g = 255; b = 255; return true; }
    if (s == "m" || s == "magenta") { r = 255; g = 0;   b = 255; return true; }
    if (s == "w" || s == "white")   { r = 255; g = 255; b = 255; return true; }
    if (s == "off" || s == "0" || s == "none") { r = 0; g = 0; b = 0; return true; }
    return false;
}

} // namespace

void HwCmds::beep(uint16_t count, uint16_t durMs, uint16_t gapMs, LineCallback emit) {
    spkEnsure();
    bool interrupted = false;
    for (uint16_t i = 0; i < count; ++i) {
        M5Cardputer.Speaker.tone(BEEP_FREQ_HZ, durMs);
        if (waitMs(durMs)) { interrupted = true; break; }
        M5Cardputer.Speaker.stop();
        if (i + 1 < count) {
            if (waitMs(gapMs)) { interrupted = true; break; }
        }
    }
    M5Cardputer.Speaker.stop();
    if (interrupted) { emit("^C\n"); Helpers::cmd_status = 130; }
    else Helpers::cmd_status = 0;
}

void HwCmds::led_set(const std::string& color, LineCallback emit) {
    uint8_t r, g, b;
    if (!colorRgb(color, r, g, b)) {
        emit("led: unknown color: " + color + "\n");
        Helpers::cmd_status = 1;
        return;
    }
    if (r || g || b) ledShow(r, g, b);   // hold colour, keep display bright
    else             ledOffAndRestore(); // "led off" -> dark + restore brightness
    Helpers::cmd_status = 0;
}

void HwCmds::led_blink(const std::string& color, uint16_t count,
                       uint16_t durMs, uint16_t gapMs, LineCallback emit) {
    uint8_t r, g, b;
    if (!colorRgb(color, r, g, b)) {
        emit("led: unknown color: " + color + "\n");
        Helpers::cmd_status = 1;
        return;
    }
    bool interrupted = false;
    for (uint16_t i = 0; i < count; ++i) {
        ledShow(r, g, b);
        if (waitMs(durMs)) { interrupted = true; break; }
        g_leds[0] = CRGB::Black; FastLED.show(); // brief off, keep display bright
        if (i + 1 < count) {
            if (waitMs(gapMs)) { interrupted = true; break; }
        }
    }
    ledOffAndRestore(); // always end dark + restore display brightness
    if (interrupted) { emit("^C\n"); Helpers::cmd_status = 130; }
    else Helpers::cmd_status = 0;
}
