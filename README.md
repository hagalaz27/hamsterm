# hamsTerm

A Linux-like terminal firmware for the **M5Stack Cardputer** (ESP32-S3). It gives
the handheld a real shell: a filesystem on internal flash and microSD, Wi-Fi and
network tools, a full-screen text editor, interactive telnet/nc and SSH clients,
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
- **Downloads:** `wget <url> [-o path]` - fetch a file over HTTP/HTTPS (streamed to flash or `/sd`)
- **Access point:** host a Wi-Fi AP with `ap -s ssid [-p pass] start` (open or WPA2)
- **telnet / nc:** interactive client with local line editing, ANSI handling
  and screen scrollback
- **SSH:** password-based SSH client (LibSSH-ESP32) for a remote shell
- **Editor:** `edit` / `ed` - full-screen editor with cursor keys and Ctrl shortcuts
- **System:** `help`, `sysinfo`, `free`, `df`, `battery`, `reboot`

Type `help` on the device for the full list.

## Hardware
M5Stack Cardputer - ESP32-S3 (Wi-Fi + BLE), ~512 KB SRAM (no PSRAM), 8 MB flash,
240x135 display, 56-key keyboard, microSD, IR LED, mic and speaker.

## Install

Flash it straight from your browser - no toolchain, no command line:

### → [hamsterm.com](https://hamsterm.com/)

Connect the Cardputer over USB-C, open the site in Chrome or Edge, and click flash.
That's it - the device reboots into hamsTerm when it's done.

<details>
<summary>Building from source (for contributors)</summary>

With [PlatformIO](https://platformio.org/):

```bash
pio run -t upload      # build and flash over USB-C
pio device monitor     # open the serial monitor
```

The build environment is `m5stack-cardputer`; artifacts land in
`.pio/build/m5stack-cardputer/`.
</details>

## Project layout
```
src/            firmware sources (header-per-module + .cpp)
platformio.ini  build configuration
```

## License
Released under the MIT License. See [LICENSE](LICENSE).
