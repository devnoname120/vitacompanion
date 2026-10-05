# Vitacompanion

Vitacompanion is a user module which makes developing homebrews for the PS Vita device easier. It:

- Opens a FTP server on port 1337
- Listens to commands on port 1338
- Simulates controller and touch input through a small kernel module

# Build

```bash
mkdir build
cd build
cmake ..
make
```

For a side-by-side test build that does not replace the canonical
`1337`/`1338` service, configure distinct ports and a distinct module name:

```bash
cmake .. \
  -DVITACOMPANION_FTP_PORT=1340 \
  -DVITACOMPANION_CMD_PORT=1341 \
  -DVITACOMPANION_MODULE_NAME=vitacompanion_test
make
```

# Install

Run VitaShell on your PS Vita, press SELECT to activate the FTP server and
copy both `vitacompanion.suprx` and `vitacompanion_kernel.skprx` to
`ur0:/tai`. Add the kernel module under `*KERNEL` and the user module under
`*main` in `ur0:/tai/config.txt`:

```
*KERNEL
ur0:tai/vitacompanion_kernel.skprx

*main
ur0:tai/vitacompanion.suprx
```

The two modules are loaded separately by taiHEN. The kernel module is required
because the user module imports its input API; the user module does not load
the kernel module itself. Reboot after replacing either module so older copies
are not left resident in memory.

# Usage

## FTP server

You can upload stuff to your vita by running:
```
curl -q -T somefile.zip ftp://IP_TO_VITA:1337/ux0:/somedir/somefile.zip
```
Or you can use your regular FTP client. The server accepts both Vita-style
paths such as `ux0:/somedir/` and FTP absolute paths such as `/ux0:/somedir/`.
It supports passive mode with `PASV`/`EPSV`, unrestricted IPv4 active mode
with `PORT`/`EPRT`, modern and traditional directory listings, ASCII and binary
transfers, file metadata, and transfer restart/append commands for compatibility
with generic FTP clients.

If you want curl to send the full FTP path directly instead of changing
directories first, use the double-slash URL form:
```
curl -q --ftp-method nocwd ftp://IP_TO_VITA:1337//ux0:/somedir/
```

## Command server

Send a command by opening a TCP connection to the port 1338 of your Vita.

For example, you can reboot your vita by running:
```
echo reboot | nc IP_TO_PSVITA 1338
```

Note that you need to append a newline character to the command that you send. `echo` already adds one, which is why it works here.

Multiple commands can be executed sequentially by separating them with
semicolons. The final semicolon is optional:

```
echo 'press cross; wait 100ms; release cross' | nc IP_TO_PSVITA 1338
```

### Available commands

| Command   | Arguments                       | Explanation                  |
| --------- | ------------------------------- | ---------------------------- |
| `help`    | none                            | display the help screen      |
| `launch`  | `<TITLEID>`                     | launch an application by id e.g. `launch VHBB00001` to launch the [Vita Homebrew Browser](https://github.com/devnoname120/vhbb) |
| `nosleep` | `on`, `off` or `status`         | enable or disable automatic suspend prevention. This is enabled by default at boot |
| `press`   | input target and values         | press a button, position a stick, or start/update a touch |
| `promote` | `<directory>`                   | promote an extracted Vita application directory |
| `quit`    | `<TITLEID>` or `all`            | quit an application by id, or all running applications |
| `reboot`  | none                            | reboot the console           |
| `release` | input target                    | release one input or all synthetic input |
| `screen`  | `on` or `off`                   | turn screen on or off        |
| `version` | none                            | display the loaded Vita Companion module version |
| `wait`    | duration ending in `ms` or `s`  | wait before executing the next chained command |

`wait` accepts integer durations such as `wait 1000ms` and `wait 3s`.

`promote` accepts only an extracted application directory, for example
`promote ux0:data/my-homebrew`. It does not extract or install VPK files. If
the directory does not already contain `sce_sys/package/head.bin`, Vita
Companion generates compatible promotion metadata from `sce_sys/param.sfo`,
including a fallback content ID for homebrew that omits `CONTENT_ID`.

Buttons use the following form:

```
press cross
release cross
```

Supported button names are `select`, `start`, `up`, `right`, `down`, `left`,
`l`, `r`, `l1`, `r1`, `l2`, `r2`, `l3`, `r3`, `triangle`, `circle`, `cross`
(`x` is an alias), `square`, and `ps`.

Use `left-stick` or `right-stick` as the analog-stick target. Coordinates are
integers from 0 through 255, with 128 as the center:

```
press left-stick 0 128
release left-stick
press right-stick 255 128
release right-stick
```

Front and rear touches have four independently controlled slots, numbered 0
through 3. Repeating `press` for an active slot moves that same touch while
preserving its contact ID. Coordinates use the Vita's raw 1920 by 1088 touch
space:

```
press front-touch 0 960 544
release front-touch 0
press rear-touch 1 400 300
release rear-touch 1
```

Use `release all` to clear every synthetic button, stick, and touch.
 
 **Note**: Commands are defined in [`src/cmd_definitions.c`](https://github.com/robsdedude/vitacompanion/blob/master/src/cmd_definitions.c), you can add new commands there.
 
 # Integration in IDE's
 
 ## VSCode
 
 https://github.com/imcquee/vitacompanion-VSCODE
 
# Acknowledgements 

Thanks to xerpi for his [vita-ftploader](https://bitbucket.org/xerpi/vita-ftploader/src/87ef1d13a8aa/plugin/?at=master) plugin, I stole a lot of his code (with his permission). Thanks to cpasjuste for [PSP2SHELL](https://github.com/Cpasjuste/PSP2SHELL), it inspired me to create this tool.
