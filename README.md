# Vitacompanion
Vitacompanion is a PS Vita daemon which allows developers to upload and install packages on the device in an faster way.
Vitacompanion runs as tai userspace module and it binds the port 1337 to an FTP server and the port 1338 to a command server.

# Build and installation
```bash
cd vitacompanion
mkdir build
cd build
cmake ...
make
```
run VitaShell on the device, press SELECT to activate the FTP server and copy vitacompanion.suprx to ur0:/tai and add the
following line to ur0:/tai/config.txt

```
*main
ur0:tai/vitacompanion.suprx
```

# Usage
You can upload stuff to your vita by running:
```
curl -T hello_cpp_world.vpk ftp://anonymous@IP_TO_PSVITA:1337/ux0:yourdir/a.vpk
```
You can reboot your vita by running
```
echo reboot | nc IP_TO_PSVITA:1338
```

# Available commands:
TODO
 
