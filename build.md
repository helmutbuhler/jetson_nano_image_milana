# Building the modified Linux Kernel and the Jetson Nano Image

Here I show how to build the Image for yourself. This might also be useful if you want to build the official Jetson Nano Image or a custom one yourself.

The part about building the Kernel is originally from [here](https://blog.kevmo314.com/compiling-custom-kernel-modules-on-the-jetson-nano.html), I just simplified that part a bit and made it work with the current version. Anyway, that blogpost saved me a lot of time, as this description will hopefully help you. But you definitely need to set some time aside and can expect a lot of frustration if you intend to follow along.

## Install Ubuntu in VirtualBox
First we need a VM with Ubuntu 18.04. If you have a machine with that running you can also use that, but you probably don't. Note that WSL won't work (that would be too easy).

I recommend installing VirtualBox for this. Then download the Ubuntu Image [here](https://releases.ubuntu.com/18.04/) and install it in a new VM with about 100GB diskspace (Yes, you will need that much diskspace). Also make sure that you disable Internet access in the VM before you install. Otherwise Ubuntu will download some update that will make it unusable. Also don't use the unattended feature from VirtualBox.

## Connect to VM via SSH
Once you have it running, open a Terminal and install the ssh server:
```
sudo apt install openssh-server
```

Check it is running with:
```
sudo systemctl status ssh
```
It should show: active (running).

Next we need to open the ssh port in VirtualBox. Go to:
```
VM settings -> Network -> Advanced -> Port Forwarding
```
and set:

```
Name: ssh
Protocol: TCP
Host Port: 2222 (or any other port you like)
Guest port: 22
```

Then you should be able to connect with putty/ssh to localhost on port 2222.

## Install nvidia-sdk-manager
For this, you need a nvidia account (nvidia likes to make this as painful as possible). You can register [here](https://developer.nvidia.com/login) on your real machine.

Once you have that, open Firefox inside of the VM and go to: https://developer.nvidia.com/nvidia-sdk-manager

Download the NVIDIA SDK Manager .deb file there and note where you downloaded it. Now we are done with the GUI of Ubuntu, the rest will be done in your ssh client.

Execute this to install the sdk manager:
```
sudo apt-get update
sudo apt install -y libgconf-2-4 libcanberra-gtk-module
(Go to the path where you downloaded the sgkmanager file.)
sudo dpkg -i sdkmanager_1.****
(Replace **** with the version you have)

sdkmanager --cli --query interactive --archived-versions
```

## Download nvidia files and the Linux Kernel Source

Now we use the sdk manager to download some files we need to build the Jetson Image.

Select option 1 and login with your nvidia account.

Then select these options:
- select install
- select product: Jetson
- hardware configuration: Target Hardware
- select target: Jetson Nano modules
- select target operating system: Linux
- select version: JetPack 4.6.4
- get detailed options: No
- flash the target board: No

And run the printed command.

Now some download manager ui should open up. Select this:
- Allow analytics tracking? No
- Accept licences
- Let it download and install everything
- If a red error icon appears in the top window, Ctrl+C and restart.
- If it asks: Install stuff on your Nano? Select Skip.

We also need to download the Linux kernel source.
We download the version R32.7.4 from [here](https://developer.nvidia.com/downloads/embedded/l4t/r32_release_v7.4/sources/t210/public_sources.tbz2) 
If you want to build another version, you can browse the available versions [here](https://developer.nvidia.com/embedded/jetson-linux-archive) and search for `"Driver Package (BSP) Sources"` in the page that is linked.

Once you have the file on your VM (copy it into your home directory), execute this to unpack the files:
```
tar -xjf public_sources.tbz2
cd Linux_for_Tegra/source/public
tar -xjf kernel_src.tbz2
```

## Linux Kernel Source Modifications
Now it's time to change the source code to apply our bugfixes. It's 6 changes in total. You can use nano to edit the text files.

### Change 1
Go to this file: `Linux_for_Tegra/source/public/kernel/kernel-4.9/drivers/gpio/gpio-tegra.c`. You can optionally make sure it's the same as [this file](gpio-tegra.c_diff/gpio-tegra_ori.c). If it's equal, you can replace it with [this file](gpio-tegra.c_diff/gpio-tegra.c). If you try to compile a different version and it isn't equal, you can generate a diff and apply that.
(The changes in the diff are partly from [here](https://forums.developer.nvidia.com/t/enabling-spidev-on-the-jetson-nano-is-hanging-when-flashing/73338/59))
It modifies the GPIO driver such that it closes its access to the SPI pins when booting. This is neccessary because some code in the bootloader of the Jetson opens these pins as GPIO and that code cannot be modified from an iso image.

### Change 2
Go to: `Linux_for_Tegra/source/public/hardware/nvidia/platform/t210/porg/kernel-dts/tegra210-porg-p3448-common.dtsi`
and make sure this entry looks like this (note the comments):
```
   	chosen {
		nvidia,tegra-porg-sku;
		/*stdout-path = "/serial@70006000";*/
		nvidia,tegra-always-on-personality;
		no-tnid-sn;
		/*bootargs = "earlycon=uart8250,mmio32,0x70006000";*/
		bootargs = "";
	};
```
This disables UART logging on the debug port.

### Change 3
Go to `Linux_for_Tegra/source/public/hardware/nvidia/platform/t210/porg/kernel-dts/porg-platforms/tegra210-porg-pinmux-p3448-0000-b00.dtsi` and apply this diff: (- indicates the old line, + indicates the new line)

```
			spi1_mosi_pc0 {
				nvidia,pins = "spi1_mosi_pc0";
-				nvidia,function = "rsvd1";
+				nvidia,function = "spi1";
				nvidia,pull = <0x1>;
				nvidia,tristate = <0x0>;
				nvidia,enable-input = <0x1>;
			};

			spi1_miso_pc1 {
				nvidia,pins = "spi1_miso_pc1";
-				nvidia,function = "rsvd1";
+				nvidia,function = "spi1";
				nvidia,pull = <0x1>;
				nvidia,tristate = <0x0>;
				nvidia,enable-input = <0x1>;
			};

			spi1_sck_pc2 {
				nvidia,pins = "spi1_sck_pc2";
-				nvidia,function = "rsvd1";
+				nvidia,function = "spi1";
				nvidia,pull = <0x1>;
				nvidia,tristate = <0x0>;
				nvidia,enable-input = <0x1>;
			};

			spi1_cs0_pc3 {
				nvidia,pins = "spi1_cs0_pc3";
-				nvidia,function = "rsvd1";
+				nvidia,function = "spi1";
				nvidia,pull = <0x2>;
				nvidia,tristate = <0x0>;
				nvidia,enable-input = <0x1>;
			};

			spi1_cs1_pc4 {
				nvidia,pins = "spi1_cs1_pc4";
-				nvidia,function = "rsvd1";
+				nvidia,function = "spi1";
				nvidia,pull = <0x2>;
				nvidia,tristate = <0x0>;
				nvidia,enable-input = <0x1>;
			};

		spi2_mosi_pb4 {
				nvidia,pins = "spi2_mosi_pb4";
-				nvidia,function = "rsvd2";
+				nvidia,function = "spi2";
				nvidia,pull = <0x1>;
				nvidia,tristate = <0x0>;
				nvidia,enable-input = <0x1>;
			};

			spi2_miso_pb5 {
				nvidia,pins = "spi2_miso_pb5";
-				nvidia,function = "rsvd2";
+				nvidia,function = "spi2";
				nvidia,pull = <0x1>;
				nvidia,tristate = <0x0>;
				nvidia,enable-input = <0x1>;
			};

			spi2_sck_pb6 {
				nvidia,pins = "spi2_sck_pb6";
-				nvidia,function = "rsvd2";
+				nvidia,function = "spi2";
				nvidia,pull = <0x1>;
				nvidia,tristate = <0x0>;
				nvidia,enable-input = <0x1>;
			};

			spi2_cs0_pb7 {
				nvidia,pins = "spi2_cs0_pb7";
-				nvidia,function = "rsvd2";
+				nvidia,function = "spi2";
				nvidia,pull = <0x1>;
				nvidia,tristate = <0x0>;
				nvidia,enable-input = <0x1>;
			};

			spi2_cs1_pdd0 {
				nvidia,pins = "spi2_cs1_pdd0";
-				nvidia,function = "rsvd1";
+				nvidia,function = "spi2";
				nvidia,pull = <0x1>;
				nvidia,tristate = <0x0>;
				nvidia,enable-input = <0x1>;
			};
```
This will help make SPI work. It's copied from this poor guys [solution](https://forums.developer.nvidia.com/t/spi-setup-issues-with-jetson-nano-b01-devkit/267095/129). I'm not sure it's actually needed, havn't tried to remove it since I got it working.

### Change 4
Go to: `Linux_for_Tegra/source/public/kernel/kernel-4.9/drivers/i2c/busses/i2c-tegra.c` and change 10000 to 10 such that the TEGRA_I2C_TIMEOUT definition looks like this:
```
	#define TEGRA_I2C_TIMEOUT (msecs_to_jiffies(10))
```
This will reduce the timeout of I2C, for which 10s is ridiculously large.

### Change 5:
Go to: `Linux_for_Tegra/source/public/kernel/nvidia/drivers/tty/serial/tegra-combined-uart.c` and comment the function body out so it looks like this:

```
	static int __init tegra_combined_uart_console_setup(struct console *co,
								char *options)
	{
		//int baud = 115200;
		//int parity = 'n';
		//int bits = 8;
		//int flow = 'n';

		//if (options)
		//	uart_parse_options(options, &baud, &parity, &bits, &flow);

		//return uart_set_options(&tegra_combined_uart_port, co, baud, parity,
		//			bits, flow);
		return 0;
	}
```

### Change 6
Go to: `~/nvidia/nvidia_sdk/JetPack_4.6.4_Linux_JETSON_NANO_TARGETS/Linux_for_Tegra/p3448-0000.conf.common` and comment this out:

```
#CMDLINE_ADD="console=ttyS0,115200n8 console=tty0 fbcon=map:0 net.ifnames=0";
```

## Compile the Linux Kernel
Now we need to compile the kernel with our modifications. Unfortunately the Jetson runs on ARM64, so we first need to install some cross-platform tools.

Run this to install the compiler toolchain:
```
cd ~
wget http://releases.linaro.org/components/toolchain/binaries/7.3-2018.05/aarch64-linux-gnu/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu.tar.xz
tar xf gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu.tar.xz
export CROSS_COMPILE=$(pwd)/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-
export LOCALVERSION=-tegra
```

And run this to generate some config files:
```
# Configure and build the kernel
TEGRA_KERNEL_OUT=$HOME/t4l-kernel
mkdir $TEGRA_KERNEL_OUT
cd ~/Linux_for_Tegra/source/public/kernel/kernel-4.9/
make ARCH=arm64 O=$TEGRA_KERNEL_OUT -j4 tegra_defconfig
```

Now, open the config with:
```
nano ~/t4l-kernel/.config
```

And make sure these entries look like this:
```
CONFIG_SERIAL_8250_CONSOLE=n
CONFIG_SPI_SPIDEV=y
CONFIG_MAGIC_SYSRQ=n
```
(`CONFIG_SPI_SPIDEV=y` makes sure you don't need to manually load spi driver with `sudo modprobe spidev`)

And with this we finally compile it (-j4 indicates the number of processors to use):
```
make ARCH=arm64 O=$TEGRA_KERNEL_OUT -j4
```
This can take about 15 minutes, but hopefully it will succeed without errors. If something goes wrong, you probably made some typo while doing the modifications and the error message will point you to it.

## Build the sdcard image
Now we need to combine the kernel binary with the nvidia files we downloaded earlier.

First we need to copy our custom kernel binary to a different location:

```
cd ~/nvidia/nvidia_sdk/JetPack_4.6.4_Linux_JETSON_NANO_TARGETS/Linux_for_Tegra/kernel/
cp $TEGRA_KERNEL_OUT/arch/arm64/boot/Image Image
cp -r $TEGRA_KERNEL_OUT/arch/arm64/boot/dts/* dtb
```

Then we need to install some modules (not sure what this is)
```
cd ~/Linux_for_Tegra/source/public/kernel/kernel-4.9/
sudo make ARCH=arm64 O=$TEGRA_KERNEL_OUT modules_install INSTALL_MOD_PATH=~/nvidia/nvidia_sdk/JetPack_4.6.4_Linux_JETSON_NANO_TARGETS/Linux_for_Tegra/rootfs/
```

Finally we build the sdcard image (this takes about 10 minutes)
```
cd ~/nvidia/nvidia_sdk/JetPack_4.6.4_Linux_JETSON_NANO_TARGETS/Linux_for_Tegra
# get available options:
./tools/jetson-disk-image-creator.sh
sudo ./apply_binaries.sh -r rootfs
sudo ./tools/jetson-disk-image-creator.sh -o sdcard_4.6.4_2gb_V1.img -b jetson-nano-2gb-devkit > sdcard_4.6.4_2gb_V1.log 2>&1
sudo ./tools/jetson-disk-image-creator.sh -o sdcard_4.6.4_4gb_V1.img -b jetson-nano -r 300 > sdcard_4.6.4_4gb_V1.log 2>&1
```
You only need to execute one of the bottom two commands, depending on what kind of Jetson Nano you have.
Finally you can copy the resulting .img file out of the VM and proceed to install it as described in the [readme](readme.md).

