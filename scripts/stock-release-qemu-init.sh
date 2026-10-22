#!/bin/sh
# Init for the disposable stock-userspace Malta guest. No stock rcS is run.
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t ramfs ramfs /var
mount -t tmpfs -o size=64k,mode=0755 tmpfs /dev
/bin/mknod.sh
mkdir -p /var/tmp /var/run /var/log /var/lock /var/root /var/lib/misc
[ -e /dev/mtd3 ] || ln -s /mtd3.img /dev/mtd3
[ -e /dev/ttyS1 ] || mknod /dev/ttyS1 c 4 65

# The OpenWrt e1000 module is supplied locally in the prepared rootfs.
/opt/openwrt/lib/libc.so --library-path /opt/openwrt/lib \
    /opt/openwrt/sbin/insmod /lib/modules/6.12.94/e1000.ko \
    >/var/log/e1000.log 2>&1

# The host places this private, disposable file using the locally supplied
# password derived from this device's MTD3 image. Remove it after one use.
if [ -r /tuya/.stock-root-passwd ]; then
    printf 'root:%s\n' "$(cat /tuya/.stock-root-passwd)" | /bin/chpasswd
    rm -f /tuya/.stock-root-passwd
fi

ifconfig lo 127.0.0.1
ifconfig eth0 up
udhcpc -i eth0 -s /bin/udhcpc.script -p /var/run/udhcpc0.pid \
    >/var/log/dhcp.log 2>&1 &
sleep 2
/bin/dropbear -P /var/run/dropbear.stock-test.pid -p 22 -K 300
while :; do sleep 300; done
