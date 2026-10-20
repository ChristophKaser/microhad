# Run stock gateway userspace offline in QEMU

[The launcher](../scripts/stock-qemu.sh) runs the stock filesystem on QEMU's
big-endian MIPS Malta board with a downloaded OpenWrt kernel. No kernel build
is required. This boots a replacement kernel with the original userspace; it
does not emulate the RTL8196E board or boot its original kernel.

Run these commands from the repository root on the development computer.
No command accesses or changes the physical gateway.

## Host prerequisites

Use a Debian/Ubuntu amd64 host with Bash, `apt`, `curl`, `dpkg-deb`,
`unsquashfs` (squashfs-tools), `python3` with venv support, `mke2fs` (e2fsprogs),
and `bwrap` (bubblewrap). QEMU's host shared-library dependencies must also be
available. The launcher extracts the emulator package locally rather than
installing it system-wide.

The host must permit Bubblewrap to create user and network namespaces. If
namespace creation is denied by the host or an enclosing sandbox, use an
execution environment that permits them. Do not remove the isolation options.

The script loads [shared host settings](workspace-settings.md) automatically.
Artifacts default to ignored `build/stock-qemu/`. To choose another private,
untracked location, set the same absolute path for every command:

```sh
export STOCK_QEMU_DIR=/tmp/stock-qemu
```

## Download the emulator and kernels

```sh
scripts/stock-qemu.sh download
```

This step requires host Internet access. It downloads `qemu-system-mips` from
the configured apt repository and the OpenWrt 25.12.5 Malta big-endian
`vmlinux.elf` and `vmlinux-initramfs.elf` images. Both kernel downloads are
checked against SHA-256 values pinned in the script. The emulator version
comes from the host's apt repository.

The upstream images and published checksums are in the
[OpenWrt download directory](https://downloads.openwrt.org/releases/25.12.5/targets/malta/be/).

Optionally verify the emulator and bundled OpenWrt userspace before preparing
the stock filesystem:

```sh
scripts/stock-qemu.sh boot openwrt
```

Press Enter when prompted for a console. Shut down inside the guest with
`poweroff -f`.

## Prepare local partition backups

Supply the SquashFS root partition and the JFFS2 application/data partition
from an existing backup. In the backup used for validation, these were named
`block2.img` and `block4.img`; verify the formats of your own files rather than
assuming that numbering is universal.

```sh
file /path/to/block2.img /path/to/block4.img
scripts/stock-qemu.sh prepare /path/to/block2.img /path/to/block4.img
```

Preparation extracts the root filesystem, extracts JFFS2 into `tuya-extracted/`,
and copies that partition into the guest's `/tuya`. It adds `/qemu-init.sh`
and creates a 64 MiB ext4 disk, `stock-root.ext4`. Source partition images are
read only. Extraction logs remain in the working directory.

If `jefferson` is unavailable on PATH, preparation downloads Jefferson 0.4.8
and its dependencies into a local Python virtual environment. That initial
installation also needs host Internet access.

Preparation refuses to overwrite an existing `rootfs/`, `tuya-extracted/`, or
`stock-root.ext4`. To prepare another backup, select a fresh working directory
and run the download step there first. Larger filesystems may exceed the
current 64 MiB disk size.

Extracted partitions can contain device credentials, keys, and network state.
Keep them private. Do not commit images, extracted files, downloads, logs, or
virtual environments. Only the launcher and documentation belong in Git.

## Boot the stock filesystem

```sh
scripts/stock-qemu.sh boot
```

The guest uses a Malta board, MIPS 24Kc CPU, 256 MiB RAM, and the downloaded
kernel. It mounts the stock disk and runs `/qemu-init.sh`, which mounts proc/sys,
creates basic device nodes, prepares runtime directories, and opens the stock
BusyBox shell as PID 1. The original hardware-dependent `rcS` is not run
automatically. A warning that shell job control is unavailable is expected.

Every boot enforces both of these restrictions:

- QEMU receives `-nic none`, so it has no virtual network adapter.
- Bubblewrap creates a separate network namespace with no connection to the
  host LAN or Internet. Isolation failure aborts startup.

No host gateway flash or serial device is passed through. The guest disk uses
QEMU's disposable snapshot overlay: filesystem changes disappear on shutdown.
The extracted host files and base ext4 image remain available for another run.

Inside the guest, check isolation and then stop it:

```sh
cat /proc/net/dev
cat /proc/net/route
poweroff -f
```

For the stock guest, expect only loopback and no network route. The bundled
OpenWrt image can additionally show a software radio simulation interface;
this does not connect to the host network.

## Try the Tuya application

In the stock guest shell, run the application directly:

```sh
cd /tuya/tuya_user2
LD_LIBRARY_PATH=/tuya ./tyZ3Gw /tuya/tuya_user2 /tuya/def.cfg \
  >/var/tmp/app.log 2>&1 &
sleep 5
grep -i 'fail\|error\|uart\|serial\|thread' /var/tmp/app.log | tail -n 20
ps
```

The application directory and configuration path above match the validated
backup; other firmware versions may use different paths. Logs can contain
private device information. Guest logs disappear with the snapshot overlay.
Stop the VM with `poweroff -f` when finished.

## Verified behavior and limitations

Validation used QEMU 8.2.2 and the downloaded OpenWrt Linux 6.12.94 kernel:

- Bundled OpenWrt booted to an interactive shell.
- The stock ext4 disk booted to its original BusyBox 1.13.4 shell.
- `tyZ3Gw` reported 12 successful thread creations and no thread-creation
  failures during the observed startup interval.
- The stock guest had only loopback and no network route; both VMs shut down
  successfully.

The earlier QEMU user-mode attempt rejected the stock library's LinuxThreads
`clone(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND)` call with `EINVAL`.
Full-system emulation delegates that call to a real guest kernel and passed
this startup point. Merely changing a reported kernel version in user-mode
emulation would not fix that restriction.

The Zigbee UART (`/dev/ttyS1`), Realtek Ethernet interfaces, flash devices,
GPIO controls, and Realtek-specific proc entries are not emulated. Missing
serial-device errors, network ioctl failures, and `Network is unreachable`
are expected. Successful application startup does not establish radio
functionality, full service health, or compatibility with every stock binary.

The external initramfs attempt with the kernel-only download did not mount a
root filesystem in the tested setup. The launcher therefore uses an IDE ext4
disk for stock userspace. The separate OpenWrt smoke test uses the image with
its initramfs already built in.
