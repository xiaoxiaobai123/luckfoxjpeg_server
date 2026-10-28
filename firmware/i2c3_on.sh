#!/bin/sh
# Enable hardware I2C3 (SDA = header pin 14 / GPIO1_D2, SCL = pin 15 / GPIO1_D3, "M1" pin group) with a runtime
# device-tree overlay, the same mechanism luckfox-config uses. Does not touch the SPI0 overlay.
# The compiled overlay is cached (keyed by the pin-group number), so later boots skip the compiler.
[ -d /sys/kernel/config/device-tree/overlays/i2c3m1 ] && exit 0
P=/proc/device-tree/pinctrl/i2c3/i2c3m1-xfer/phandle
CACHE=/userdata/circle/cache
[ -f $P ] || { echo "no i2c3m1-xfer pin group"; exit 1; }
PH=$(od -An -tu1 $P | awk '{print $1*16777216+$2*65536+$3*256+$4}')
if [ -f $CACHE/i2c3.dtbo ] && [ "$(cat $CACHE/i2c3.key 2>/dev/null)" = "$PH" ]; then
    DTBO=$CACHE/i2c3.dtbo
else
    cat > /tmp/i2c3.dts <<DTS
/dts-v1/;
/plugin/;
&{/i2c@ff460000}{
    status = "okay";
    clock-frequency = <400000>;
    pinctrl-0 = <$PH>;
};
DTS
    dtc -I dts -O dtb /tmp/i2c3.dts -o /tmp/i2c3.dtbo 2>/dev/null || { echo "dtc failed"; exit 1; }
    DTBO=/tmp/i2c3.dtbo
    mkdir -p $CACHE && cp /tmp/i2c3.dtbo $CACHE/i2c3.dtbo && echo "$PH" > $CACHE/i2c3.key
fi
mkdir -p /sys/kernel/config/device-tree/overlays/i2c3m1
cat $DTBO > /sys/kernel/config/device-tree/overlays/i2c3m1/dtbo
echo 1 > /sys/kernel/config/device-tree/overlays/i2c3m1/status
