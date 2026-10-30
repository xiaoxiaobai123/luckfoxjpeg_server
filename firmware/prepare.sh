#!/bin/bash
# usage: prepare.sh SDK_DIR APP_BINARY
# Slims the Luckfox Pico Mini SDK (before ./build.sh) and adds the circle checker to the root file system.
set -e
SDK=$1; APP=$2; HERE=$(cd "$(dirname "$0")" && pwd)

# 1. Buildroot: drop python, ssh, ntp, network tools, compression/debug extras
DEF=$SDK/sysdrv/tools/board/buildroot/luckfox_pico_defconfig
grep -vE '^BR2_PACKAGE_(PYTHON|GNUTLS|LIBCURL|LIBRSYNC|P7ZIP|ZIP|UNZIP|BMON|DHCPCD|IPERF|IPTABLES|LRZSZ|NTP|OPENSSH|RSYNC|SOCAT|WGET|DIALOG|TIME|NANO|CA_CERTIFICATES|LIBMD|PCRE2|DTC|LIBV4L)' "$DEF" > "$DEF.new"
mv "$DEF.new" "$DEF"

# 2. init scripts from the SDK overlays: python, ssh, microphone, bluetooth, luckfox-config
OV=$SDK/project/cfg/BoardConfig_IPC/overlay
rm -f $OV/overlay-luckfox-buildroot-init/etc/init.d/{S50sshd,S60micinit,S99hciinit,S99python}
rm -f $OV/overlay-luckfox-config/etc/init.d/S99luckfoxconfigload

# 3. our overlay: the program, helper scripts and the autostart script
O=$OV/overlay-circle
mkdir -p $O/etc/init.d $O/opt/circle
install -m755 $HERE/S21zcircle $O/etc/init.d/S21zcircle
install -m755 $HERE/spi0_on.sh $HERE/i2c3_on.sh $O/opt/circle/
install -m644 $HERE/profile $O/opt/circle/profile
install -m644 $HERE/spi0.dtbo $O/opt/circle/spi0.dtbo
install -m755 "$APP" $O/opt/circle/circle_detect
