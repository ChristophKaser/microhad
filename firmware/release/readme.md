# EFR32 HDLC multipan firmware @VERSION@

Application-only firmware for the EFR32MG1B232F256GM48 in the supported
Lidl/Silvercrest gateway. The resulting radio uses HDLC/Spinel multipan at
460800 baud with RTS/CTS, Zigbee IID1 and Thread IID2. This package replaces
the radio application only; it does not install Ziggurat or gateway services.

GBL SHA-256: `@IMAGE_SHA256@`

@PROVENANCE@

## Host setup and validation

Use a Linux host with Python 3.11+, OpenSSH, and network access to the gateway.
The gateway needs root SSH access and its existing supported Gecko bootloader.
Register and verify the gateway's SSH host key before running the script.
Standard SSH keys/agent or interactive password authentication are supported;
`--ssh-port` and `--identity` select a nondefault port/key. Legacy Dropbear RSA
compatibility is enabled only for this SSH invocation.
For unattended password authentication, install host `sshpass` and supply
`--password-file /private/gateway-password`; the password is never a command-line
argument or copied to the gateway. The script supports stock BusyBox without
`stat`, `readlink`, `tr`, `mktemp`, `id` or `netstat` applets.

```sh
python3 -m venv .venv
.venv/bin/pip install bellows==1.0.2 universal-silabs-flasher==1.1.0
.venv/bin/python flash.py
```

Validation checks the bundled hash, complete GBL CRC and application ranges.
Without `--flash`, no gateway connection or download is made.
No SDK, compiler or repository checkout is needed to use the package.

## HDLC update

If the paired stack is running, first follow the
[orderly stop and frozen export procedure](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/recovery-tooling.md#orderly-stop-and-frozen-zigbee-export).
Quiesce and save the private frozen snapshot; leave the supervisor quiesced
so the script can stop it and restart the recognized paired launcher after
success. The flag below asserts that you have saved that snapshot.

```sh
.venv/bin/python flash.py --host 192.168.1.50 --radio hdlc \
  --frozen-snapshot-saved --flash
```

The script enters the bootloader using Spinel IID1 through a temporary bridge,
uploads the GBL, verifies an IID1 version reply, and restarts a paired stack it stopped itself. An already
stopped stack remains stopped. Verify online/formed status and real device
traffic afterward. Do not reset network state to fix a missing report.

## Replacing EZSP

**WARNING: YOUR ZIGBEE NETWORK WILL PROBABLY NEED TO BE RECREATED.**
Replacing EZSP with HDLC does not migrate coordinator state. To preserve an
existing network, complete the preparation and final capture in the
[Ziggurat migration guide](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/ziggurat-deployment-plan.md)
before flashing. Retain private logical backups and the original NCP recovery
image. The acknowledgment flag accepts replacement, not successful migration.

After a final capture has already entered the bootloader, stop the capture's
bridge and use:

```sh
.venv/bin/python flash.py --host 192.168.1.50 --radio ezsp \
  --already-in-bootloader --acknowledge-ezsp-replacement --flash
```

For direct replacement where preserving the old network is not required:

```sh
.venv/bin/python flash.py --host 192.168.1.50 --radio ezsp \
  --acknowledge-ezsp-replacement --flash
```

This enters the bootloader via EZSP. Recognized native EZSP services are
stopped and remain stopped. Factory services, external ZHA and unknown
supervisors must be stopped separately. Do not reboot into the old coordinator
launcher: install/configure the compatible Ziggurat stack using the
[installation guide](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/installation.md).
The script does not change the installed boot launcher.

## Temporary bridge and downloads

An installed, byte-matching `/tuya/serialgateway` is reused. If missing or
different, the host downloads the MIPS static bridge from
[Paul Banks' Release-1.2](https://github.com/banksy-git/lidl-gateway-freedom/releases/tag/Release-1.2),
checks SHA-256
`1321510921d97b59336f7e6dc80cc3e8b8eccb2b804c940ab264c666aec3e827`,
and transfers it over SSH stdin to a private directory under `/tmp`.
The gateway does not need curl, Internet access, Python or SCP. Existing
`/tuya/serialgateway` is never overwritten.

For an offline host, supply a previously downloaded copy:

```sh
.venv/bin/python flash.py --host 192.168.1.50 --radio hdlc \
  --serialgateway /private/serialgateway --frozen-snapshot-saved --flash
```

The same checksum is required. The bridge uses TCP port 8888 unless `--port`
is supplied. Keep this unauthenticated port on a trusted LAN and stop external
clients. An existing bridge/UART owner or port listener is refused. Temporary
bridge files and owned processes are cleaned up after the operation. A remote
maintenance lock prevents concurrent invocations. On failure its private
diagnostic directory is retained and its path is printed.

## Failure and recovery

The uploader requires a recognized Gecko menu and successful upload acknowledgment.
It never updates the bootloader, erases NVM, flashes raw BIN images or retries
an uncertain upload automatically. On failure, services remain stopped for
inspection. Check SSH, the printed temporary directory's `bridge.log`, UART ownership and
the bootloader/application before retrying. An unresponsive radio may need
hardware recovery; software bootloader entry cannot repair it.

Preserve configuration, keys, Matter identity and all backups. Follow the
[migration rollback checkpoints](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/ziggurat-deployment-plan.md#rollback)
instead of restoring a stale Zigbee snapshot. See `license.md` for radio-specific
licenses; the combined firmware is not uniformly MIT licensed. Packaging does
not remove the SDK redistribution review required before publishing a release.
