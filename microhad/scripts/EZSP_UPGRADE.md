# EZSP upgrade and recovery record

## Current outcome (2026-09-23)

The native daemon supports v8 and v13 pairing commands. It has been tested
on the existing EZSP v8 radio. No radio firmware was flashed.
Host tests and the prescribed MIPS build passed.

Live radio: EmberZNet 6.7.8.0 build 373, Gecko Bootloader 1.8.0.
Entering the bootloader, reading its menu, and returning to the existing
application succeeded. The native supervisor was then resumed and the
dashboard reported online with the network formed and eight known devices.

## Private recovery assets

Local ignored directory: `diagnostics/ezsp-upgrade-recovery/` (mode 0700).
`native-before-v13.tar` contains the original daemon, configuration and launcher.
The original daemon MD5 is `1f0143402e484a2d612c3d3f94970abc`.
`network-current.json` is a fresh open-coordinator-format backup, including
network key, frame counter and hashed trust-center secret. Its two device
entries reflect radio-exported records, not the dashboard's complete device list.
Keep both backups private; neither is a full radio flash dump.
The historical 2023 backups are not current recovery data.

The local 6.7.8 rollback candidate is
`../radio_firmware/NCP_UHW_MG1B232_678_PA0-PA1-PB11_PA5-PA4.gbl`.
SHA256: `b1ecf15caad2407e404954defbf88153337ccaa78f7d9e3c856f971e782db735`.
Its image CRC is valid. A matching version is not proof that it is byte-identical
to the running firmware. Returning to it after a storage migration is unverified.

The 7.5.1 candidate is pinned to jnilo1/rtl8196e-gateway revision
`2cd6f3714c34adcdd503c8740a872c19ff24ce83`, path
`2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW/firmware/ncp-uart-hw-7.5.1-115200-hw.gbl`.
It has a valid CRC and no bootloader-update tag. Both application images start
at 0x4000. The candidate reserves 36 KiB for NVM3 in its project configuration.
Candidate SHA256: `05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d`.

## Why flashing is on hold

The currently installed token-storage format has not been established.
Silicon Labs makes SimEE2 migration conditional on the
`sim_eeprom2_to_nvm3_upgrade` component. The candidate project does not explicitly
include it; generated component-resolution output was not available to verify it.
Neither in-place network preservation nor reverse migration is established.
Do not infer safety from a valid image CRC or a working bootloader.

Before flashing, establish the current storage layout, verify migration in the
exact candidate build, and validate a restore path that preserves identity and
frame-counter monotonicity. Without that evidence, use an SWD programmer for
a full flash backup/recovery path. No SWD programmer was available in this run.

Sources:
- https://docs.silabs.com/gecko-platform/3.2/service/api/group-simeeprom2-to-nvm3-upgrade
- https://github.com/SiliconLabs/gecko_sdk/blob/gsdk_4.4/platform/service/token_manager/src/sl_token_manager.c
- https://github.com/jnilo1/rtl8196e-gateway/tree/2cd6f3714c34adcdd503c8740a872c19ff24ce83/2-Zigbee-Radio-Silabs-EFR32/24-NCP-UART-HW

## Reusable inspection procedure

Use an isolated host venv with `bellows==1.0.2`,
`universal-silabs-flasher==1.1.0`, `zigpy-cli==2.0.0` (this run used zigpy 2.2.0).
`ezsp_preflight.py image FILE...` is offline. It validates image CRC, rejects
bootloader updates and program ranges outside the Lidl application region.
It does not certify hardware compatibility or storage migration.

Before any live radio command, resolve current PIDs using `ps w`, `/proc/PID/status`
and `/proc/PID/cmdline`. Confirm the daemon's parent is the native launcher.
Never reuse PIDs from a previous session. Suspend that supervisor with SIGSTOP,
terminate only the exact daemon with SIGTERM, and verify the UART is released.
Start `/tuya/serialgateway -p 8888` with its output redirected into `/tmp`.
Do not start it alongside the native daemon.

With exclusive radio access:

```sh
umask 077
zigpy radio --baudrate 115200 ezsp socket://192.168.178.62:8888 backup PRIVATE_BACKUP.json
python scripts/ezsp_preflight.py radio --host 192.168.178.62 --exclusive-radio
python scripts/ezsp_preflight.py bootloader-roundtrip --host 192.168.178.62 --exclusive-radio
```

The script uses the existing bridge's TCP out-of-band commands to switch hardware
flow control, waits for the UART to reopen, reads the menu, and sends only `run`.
It never sends `upload`. If entry succeeded but menu reading failed, use the
`bootloader-return` command after checking bridge ownership. Verify the radio
answers EZSP before resuming normal operation.

Always finish by terminating the exact temporary bridge, checking that it has
exited, and sending SIGCONT to the verified supervisor. Check HTTP/HTTPS status,
network identity and live reports. If the application cannot be reached, stop;
do not erase, factory-reset, replace the bootloader, or create a new network.

## Daemon-only rollback

The previous executable was extracted under
`diagnostics/ezsp-upgrade-recovery/tuya/native/tuya-zigbee-native` and its checksum
matched the running pre-upgrade daemon. It can be redeployed with `deploy.sh`
using `--binary` and `--binary-only`, the explicit password-file pathname,
`--target /tuya/native`, and `--restart`. Do not replace configuration.
The legacy device's restart check can report failure despite a successful
restart; reconcile that using the running `/proc/PID/exe` checksum and dashboard.
Do not treat this daemon rollback as a radio/network rollback.
