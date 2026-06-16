# hamsTerm

A Linux-like terminal firmware for the **M5Stack Cardputer** (ESP32-S3). It gives
the handheld a real shell: a filesystem on internal flash and microSD, Wi-Fi and
network tools, a full-screen text editor, an interactive telnet/nc client,
shell-style globbing, and user variables - all driven from the device keyboard.

## Features
- **Files:** `ls`, `cd`, `cat`, `cp -r`, `mv`, `rm -r`, `mkdir`, `touch`,
  `head`, `tail`, `find`, hidden (dot) files
- **Globbing:** `*` and `?` expand in every command (e.g. `rm *.log`, `cat *.md`)
- **Variables:** `set NAME value` / `NAME=value`, expand as `$NAME` / `${NAME}`,
  persisted across reboots in `/.environment`
- **Pipes & redirection:** `>`, `>>`, `| grep`
- **Storage:** mount/unmount microSD at `/sd`, `df`
- **Wi-Fi & network:** scan, connect, `ping`, local network + port scan
- **telnet / nc:** interactive client with line editing, `[c]`/`[s]` labels,
  ANSI handling and screen scrollback
- **Editor:** `edit` / `ed` - full-screen editor with cursor keys and Ctrl shortcuts
- **System:** `help`, `sysinfo`, `free`, `df`, `battery`

Type `help` on the device for the full list.

## Hardware
M5Stack Cardputer - ESP32-S3 (Wi-Fi + BLE), ~512 KB SRAM (no PSRAM), 8 MB flash,
240x135 display, 56-key keyboard, microSD, IR LED, mic and speaker.

## Build & flash (PlatformIO)
```bash
pio run -t upload      # build and flash over USB-C
pio device monitor     # open the serial monitor
```
The build environment is `m5stack-cardputer`; artifacts land in
`.pio/build/m5stack-cardputer/`.

To produce a single image for a browser-based installer (ESP Web Tools):
```bash
esptool --chip esp32s3 merge_bin -o hamsterm.bin \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0     .pio/build/m5stack-cardputer/bootloader.bin \
  0x8000  .pio/build/m5stack-cardputer/partitions.bin \
  0xe000  ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 .pio/build/m5stack-cardputer/firmware.bin
```

## Project layout
```
src/            firmware sources (header-per-module + .cpp)
platformio.ini  build configuration
```

## License
Released under the MIT License. See [LICENSE](LICENSE).
