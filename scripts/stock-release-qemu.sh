#!/usr/bin/env bash
# Run the paired release installer against private stock userspace in QEMU.
# All images, packages, credentials, captures and logs remain outside Git.
set -euo pipefail
umask 077

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
. "$repo_root/scripts/workspace-env.sh"
workspace_load "$repo_root"

work_dir=${STOCK_RELEASE_QEMU_DIR:-/tmp/stock-release-qemu-$(id -u)}
disk_size=${STOCK_RELEASE_DISK_SIZE:-256M}
ssh_port=${STOCK_RELEASE_SSH_PORT:-2223}
http_port=${STOCK_RELEASE_HTTP_PORT:-18081}
qemu_bin=${STOCK_RELEASE_QEMU_BIN:-}
kernel=${STOCK_RELEASE_KERNEL:-}

usage() {
    cat <<'EOF'
Usage:
  scripts/stock-release-qemu.sh prepare ROOTFS_TREE MTD3_IMAGE ROOT_PASSWORD_FILE
  scripts/stock-release-qemu.sh boot
  scripts/stock-release-qemu.sh install-paired RELEASE_DIR [CONFIG_JSON] [ZIGGURAT_STATE_JSON]

Set STOCK_RELEASE_QEMU_BIN and STOCK_RELEASE_KERNEL to local QEMU/kernel paths.
The stock rootfs tree must already contain the stock Dropbear/account files and
the local OpenWrt e1000 module at lib/modules/6.12.94/e1000.ko. The password
file must contain the MTD3-derived root password and be readable only by you.

prepare copies the supplied files into a private /tmp work directory and builds
an ext4 base disk. boot starts an interactive Malta guest. install-paired starts
the same fresh guest, runs the release bundle's paired installer, captures its
network packets, then stops QEMU. The installer test defaults to the bundle's
paired example config and a synthetic Zigbee state generated from repository
test fixtures; it never uses real Zigbee network keys.

QEMU uses restricted user networking, a loopback-only SSH/HTTP forward, and an
e1000 adapter. The guest has no Internet route. The release package directory,
firmware images, kernel, emulator, rootfs, MTD3, password and all outputs remain
local; this script does not download or install firmware.
EOF
}

need_file() { [[ -f $1 ]] || { echo "Required file missing: $1" >&2; exit 1; }; }
need_dir() { [[ -d $1 ]] || { echo "Required directory missing: $1" >&2; exit 1; }; }
check_runtime() {
    [[ -n $qemu_bin && -x $qemu_bin ]] || { echo 'Set STOCK_RELEASE_QEMU_BIN to a local qemu-system-mips executable' >&2; exit 1; }
    [[ -n $kernel ]] || { echo 'Set STOCK_RELEASE_KERNEL to a local Malta kernel ELF' >&2; exit 1; }
    need_file "$kernel"
    command -v mke2fs >/dev/null || { echo 'mke2fs is required' >&2; exit 1; }
    command -v ssh-keyscan >/dev/null || { echo 'ssh-keyscan is required' >&2; exit 1; }
    command -v sshpass >/dev/null || { echo 'sshpass is required' >&2; exit 1; }
    command -v python3 >/dev/null || { echo 'python3 is required' >&2; exit 1; }
}

mkdir -p "$work_dir"
chmod 700 "$work_dir"
work_dir=$(cd -- "$work_dir" && pwd)
rootfs_dir=$work_dir/rootfs
base_disk=$work_dir/stock-root.ext4
guest_disk=$work_dir/guest.ext4
qemu_pidfile=$work_dir/qemu.pid
console_log=$work_dir/guest-console.log
pcap_file=$work_dir/guest-network.pcap

qemu_args() {
    QEMU_ARGS=(-M malta -cpu 24Kc -m 256 -kernel "$kernel"
        -drive "file=$guest_disk,format=raw,if=ide"
        -netdev "user,id=offline,restrict=on,hostfwd=tcp:127.0.0.1:$ssh_port-:22,hostfwd=tcp:127.0.0.1:$http_port-:80"
        -device e1000,netdev=offline
        -object "filter-dump,id=guest-pcap,netdev=offline,file=$pcap_file"
        -display none -monitor none -no-reboot
        -append 'console=ttyS0,115200 root=/dev/sda rw rootwait init=/qemu-init.sh')
}

