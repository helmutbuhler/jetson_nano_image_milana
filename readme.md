# Overview
This repository contains an unofficial ISO image for the Jetson Nano (2GB and 4GB) which fixes a few issues:
 - Working UART port on the 12-Pin Button Header. By default this can be only used as a debugging console. This way you have access to two UART ports. (The other one is on the 40-Pin Header)
 -  Working SPI ports. Due to a bug in the official image, the SPI ports cannot be used because the GPIO driver accesses them. (There are no working workarounds that don't involve flashing the Jetson Nano from a Linux machine).
 - More robust I2C ports. The official release can lock the kernel randomly for up for 10 seconds when using I2C.

All of these changes required me to build a custom Linux kernel and rebuild the ISO files. To save you the hassle I publish the modified ISO here. This ISO is also used in the Milana robot (todo link) project, but the issues it fixes are completely independent of it and you can use it for anything you like.

## Links to the ISOs
Here you can download the ISOs. See [here](build.md) if you want to build the ISO yourself. 
todo

## Usage
### Flash ISO file
First you need to flash the ISO file to your sd-card. I recommend using [balenaEtcher](https://www.balena.io/etcher/) for that.

Then you need to insert the sd-card into your Jetson Nano and setup it. todo Here is the official guide. But it boils down to this:

### Setup Jetson Nano
- Connect your Jetson to your PC via Micro USB and power it up.
- Find out the com number in device-manager.
- Connect with Putty/ssh on that com port and baudrate 115200. If there is locking error message, reboot the Jetson.
- Do the config (skip network, no swap).

### Connecting to WIFI
In case you want to connect to a WIFI, you can use these commands:
- Display status: `nmcli d`
- List wifis: `nmcli dev wifi`
- List known wifis: `ls /etc/NetworkManager/system-connections`
- Force a rescan: `sudo nmcli device wifi rescan`
- Enable WIFI: `nmcli r wifi on`
- Connect to a WIFI: `sudo nmcli d wifi connect *wifiname* password *pw*`

(reboot if connection fails or no wifis are listed)

### Install updates
After you have established network connection you can shutdown the Jetson and disconnect the USB cable. 

If you don't need the stuff in this list, you can execute this to save some space and also reduce the amount of updates you need in the future:
```
sudo apt remove libreoffice-writer libreoffice-avmedia-backend-gstreamer libreoffice-base-core libreoffice-calc libreoffice-common libreoffice-core libreoffice-draw libreoffice-gnome libreoffice-gtk3 libreoffice-impress libreoffice-math libreoffice-ogltrans libreoffice-pdfimport libreoffice-style-breeze libreoffice-style-galaxy libreoffice-style-tango chromium-browser chromium* yelp unity thunderbird rhythmbox nautilus gnome-software
sudo apt autoremove
```

Since we use a custom kernel, we need to disable future updates for it. Otherwise the changes in this repository might get overwritten by an update in the future:
```
sudo apt-mark hold nvidia-l4t-kernel nvidia-l4t-kernel-dtbs nvidia-l4t-kernel-headers nvidia-l4t-bootloader nvidia-l4t-jetson-io
```

Finally we can update it:
```
sudo apt-get update && sudo apt-get upgrade
```

### Init UART
In case you want to use the extra UART port, you need to execute this:

```
sudo systemctl stop nvgetty
sudo systemctl disable nvgetty
udevadm trigger
```

If you want to be able to access UART ports without sudo, you can also execute this:
```
sudo adduser *user* dialout
sudo chmod 666 /dev/ttyS0
sudo chmod 666 /dev/ttyTHS1
```
(Replace *user* with your username)

### Init SPI Pins:
If you want to access SPI, do this: (This is the same as in this [tutorial](todo))

```
sudo /opt/nvidia/jetson-io/jetson-io.py
Configure 40 pin
Configure header pins manually
enable spi1 and/or spi2
Save pin changes
Save without reboot
```

Note: In this ISO the SPI driver is loaded automatically on startup. In the official ISO you would also need to start it on every boot with `modprobe spidev`.

And that's it! You now have a Jetson Nano that actually works with the communication ports it is advertized to work with. Apart from the changes mentioned in this readme, it should work just like the official version.

I took me quite some time to figure all this stuff out. Hopefully you'll have an easier time with your Jetson Nano than I had.
