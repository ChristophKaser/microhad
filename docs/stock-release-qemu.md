# Test a paired release in stock userspace with QEMU

These scripts run the paired release installer against a private copy of the
stock userspace. They require local inputs; they do not download firmware or
modify a physical gateway. QEMU user networking is restricted, with SSH and
HTTP forwarded only to host loopback. The guest has no Internet access.

Set paths to a local QEMU Malta emulator and compatible kernel, then prepare a
private guest from a rootfs tree and MTD3 backup:

```sh
export STOCK_RELEASE_QEMU_BIN=/path/to/qemu-system-mips
export STOCK_RELEASE_KERNEL=/path/to/malta-kernel.elf
scripts/stock-release-qemu.sh prepare /path/to/rootfs-tree \
  /path/to/mtd3.img /path/to/root-password-file
```

The rootfs tree must include the stock Dropbear/account files and
`lib/modules/6.12.94/e1000.ko`. The password file must contain the root
password derived from that MTD3 image and have mode `0600`. Preparation copies
the inputs into a private work directory (default
`/tmp/stock-release-qemu-<uid>`) and builds an ext4 guest disk. Set
`STOCK_RELEASE_QEMU_DIR` to choose a different work directory. Keep all inputs
and outputs private; they may contain device credentials or state.

Start an interactive guest with `scripts/stock-release-qemu.sh boot`. To test a
release package, run:

```sh
scripts/stock-release-qemu.sh install-paired /path/to/release-package
```

The optional second and third arguments select a config JSON and Ziggurat
state JSON. By default, the package's paired example config is used with a
synthetic Zigbee state built from repository fixtures; no real Zigbee keys are
used. Each run starts from a fresh copy of the prepared base disk, checks that
DHCP works and outbound Internet is unreachable, runs the package check and
installer, then stops QEMU. Installer changes therefore do not carry over to
the next run or update the base disk.

Console output, installer logs, and a PCAP of guest network traffic are kept in
the work directory (`guest-console.log`, `installer*.log`, and
`guest-network.pcap`). The default SSH and HTTP host ports are 2223 and 18081;
override them with `STOCK_RELEASE_SSH_PORT` and `STOCK_RELEASE_HTTP_PORT` if
they are already occupied. Disk size and emulator path can also be set with
`STOCK_RELEASE_DISK_SIZE` and `STOCK_RELEASE_QEMU_BIN`.

This setup emulates a Malta board, not the gateway's Realtek hardware. The
e1000 module provides guest networking, but the Zigbee NCP/UART and other
device-specific hardware are not emulated, so service readiness checks may
fail even when package preflight and installation work.
