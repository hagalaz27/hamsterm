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
- **Conditions & if:** `test EXPR` / `[ EXPR ]` set `$?` for use with `&&`/`||`
  and `if`: file tests `-e` `-f` `-d` `-s`, string tests `-z` `-n` `=` `!=`,
  numeric `-eq` `-ne` `-lt` `-le` `-gt` `-ge`, and a leading `!` to negate -
  e.g. `[ -f /config ] && sh /setup.sh`
- **if / elif / else / fi:** conditional blocks in scripts (and as a single line
  at the prompt: `if [ -d /sd ]; then ls /sd; else echo no SD; fi`). The condition
  is any command - its exit status picks the branch. Blocks nest, and `elif`
  chains alternatives.
- **Loops:** `while C; do ...; done` (repeat while C is true), `until C; do ...;
  done` (until C becomes true), and `for x in LIST; do ...; done` (LIST is words,
  with `$vars`, numeric ranges `{1..5}` / `{0..10..2}`, and `*` globs expanded).
  Loops nest with each other and with `if`. Press **Ctrl+C** to break a running
  loop.
- **Brace ranges:** `{N..M}` and `{N..M..STEP}` expand to a numeric sequence
  anywhere on the line (`echo {1..5}`, `touch f{1..3}.txt`, counts down with
  `{5..1}`). Expanded after variables, so `{1..$n}` works too. Non-numeric or
  oversized ranges are left as-is.
- **Arithmetic:** `$(( EXPR ))` evaluates integer math with `+ - * / %`,
  parentheses, unary minus and variables (`$((i+1))`, `$(((a+b)*2))`). Inside the
  parentheses a bare name means that variable. Enables counters in loops:
  `i=$((i+1))`. Division/modulo by zero yields 0.
- **Command substitution:** `$( ... )` captures a command's output (trailing
  newlines trimmed) for use in another command or a variable: `today=$(date)`,
  `for f in $(ls /sd); do ...`. Pipes, redirection and nesting work inside; output
  is capped at 4 KB and Ctrl+C aborts a running script.
- **Interactive input:** `read [-p prompt] name...` pauses for a typed line and
  stores it (multiple names split the input by words, the last gets the rest) -
  the basis for prompts, menus and confirmations in scripts. Ctrl+C cancels.
- **Pipes, redirection & logic:** `>`, `>>`, `| grep`, and `&&` / `||` for
  conditional chains (`cmd1 && cmd2` runs the second only if the first succeeded;
  `cmd1 || cmd2` only if it failed) - works at the prompt and in scripts
- **Storage:** mount/unmount microSD at `/sd`, `df`
- **Wi-Fi & network:** scan, connect, `ping`, local network + port scan
  (`net s p <host> [ports]`, defaults to the top 1000; open ports are labelled
  with their likely service)
- **Air monitor:** `air s [seconds]` - passive 802.11 sniffer that hops the
  channels and reports access points, their clients, and probing stations; and
  `air w <ssid|bssid>` - live view of who joins/leaves a single AP
  (read-only; drops the Wi-Fi link while running)
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
  **delete** (folders recursively), and **create files and folders** on internal flash and
  `/sd`. Pass a `path` to expose only that subtree (e.g. `httpd start /docs`) -
  access is confined to it. `httpd stop` / `httpd status` to manage it.
- **Scripts:** `sh [-v] <file> [args...]` runs a script - each line executed as a
  command (`#` comments and blank lines ignored). By default only the commands'
  output is shown; `-v` also echoes each command before it runs (like bash's `-v`).
  Arguments after the file become positional parameters inside the script: `$1`,
  `$2`, ... `$9` (use `${10}` and up), `$@` for all of them, `$#` for the count,
  and `$0` for the script name. `$?` holds the exit status of the last command -
  every command reports success (`0`) or failure (non-zero): a missing file,
  a bad path, a failed `cd`, a download or Wi-Fi error all set a non-zero status,
  and an unknown command gives `127`. This is the groundwork for conditional
  execution. Scripts can call other scripts (depth-limited,
  each with its own arguments). The same mechanism powers `/.profile`, which runs
  automatically at boot - edit it with `ed /.profile` and reboot to apply.
  Handy for setting variables, `cd`-ing to a working dir, connecting to
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
