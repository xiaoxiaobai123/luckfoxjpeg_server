#!/bin/sh
# Enable SPI0 (header pins 6/7/8 = CS0/CLK/MOSI, MISO is left alone) with a runtime device-tree overlay, the same
# mechanism luckfox-config uses, so the LCD can start without waiting for "luckfox-config load" (about 13 s).
# The compiled overlay is cached (keyed by the pin-group numbers it contains), so later boots skip the compiler.
[ -d /sys/kernel/config/device-tree/overlays/spi0m0_early ] && exit 0
D=/proc/device-tree/pinctrl/spi0
CACHE=/userdata/circle/cache
ph() { od -An -tu1 $1/phandle | awk '{print $1*16777216+$2*65536+$3*256+$4}'; }
for n in spi0m0-clk spi0m0-mosi spi0m0-cs0; do [ -f $D/$n/phandle ] || { echo "no pin group $n"; exit 1; }; done
CLK=$(ph $D/spi0m0-clk); MOSI=$(ph $D/spi0m0-mosi); CS=$(ph $D/spi0m0-cs0)
KEY="$CLK $MOSI $CS"
if [ -f $CACHE/spi0.dtbo ] && [ "$(cat $CACHE/spi0.key 2>/dev/null)" = "$KEY" ]; then
    DTBO=$CACHE/spi0.dtbo
elif [ "$KEY" = "72 74 75" ] && [ -f /opt/circle/spi0.dtbo ]; then
    DTBO=/opt/circle/spi0.dtbo   # precompiled (the slim image has no dtc)
else
    cat > /tmp/spi0.dts <<DTS
/dts-v1/;
/plugin/;
&{/spi@ff500000}{
    status = "okay";
    pinctrl-0 = <$CLK $MOSI $CS>;
};
&{/spi@ff500000/spidev@0}{
    status = "okay";
    spi-max-frequency = <40000000>;
};
DTS
    dtc -I dts -O dtb /tmp/spi0.dts -o /tmp/spi0.dtbo 2>/dev/null || { echo "dtc failed"; exit 1; }
    DTBO=/tmp/spi0.dtbo
    mkdir -p $CACHE && cp /tmp/spi0.dtbo $CACHE/spi0.dtbo && echo "$KEY" > $CACHE/spi0.key
fi
mkdir -p /sys/kernel/config/device-tree/overlays/spi0m0_early
cat $DTBO > /sys/kernel/config/device-tree/overlays/spi0m0_early/dtbo
echo 1 > /sys/kernel/config/device-tree/overlays/spi0m0_early/status
