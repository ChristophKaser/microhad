#!/usr/bin/env bash
# Offline stock userspace on a replacement Malta kernel, not RTL8196E emulation.
set -euo pipefail
umask 077
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
. "$repo_root/scripts/workspace-env.sh"
workspace_load "$repo_root"
work_dir=${STOCK_QEMU_DIR:-$repo_root/build/stock-qemu}
version=25.12.5
base_url="https://downloads.openwrt.org/releases/$version/targets/malta/be"
usage() {
    cat <<'EOF'
Usage: scripts/stock-qemu.sh download
       scripts/stock-qemu.sh prepare ROOTFS_SQUASHFS TUYA_JFFS2
       scripts/stock-qemu.sh boot [stock|openwrt]

Downloads go to ignored build/stock-qemu (override with STOCK_QEMU_DIR).
Requires a Debian/Ubuntu amd64 host, apt, curl, dpkg-deb, unsquashfs,
python3 with venv support, mke2fs, and bubblewrap. Download needs Internet;
boot always uses -nic none AND a private network namespace, with no fallback.
Prepare downloads Jefferson into a local venv if it is not already available.
No packages are installed system-wide. Host QEMU shared libraries must exist.

Prepare takes explicit LOCAL partition backups, never contacts a gateway,
and refuses to overwrite an existing extraction or disk. Extracted data may
contain secrets: keep the working directory private and out of version control.
Boot uses a disposable disk overlay; guest changes disappear on shutdown.
The replacement OpenWrt kernel is not the original Realtek kernel.

Stock boot mounts proc/sys, creates basic devices, and opens the original
BusyBox shell as PID 1. It does not run hardware-dependent rcS automatically.
To test the application from that shell:
  cd /tuya/tuya_user2
  LD_LIBRARY_PATH=/tuya ./tyZ3Gw /tuya/tuya_user2 /tuya/def.cfg
Missing Zigbee UART and network errors are expected. Shut down with:
  poweroff -f

If namespace creation is denied, run in an environment allowing user/network
namespaces; never remove the network isolation options to work around it.
EOF
}
mkdir -p "$work_dir"
work_dir=$(cd -- "$work_dir" && pwd)
case ${1:-help} in
    download)
        [[ $# == 1 ]] || { usage; exit 2; }
        [[ $(dpkg --print-architecture) == amd64 ]] || { echo 'Requires amd64 host' >&2; exit 1; }
        mkdir -p "$work_dir/packages"
        (cd "$work_dir/packages"; apt download qemu-system-mips)
        for package in "$work_dir"/packages/qemu-system-mips_*.deb; do
            dpkg-deb -x "$package" "$work_dir/emulator"
        done
        for kind in vmlinux vmlinux-initramfs; do
            curl --fail --location --connect-timeout 15 --retry 2 \
                -o "$work_dir/$kind.elf.part" "$base_url/openwrt-$version-malta-be-$kind.elf"
            case $kind in
                vmlinux) checksum=ced8942d9286483cd29be4ce3b05eff73beb8f9e070d498e484e18c1dbb94c96 ;;
                *) checksum=21e0b62a96f3e87056adf3fd38715bcd8a209e119e7f7d6269a689f36bfda5ac ;;
            esac
            printf '%s  %s\n' "$checksum" "$work_dir/$kind.elf.part" | sha256sum --check
            mv "$work_dir/$kind.elf.part" "$work_dir/$kind.elf"
        done
        "$work_dir/emulator/usr/bin/qemu-system-mips" --version
        ;;
    prepare)
        [[ $# == 3 ]] || { usage; exit 2; }
        [[ -f $2 && -f $3 ]] || { echo 'Both local partition images are required' >&2; exit 1; }
        for output in rootfs tuya-extracted stock-root.ext4; do
            [[ ! -e $work_dir/$output ]] || { echo "Refusing to overwrite $work_dir/$output" >&2; exit 1; }
        done
        if command -v jefferson >/dev/null; then
            extractor=$(command -v jefferson)
        else
            python3 -m venv "$work_dir/extractor-venv"
            "$work_dir/extractor-venv/bin/pip" install 'jefferson==0.4.8'
            extractor=$work_dir/extractor-venv/bin/jefferson
        fi
        unsquashfs -no-progress -d "$work_dir/rootfs" "$2" > "$work_dir/extract-rootfs.log" 2>&1
        "$extractor" "$3" -d "$work_dir/tuya-extracted" > "$work_dir/extract-tuya.log" 2>&1
        [[ -f $work_dir/tuya-extracted/tuya_start.sh ]] || { echo 'Expected /tuya layout missing; inspect extraction log' >&2; exit 1; }
        cp -a "$work_dir/tuya-extracted/." "$work_dir/rootfs/tuya/"
        cat > "$work_dir/rootfs/qemu-init.sh" <<'EOF'
#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /dev
mknod /dev/null c 1 3
mknod /dev/zero c 1 5
mknod /dev/random c 1 8
mknod /dev/urandom c 1 9
mknod /dev/console c 5 1
mkdir -p /var/tmp /var/run /var/log
printf '\nStock userspace on Malta; no network adapter. poweroff -f to stop.\n'
exec /bin/sh
EOF
        chmod 755 "$work_dir/rootfs/qemu-init.sh"
        truncate -s 64M "$work_dir/stock-root.ext4"
        mke2fs -q -t ext4 -F -d "$work_dir/rootfs" "$work_dir/stock-root.ext4"
        echo "Prepared $work_dir/stock-root.ext4"
        ;;
    boot)
        [[ $# -le 2 ]] || { usage; exit 2; }
        qemu=$work_dir/emulator/usr/bin/qemu-system-mips
        [[ -x $qemu ]] || { echo 'Run download first' >&2; exit 1; }
        case ${2:-stock} in
            stock)
                [[ -f $work_dir/stock-root.ext4 ]] || { echo 'Run prepare first' >&2; exit 1; }
                args=(-kernel "$work_dir/vmlinux.elf" -snapshot
                    -drive "file=$work_dir/stock-root.ext4,format=raw,if=ide"
                    -append 'console=ttyS0,115200 root=/dev/sda rw rootwait init=/qemu-init.sh') ;;
            openwrt)
                args=(-kernel "$work_dir/vmlinux-initramfs.elf" -append 'console=ttyS0,115200') ;;
            *) usage; exit 2 ;;
        esac
        exec bwrap --unshare-all --die-with-parent --ro-bind / / \
            --tmpfs /tmp --tmpfs /var/tmp --dev /dev \
            --ro-bind "$work_dir" "$work_dir" --cap-drop ALL -- \
            "$qemu" -M malta -cpu 24Kc -m 256 -nic none \
            -display none -monitor none -serial stdio -no-reboot "${args[@]}"
        ;;
    help|--help|-h) usage ;;
    *) usage; exit 2 ;;
esac