start_background_qemu() {
    cp --reflink=auto "$base_disk" "$guest_disk"
    rm -f "$qemu_pidfile" "$console_log" "$pcap_file"
    qemu_args
    "$qemu_bin" -daemonize -pidfile "$qemu_pidfile" -serial "file:$console_log" "${QEMU_ARGS[@]}"
    for _ in $(seq 1 30); do
        [[ -s $qemu_pidfile ]] && kill -0 "$(cat "$qemu_pidfile")" 2>/dev/null && return 0
        sleep 1
    done
    echo 'QEMU failed to start; inspect guest-console.log' >&2
    return 1
}

stop_qemu() {
    if [[ -s $qemu_pidfile ]]; then
        pid=$(cat "$qemu_pidfile")
        kill "$pid" 2>/dev/null || true
        for _ in $(seq 1 10); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 1
        done
        kill -9 "$pid" 2>/dev/null || true
        rm -f "$qemu_pidfile"
    fi
}

case ${1:-help} in
    prepare)
        [[ $# == 4 ]] || { usage; exit 2; }
        check_runtime
        rootfs_source=$(realpath "$2")
        mtd3_source=$(realpath "$3")
        password_source=$(realpath "$4")
        need_dir "$rootfs_source"
        need_file "$mtd3_source"
        need_file "$password_source"
        [[ ! -e $rootfs_dir && ! -e $base_disk ]] || {
            echo "Refusing to overwrite existing prepared files in $work_dir" >&2; exit 1;
        }
        [[ -r $rootfs_source/tuya/tuya_start.sh && -x $rootfs_source/bin/dropbear &&
           -x $rootfs_source/bin/chpasswd ]] || {
            echo 'Rootfs must contain /tuya/tuya_start.sh, /bin/dropbear and /bin/chpasswd' >&2; exit 1;
        }
        [[ -f $rootfs_source/lib/modules/6.12.94/e1000.ko ]] || {
            echo 'Rootfs is missing lib/modules/6.12.94/e1000.ko' >&2; exit 1;
        }
        [[ $(stat -c '%a' "$password_source") == 600 ]] || {
            echo 'Password file permissions must be 0600' >&2; exit 1;
        }
        install -d -m 700 "$rootfs_dir"
        cp -a "$rootfs_source/." "$rootfs_dir/"
        install -m 600 "$mtd3_source" "$rootfs_dir/mtd3.img"
        install -m 600 "$password_source" "$work_dir/root-password"
        install -m 600 "$password_source" "$rootfs_dir/tuya/.stock-root-passwd"
        install -m 755 "$repo_root/scripts/stock-release-qemu-init.sh" "$rootfs_dir/qemu-init.sh"
        truncate -s "$disk_size" "$base_disk"
        mke2fs -q -t ext4 -F -d "$rootfs_dir" "$base_disk"
        chmod 600 "$base_disk"
        echo "Prepared private stock guest in $work_dir"
        ;;
    boot)
        [[ $# == 1 ]] || { usage; exit 2; }
        check_runtime
        need_file "$base_disk"
        cp --reflink=auto "$base_disk" "$guest_disk"
        chmod 600 "$guest_disk"
        rm -f "$pcap_file"
        qemu_args
        exec "$qemu_bin" "${QEMU_ARGS[@]}" -serial stdio
        ;;
    install-paired)
        [[ $# -ge 2 && $# -le 4 ]] || { usage; exit 2; }
        check_runtime
        need_file "$base_disk"
        release_dir=$(realpath "$2")
        need_file "$release_dir/install.sh"
        config=${3:-$release_dir/config.paired.example.json}
        need_file "$config"
        state=${4:-$work_dir/ziggurat-state.synthetic.json}
        if [[ $# -lt 4 ]]; then
            PYTHONPATH="$repo_root/microhad/scripts" python3 - "$state" <<'PY'
import json
import sys
from pathlib import Path
from test_network_migration import fixture
from ziggurat_migration import native_state, prepared_state

output = Path(sys.argv[1])
output.write_text(json.dumps(native_state(prepared_state(fixture())), separators=(",", ":")) + "\n")
output.chmod(0o600)
PY
        else
            need_file "$state"
        fi
        config=$(realpath "$config")
        state=$(realpath "$state")
        [[ -r $work_dir/root-password ]] || { echo 'Run prepare first' >&2; exit 1; }
        mkdir -p "$work_dir/home/.ssh"
        chmod 700 "$work_dir/home" "$work_dir/home/.ssh"
        rm -f "$work_dir/home/.ssh/known_hosts"
        ssh_bin=$(command -v ssh) || { echo 'ssh is required' >&2; exit 1; }
        shim_dir=$work_dir/ssh-shim
        mkdir -p "$shim_dir"
        printf -v quoted_ssh '%q' "$ssh_bin"
        printf -v quoted_known_hosts '%q' "$work_dir/home/.ssh/known_hosts"
        {
            printf '#!/bin/sh\n'
            printf 'exec %s -o UserKnownHostsFile=%s -o GlobalKnownHostsFile=/dev/null "$@"\n' \
                "$quoted_ssh" "$quoted_known_hosts"
        } > "$shim_dir/ssh"
        chmod 700 "$shim_dir/ssh"
        trap 'stop_qemu' EXIT
        trap 'exit 130' INT
        trap 'exit 143' TERM
        trap 'exit 129' HUP
        start_background_qemu
        ready=false
        for _ in $(seq 1 30); do
            if ssh-keyscan -T 2 -p "$ssh_port" 127.0.0.1 \
                >> "$work_dir/home/.ssh/known_hosts" 2>/dev/null &&
                [[ -s $work_dir/home/.ssh/known_hosts ]]; then
                ready=true
                break
            fi
            sleep 1
        done
        [[ $ready == true ]] || { echo 'Dropbear did not become reachable; inspect guest-console.log' >&2; exit 1; }
        chmod 600 "$work_dir/home/.ssh/known_hosts"
        export HOME=$work_dir/home WORKSPACE_CONFIG=/dev/null
        export PATH=$shim_dir:$PATH
        export SSH_PORT=$ssh_port SSH_USER=root SSH_PASSWORD_FILE=$work_dir/root-password
        export SSH_CONNECT_TIMEOUT=8
        if ! sshpass -f "$work_dir/root-password" "$shim_dir/ssh" -p "$ssh_port" \
            -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
            -o PreferredAuthentications=password -o PubkeyAuthentication=no \
            root@127.0.0.1 'ifconfig eth0 | grep -q "inet addr:10.0.2.15" || exit 2; \
                /bin/busybox ping -c 1 -W 2 1.1.1.1 >/dev/null 2>&1; rc=$?; \
                [ "$rc" -ne 0 ] || exit 3'; then
            echo 'Guest did not pass the DHCP/offline network check; refusing installer test' >&2
            exit 1
        fi
        echo 'Guest network check passed: DHCP works and outbound Internet is unreachable'
        installer_args=(--mode paired --host 127.0.0.1 --password-file "$work_dir/root-password"
            --ssh-port "$ssh_port" --config "$config" --ziggurat-state "$state")
        check_args=(--mode paired --host 127.0.0.1 --password-file "$work_dir/root-password" --ssh-port "$ssh_port")
        if "$release_dir/install.sh" check "${check_args[@]}" > "$work_dir/installer-check.log" 2>&1; then
            cat "$work_dir/installer-check.log"
        else
            cat "$work_dir/installer-check.log" >&2
            exit 1
        fi
        if "$release_dir/install.sh" install "${installer_args[@]}" > "$work_dir/installer.log" 2>&1; then
            cat "$work_dir/installer.log"
            echo "Guest packet capture: $pcap_file"
        else
            rc=$?
            cat "$work_dir/installer.log" >&2
            echo "Paired installer exited $rc. Guest console and packet capture are in $work_dir" >&2
            exit "$rc"
        fi
        ;;
    help|--help|-h) usage ;;
    *) usage; exit 2 ;;
esac
