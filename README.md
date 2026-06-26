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
- **Web server / file manager:** `httpd start [path]` serves a built-in file
  manager on port 80, reachable on both the SoftAP IP and the Wi-Fi IP. From a
  browser you can browse, download, **upload**, **edit text files**, **rename**,
  **delete** (folders recursively), and **create folders** on internal flash and
  `/sd`. Pass a `path` to expose only that subtree (e.g. `httpd start /docs`) -
  access is confined to it. `httpd stop` / `httpd status` to manage it.
- **Startup script:** commands in `/.profile` run automatically at boot, one per
  line, exactly as if typed (`#` comments and blank lines are ignored). Lives on
  internal flash, so it's always available. Edit with `ed /.profile` and reboot to
  apply. Handy for setting variables, `cd`-ing to a working dir, connecting to
  Wi-Fi (`wf c <ssid>` then the password on the next line), or printing a banner.

Type `help` on the device for the full list.

## Keys

The Cardputer's arrow keys are `Fn` + `;` `.` `,` `/` (up / down / left / right).

| Key | Action |
| --- | --- |
| Up / Down (`Fn`+`;` / `Fn`+`.`) | Command history: previous / next |
| Left / Right (`Fn`+`,` / `Fn`+`/`) | Move the edit cursor within the line |
| `Ctrl`+`;` / `Ctrl`+`.` | Scroll the output up / down |
| `Tab` | Autocomplete a name from the last `ls` |

In the **editor**, the arrows move the cursor and `Ctrl`+`S` / `Ctrl`+`X` / `Ctrl`+`Q`
save / save&exit / quit. In **telnet / SSH** sessions, `Ctrl`+`;` / `Ctrl`+`.` scroll
the local screen back, the `Fn` arrows are sent to the remote host, `Ctrl`+`Q`
disconnects, and other `Ctrl`+letter combos send control bytes (e.g. `Ctrl`+`C`).

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
