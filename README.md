# Vitacompanion

Vitacompanion is a user module which makes developing homebrews for the PS Vita device easier. It does two things:
- Open a FTP server on port 1337
- Listen to commands on port 1338

# Build

```bash
mkdir build
cd build
cmake ..
make
```

# Install

Run VitaShell on your PS Vita, press SELECT to activate the FTP server and copy `vitacompanion.suprx` to `ur0:/tai`. Finally, add the following line to `ur0:/tai/config.txt`:

```
*main
ur0:tai/vitacompanion.suprx
```

# Usage

## FTP server

You can upload stuff to your vita by running:
```
curl --ftp-method nocwd -T somefile.zip ftp://IP_TO_VITA:1337/ux0:/somedir/
```
Or you can use your regular FTP client.

## Command server

Send a command by opening a TCP connection to the port 1338 of your Vita.

For example, you can reboot your vita by running:
```
echo reboot | nc IP_TO_PSVITA 1338
```

Note that you need to append a newline character to the command that you send. `echo` already adds one, which is why it works here.

### Available commands

| Command     | Arguments     | Explanation                  |
| ----------- | ------------- | ---------------------------- |
| `destroy`   | none          | kill all running applications |
| `kmodstart` | path          | load and start a kernel module (`.skprx`) from a vita path, returns a uid on success |
| `kmodstop`  | uid           | unload and stop a kernel module from a uid |
| `launch`    | `<TITLEID>`   | launch an application by id e.g. `launch VHBB00001` to launch the [Vita Homebrew Browser](https://github.com/devnoname120/vhbb) |
| `reboot`    | none          | reboot the console           |
| `screen`    | `on` or `off` | turn screen on or off        |
 
 **Note**: Commands are defined in [`src/cmd_definitions.c`](https://github.com/robsdedude/vitacompanion/blob/master/src/cmd_definitions.c), you can add new commands there.
 
# Acknowledgements 

Thanks to xerpi for his [vita-ftploader](https://bitbucket.org/xerpi/vita-ftploader/src/87ef1d13a8aa/plugin/?at=master) plugin, I stole a lot of his code (with his permission). Thanks to cpasjuste for [PSP2SHELL](https://github.com/Cpasjuste/PSP2SHELL), it inspired me to create this tool.
