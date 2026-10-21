# Install on a factory-new Silvercrest gateway

This guide retains **factory Linux** on the Lidl/Silvercrest gateway containing
an **RTL8196E** CPU and **EFR32MG1B232** radio, approximately 32 MiB RAM and
16 MiB SPI flash. Check the PCB/chip markings against the
[original teardown](https://paulbanks.org/projects/lidl-zigbee/).
Do not infer compatibility from the brand or apply these addresses to another
board, including the Sengled G4. Replacement Linux is a separate
[upstream installation path](https://github.com/jnilo1/rtl8196e-gateway/blob/main/docs/getting-started.md);
our factory-Linux binaries have not been validated on that replacement system.

A fresh device has no Zigbee devices whose pairings need preservation. If yours
already has a network, use the [existing-network migration runbook](ziggurat-deployment-plan.md)
instead of creating a fresh network or following the destructive reset steps.

Check [supported devices](supported-devices.md) before choosing accessories.

Application binaries can be installed without a compiler using the
[binary release installer](binary-releases.md). Complete the root-access, backups
and radio preparation below first. Choose `microhad-only` for compatible stock
EZSP firmware, or `paired` after the Ziggurat radio/network handover.

The HDLC radio also has a [prebuilt firmware package](../firmware/README.md#prebuilt-firmware-releases)
with its GBL, standalone SSH-assisted flashing script, README and radio license
notices. It supports replacement from either EZSP or HDLC and downloads a
checksum-pinned temporary serial bridge when needed. Follow root-access and
backup preparation first. **Replacing EZSP may require recreating the Zigbee
network**; use the [migration runbook](ziggurat-deployment-plan.md) to preserve it.

## Choose a backend

| Setup | Radio firmware | Runtime |
| --- | --- | --- |
| [Mode A: native EZSP](#mode-a-native-service-without-ziggurat) | EZSP NCP | MicroHAD owns the UART |
| [Mode B: native Ziggurat](#mode-b-native-service-with-ziggurat) | Two-instance HDLC RCP | Ziggurat owns UART; MicroHAD owns application/durable network state |
| [Mode C: external Home Assistant](#mode-c-home-assistant-without-ziggurat) | Compatible EZSP NCP | TCP bridge to an external ZHA instance |

The default application build enables Ziggurat and Matter. First establish
Zigbee with the matching backend, then add the Matter service using
[deployment](deployment.md) and [Thread setup](../microhad/docs/matter-dashboard.md).
The RCP image alone supplies neither a Matter controller nor a border router.
Only one coordinator client may own the radio. Home Assistant runs externally.

## 1. Equipment, sources and host settings

Use a Linux development computer, Ethernet, the gateway's normal power supply,
and a **3.3 V TTL USB-to-UART adapter**. Connect only GND, TX and RX. Do not
connect the adapter's VCC, use 5 V logic or a PC RS-232 port. Keep serial-console
access until SSH and your selected boot launcher work.

Use the [binary package](binary-releases.md) for prebuilt application binaries,
or follow [build setup](building.md) for source builds. Radio preparation and
existing-network migration remain separate from application installation. Gateway application builds support separately
acquired RSDK (the default) or source-built crosstool-NG.
The source-built external serial bridge uses its documented RSDK recipe; the
prebuilt firmware flasher can download the compatible upstream bridge. Deployment
examples below use RSDK artifact paths; crosstool-NG builds require explicit paths
and the compatibility checks in [toolchain selection](../config/TOOLCHAINS.md).
For UART tools on the host:

```sh
python3 -m venv "$WORKSPACE/console-venv"
"$WORKSPACE/console-venv/bin/pip" install pyserial cryptography
```

Create `workspace.local.env` from the root example and set your `GATEWAY_IP`,
`LAN_INTERFACE`, compiler paths and `SSH_PASSWORD_FILE`; see
[shared host settings](workspace-settings.md). For this first-install procedure,
choose a private password-file pathname even before recovering the password.
Use `SSH_PORT=2333` only if that is the verified factory SSH port; update it to
22 after the optional normalization below.

Load settings for the raw shell commands, then set installation-specific paths:

```sh
. "$PROJECT_ROOT/scripts/workspace-env.sh"
workspace_load "$PROJECT_ROOT"
: "${GATEWAY_IP:?Set GATEWAY_IP}" "${SSH_PASSWORD_FILE:?Set SSH_PASSWORD_FILE for first installation}"
export SERIAL_PORT=/dev/ttyUSB0
export PRIVATE_DIR="$HOME/silvercrest-private"
umask 077
mkdir -p "$PRIVATE_DIR"
chmod 700 "$PRIVATE_DIR"
export CONFIG_FILE="$PRIVATE_DIR/native-config.json"
```

`WORKSPACE`, `PROJECT_ROOT`, `MIGRATION_PYTHON`, `NCP_IMAGE` and `RCP_IMAGE` come from
[build setup](building.md); RSDK is needed only if building the serial bridge
from source. The firmware package and upstream prebuilt bridge need no compiler.
Only Mode B needs `RCP_IMAGE`: build it, or set it to the GBL extracted from the
prebuilt firmware package. For that package also set `RCP_PACKAGE_DIR` to its
absolute extraction directory and follow its host dependency setup.
Store all credentials and backups outside Git. The host settings file is
private and is never installed on the gateway.

## 2. Open the case and obtain root access

The original case has plastic clips. With power disconnected, open it and
identify J1 using the [PCB photographs and pinout](https://paulbanks.org/projects/lidl-zigbee/).
Do not determine orientation by guessing which end is pin 1.

| J1 pin | Gateway signal | USB adapter connection |
| --- | --- | --- |
| 1 | 3.3 V | Leave disconnected |
| 2 | GND | GND |
| 3 | RTL console TX | RX |
| 4 | RTL console RX | TX |
| 5, 6 | Radio SWDIO/SWCLK | Leave disconnected for UART installation |

Host command (only one program may hold the serial port):

```sh
picocom --baud 38400 --flow n --parity n --databits 8 "$SERIAL_PORT"
```

Power the gateway normally and press Escape early during boot until the
`<RealTek>` prompt appears. The RTL console is **38400, 8N1, no flow control**;
this is different from the radio UART's later 115200/460800 settings.

At the bootloader prompt, these **read-only console commands** copy label data
to RAM and display it:

```text
FLR 80000000 401802 16
DW 80000000 4
FLR 80000000 402002 32
DW 80000000 8
```

Run the decoder on the host and paste the requested lines privately:

```sh
"$WORKSPACE/console-venv/bin/python" \
  "$WORKSPACE/lidl-gateway-freedom/scripts/lidl_auskey_decode.py"
```

The recovered password is device-specific; save it in `SSH_PASSWORD_FILE` with mode
0600 using a local editor. The decoder displays secrets: do not share its output
or put it in a build log. This procedure follows
[Paul Banks' root-access instructions](https://paulbanks.org/projects/lidl-zigbee/root/).
If the bootloader/layout differs, stop rather than using these offsets blindly.

## 3. Back up the factory system

Before changing any script or radio firmware, save the complete 16 MiB SPI flash.
At the Realtek prompt, close picocom, then run this **host command**:

```sh
"$WORKSPACE/console-venv/bin/python" \
  "$WORKSPACE/lidl-gateway-freedom/scripts/dump_flash.py" \
  --serial-port "$SERIAL_PORT" --start-addr 0x0 --end-addr 0x1000000 \
  --output-file "$PRIVATE_DIR/factory-fullflash.bin"
stat -c '%s' "$PRIVATE_DIR/factory-fullflash.bin"
sha256sum "$PRIVATE_DIR/factory-fullflash.bin" > "$PRIVATE_DIR/factory-fullflash.sha256"
```

This slow UART dump can take hours. Require **16777216 bytes**, successful
completion and a retained checksum. A host SPI backup does not include the
separate EFR32 radio flash; retain the radio's identity/firmware information and
a compatible recovery image before changing its application.

Reconnect picocom and power-cycle normally without Escape to boot Linux.
Find the Ethernet/DHCP address in your router or run `ifconfig` from the serial
root shell. A factory SSH server commonly listens on **2333**. Verify its host
key interactively, then authenticate using the recovered password:

```sh
ssh -p "$SSH_PORT" -l "$SSH_USER" -o HostKeyAlgorithms=+ssh-rsa \
  -o PubkeyAcceptedAlgorithms=+ssh-rsa "$GATEWAY_IP"
```

Do not repeatedly guess passwords: factory SSH has a persistent failure lockout.
If port 2333 is unavailable, use the serial console to inspect the running
Dropbear/monitor scripts. Do not assume it already behaves like our test device.

## 4. Normalize SSH and preserve the startup files

The shared SSH wrapper uses `SSH_PORT` (default **22**). If the factory SSH
server remains on a verified custom port, configure that value instead; host
tools no longer require changing it. The normalization below is optional and
applies only to the documented factory layout.
On the **gateway's serial root console**, preserve factory scripts once:

```sh
umask 077
[ -f /tuya/tuya_start.sh.factory-backup ] || cp /tuya/tuya_start.sh /tuya/tuya_start.sh.factory-backup
[ -f /tuya/ssh_monitor.sh.factory-backup ] || cp /tuya/ssh_monitor.sh /tuya/ssh_monitor.sh.factory-backup
```

Inspect `/tuya/ssh_monitor.sh` and `ps` first. On the documented factory layout,
replace the customized monitor with an empty shell script, following the
[original SSH setup](https://paulbanks.org/projects/lidl-zigbee/ha/):

```sh
printf '#!/bin/sh\n' > /tuya/ssh_monitor.sh
chmod 755 /tuya/ssh_monitor.sh
```

Reboot using the available BusyBox `reboot` command and verify standard Dropbear
on port 22. Keep the serial console open so you can restore the backup if the
factory revision does not start standard Dropbear. Do not launch a second daemon
blindly or stop your only remote-access process.

On the **host**, update `SSH_PORT` in `workspace.local.env` to the verified
port (22 after normalization). Start a fresh host shell and reload the section-1
settings/variables so an already exported port cannot override the edited file.
Register/verify its host key interactively. Define a helper that uses the shared
credential wrapper:

```sh
workspace_load "$PROJECT_ROOT"
ssh -p "$SSH_PORT" -l "$SSH_USER" -o HostKeyAlgorithms=+ssh-rsa \
  -o PubkeyAcceptedAlgorithms=+ssh-rsa "$GATEWAY_IP"
gateway_ssh() {
    sh "$PROJECT_ROOT/scripts/workspace-ssh.sh" -- "$GATEWAY_IP" "$@"
}
gateway_ssh 'tar -cf - /tuya/tuya_start.sh.factory-backup /tuya/ssh_monitor.sh.factory-backup' \
  > "$PRIVATE_DIR/factory-startup.tar"
gateway_ssh 'uname -a; cat /proc/meminfo; df -k /tuya; ps'
```

The packaged firmware flasher provisions its own temporary serial bridge;
skip this manual bridge installation if that is all you need. The manual EZSP
baseline, network-capture and external Home Assistant procedures below use
`/tuya/serialgateway`. Obtain that binary without a compiler on the host:

```sh
curl --fail --location \
  https://github.com/banksy-git/lidl-gateway-freedom/releases/download/Release-1.2/serialgateway \
  --output "$PRIVATE_DIR/serialgateway"
echo "1321510921d97b59336f7e6dc80cc3e8b8eccb2b804c940ab264c666aec3e827  $PRIVATE_DIR/serialgateway" | sha256sum --check
```

Alternatively build it using [build setup](building.md) and substitute that
artifact below. Transfer through SSH stdin, not SCP (old Dropbear has no SCP subsystem):

```sh
cat "$PRIVATE_DIR/serialgateway" | \
  gateway_ssh 'cat > /tuya/serialgateway.new && chmod 755 /tuya/serialgateway.new && mv /tuya/serialgateway.new /tuya/serialgateway'
```

Install an idle launcher on the **gateway console** before a maintenance boot:

```sh
printf '#!/bin/sh\n' > /tuya/tuya_start.sh
chmod 755 /tuya/tuya_start.sh
reboot
```

After reboot inspect `ps`: factory Zigbee applications must be stopped before
opening `/dev/ttyS1`. Check the actual process names on your revision; do not
assume the factory processes are native or low.js. Retain the untouched factory
launcher. Permanent mode launchers below replace this idle launcher only after
preparation. Keep executables on `/tuya` and transient logs in RAM; check flash
and RAM before staging.

### Clock setup for native modes

The supplied native launchers call `/tuya/set_time.sh`, a site-local clock hook
that is **not provided by either repository**. Check `date` on the gateway.
A fresh device without a battery-backed clock may boot with an incorrect date;
correct time is required for TLS, alerts and useful timestamps.

If the factory BusyBox supplies `ntpd` with `-n -q -p` (check
`busybox ntpd --help` on the gateway console), install this hook there:

```sh
cat > /tuya/set_time.sh <<'SH'
#!/bin/sh
exec busybox ntpd -n -q -p pool.ntp.org
SH
chmod 755 /tuya/set_time.sh
/tuya/set_time.sh
```

Use your LAN NTP server instead of `pool.ntp.org` if appropriate. Verify DNS,
network access and `date` afterward. If this applet is absent, this hook will not
work: arrange a compatible clock-sync program before accepting unattended native
startup. For an initial test only, set the date from the serial console using the
format printed by `date --help`; this does not survive a power cycle. Retain an
existing working hook rather than overwriting it. Clock-sync availability
depends on the factory revision.

## 5. Establish a compatible EZSP baseline

Modes A/B use the pinned NCP **7.5.1 / EZSP v13**, 115200 baud with
RTS/CTS. Mode C can retain factory NCP firmware if current ZHA supports it;
otherwise use this same baseline. Record the original firmware before updating.

On the **gateway console**, while factory services and other clients are stopped:

```sh
/tuya/serialgateway -p 8888 -b 115200 >/dev/null 2>&1 &
```

On the **host**:

```sh
cd "$PROJECT_ROOT/microhad"
"$MIGRATION_PYTHON" scripts/ezsp_preflight.py radio \
  --host "$GATEWAY_IP" --exclusive-radio
```

This connection resets the NCP transport; do not run it beside a live coordinator.
If the radio is already the intended version, skip flashing. Otherwise, on this
fresh unpaired device, validate and upload the pinned NCP application:

```sh
"$MIGRATION_PYTHON" scripts/ezsp_flash.py --image "$NCP_IMAGE" \
  --sha256 05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d
"$MIGRATION_PYTHON" scripts/ezsp_flash.py --image "$NCP_IMAGE" \
  --sha256 05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d \
  --host "$GATEWAY_IP" --exclusive-radio --flash
"$MIGRATION_PYTHON" scripts/ezsp_preflight.py radio \
  --host "$GATEWAY_IP" --exclusive-radio
```

Require bootloader acceptance and EZSP v13 before continuing. Do not retry an
uncertain upload blindly. The image excludes bootloader and NVM3; this is not a
chip erase. If a bootloader menu/upgrade is unavailable, retain console access and
follow radio-specific recovery rather than flashing Linux or replacing the loader.
Keep TCP:8888 on a trusted LAN; the bridge has no authentication.

## Mode A: Native service without Ziggurat

1. Complete sections 1–5 and build the EZSP native binary and dashboard.
2. On the **host**, create a minimal local configuration (no TLS/cloud credentials
   are required to begin):

   ```sh
   cat > "$CONFIG_FILE" <<'JSON'
   {
     "serial_device": "/dev/ttyS1",
     "serial_baud": 115200,
     "http_port": 80,
     "https_port": 443,
     "runtime_dir": "/tuya/native"
   }
   JSON
   chmod 600 "$CONFIG_FILE"
   cd "$PROJECT_ROOT/microhad"
   ../deploy.sh \
     --binary build/mips/microhad --config "$CONFIG_FILE" \
     --target /tuya/native --activate
   ```

   `--activate` installs the EZSP native supervisor for the next boot. It backs
   up the preceding launcher as `.lowjs-backup`, even if no low.js was installed;
   the separate `.factory-backup` is your true original. Upload alone does not
   stop the serial bridge or restart any service.
3. Reboot from the **gateway console**. Require one native supervisor/daemon
   and no serialgateway/factory radio owner. The native daemon opens the UART.
4. On the **host**, check readiness and open `http://GATEWAY_IP/` in a browser:

   ```sh
   curl --connect-timeout 5 --max-time 8 "http://$GATEWAY_IP/api/status"
   curl --connect-timeout 5 --max-time 8 "http://$GATEWAY_IP/api/logs"
   ```

   Require `coordinator_backend: "ezsp"` and `online: true`. On this fresh device,
   select **Create fresh network** and enter the requested `DELETE` confirmation;
   this intentionally replaces any radio network left by factory testing.
   Wait for `network_forming: false` and `network_formed: true`.
5. Select **Allow joining (5 min)**, put one device in pairing mode, then verify
   an actual report/control and its last-seen time. Reboot once and confirm the
   same device still communicates. Joining should expire/close afterward.

For HTTPS, certificates, Browser Push, FCM, scenes and external devices see
[native usage](native-usage.md).
Notifications are optional and must not require copying another gateway's keys.

**Return path:** stop the exact native supervisor/daemon, restore the saved boot
launcher or choose Mode C, and reboot. Since the radio remains NCP, changing
Linux startup does not require an RCP-to-NCP flash. However, do not assume the
vendor application can use a newly created network as its original cloud network.
Keep factory recovery and coordinator backups separately.

## Mode B: Native service with Ziggurat

Start by completing Mode A through **creation of an empty network**, but do not
pair devices yet. This avoids inventing network keys/state: the native Ziggurat
client requires a prepared private state file and does not implement dashboard
network creation. Build Ziggurat, the maintenance helper and HDLC GBL using
[build setup](building.md), then build the Zigbee-only native binary:

```sh
make -C "$PROJECT_ROOT/microhad" ZIGGURAT=1 MATTER=0 mips
```

### B1. Stage, export and stop the empty NCP network

On the **host**, stage Ziggurat/helper under a separate directory; check flash
space first. The helper is reused by final capture and ownership checks:

```sh
gateway_ssh 'mkdir -p /tuya/ziggurat-stage; chmod 700 /tuya/ziggurat-stage; df -k /tuya'
cat "$PROJECT_ROOT/build/gateway-maintenance" | \
  gateway_ssh 'cat > /tuya/ziggurat-stage/gateway-maintenance.new && chmod 700 /tuya/ziggurat-stage/gateway-maintenance.new && mv /tuya/ziggurat-stage/gateway-maintenance.new /tuya/ziggurat-stage/gateway-maintenance'
cat "$PROJECT_ROOT/build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat.stripped" | \
  gateway_ssh 'cat > /tuya/ziggurat-stage/ziggurat.new && chmod 700 /tuya/ziggurat-stage/ziggurat.new && mv /tuya/ziggurat-stage/ziggurat.new /tuya/ziggurat-stage/ziggurat'
cd "$PROJECT_ROOT/microhad"
"$MIGRATION_PYTHON" scripts/network_backup.py collect \
  --host \
  --output "$PRIVATE_DIR/empty-running-export.json"
gateway_ssh 'ps'
```

Identify the **native parent supervisor PID and its daemon PID** from `ps` and
`/proc/PID/cmdline`/`status`, not by assuming a PID from this guide. Set those two
verified numeric values in the host shell as `SUPERVISOR_PID` and `NATIVE_PID`.
Suspend only that supervisor, then gracefully stop its child:

```sh
gateway_ssh "kill -STOP $SUPERVISOR_PID; kill -TERM $NATIVE_PID"
```

Wait for the daemon to release UART. If it remains a zombie under the suspended
parent, that alone does not mean it owns UART. On the **host**:

```sh
gateway_ssh '/tuya/ziggurat-stage/gateway-maintenance check-free /dev/ttyS1'
gateway_ssh 'tar -cf - /tuya/native /tuya/tuya_start.sh' > "$PRIVATE_DIR/empty-ezsp-service.tar"
gateway_ssh '/tuya/serialgateway -p 8888 -b 115200 >/dev/null 2>&1 & echo $! > /tmp/silvercrest-bridge.pid'
BRIDGE_PID=$(gateway_ssh 'cat /tmp/silvercrest-bridge.pid')
"$MIGRATION_PYTHON" scripts/ezsp_final_capture.py \
  --compare "$PRIVATE_DIR/empty-running-export.json" \
  --output "$PRIVATE_DIR/empty-final-export.json" \
  --host \
  --bridge-pid "$BRIDGE_PID" --supervisor-pid "$SUPERVISOR_PID" \
  --exclusive-radio --capture --enter-bootloader
"$MIGRATION_PYTHON" scripts/network_backup.py convert \
  --input "$PRIVATE_DIR/empty-final-export.json" \
  --output "$PRIVATE_DIR/empty-final-state.json" \
  --coordinator-output "$PRIVATE_DIR/empty-ncp-backup.json"
"$MIGRATION_PYTHON" scripts/ziggurat_migration.py prepare-import \
  --state "$PRIVATE_DIR/empty-final-state.json" \
  --output "$PRIVATE_DIR/empty-import.json"
"$MIGRATION_PYTHON" scripts/ziggurat_migration.py prepare-native \
  --state "$PRIVATE_DIR/empty-import.json" \
  --output "$PRIVATE_DIR/native-ziggurat-state.json"
```

Each output must be new inside the private directory. Require all validations
before uploading RCP. Final capture resumes the saved NCP network and can transmit;
it captures state and enters bootloader on the same connection. Do not resume the
old coordinator after preparing the handover. For interruption/rollback use the
[checkpoint procedure](ziggurat-deployment-plan.md), not a stale export.

### B2. Flash the application-only RCP

If using the prebuilt package, the final capture above has already entered the
bootloader. Stop its exact bridge, then invoke the packaged uploader on the
host; do not reconnect the old EZSP coordinator:

```sh
gateway_ssh "kill -TERM $BRIDGE_PID"
"$MIGRATION_PYTHON" "$RCP_PACKAGE_DIR/flash.py" \
  --host "$GATEWAY_IP" --radio ezsp --already-in-bootloader \
  --acknowledge-ezsp-replacement --flash
```

The package validates its bundled hash/CRC/ranges, provisions its own temporary
bridge if necessary, requires upload acknowledgment and checks an IID1 Spinel
version reply from the new radio. It does not import the network or resume
EZSP services. After success continue at **B3**. Its standalone script uses
normal SSH authentication; use `--ssh-port`, `--identity`, or `--password-file`
(with host `sshpass`) where needed.
It does not read the repository's private workspace settings.

For the source-built GBL and repository uploader instead, use the remaining
commands in this subsection:

Inspect the built RCP image with `ezsp_preflight.py image "$RCP_IMAGE"`, review
its application ranges, and set `RCP_SHA256` to the accepted artifact hash.
The radio is now in its bootloader. On the **host**, restart the exact bridge
with explicitly disabled hardware flow for the 115200-baud bootloader:

```sh
gateway_ssh "kill -TERM $BRIDGE_PID"
gateway_ssh '/tuya/ziggurat-stage/gateway-maintenance check-free /dev/ttyS1'
gateway_ssh '/tuya/serialgateway -f -p 8888 -b 115200 >/dev/null 2>&1 & echo $! > /tmp/silvercrest-bridge.pid'
BRIDGE_PID=$(gateway_ssh 'cat /tmp/silvercrest-bridge.pid')
"$MIGRATION_PYTHON" scripts/ezsp_flash.py --image "$RCP_IMAGE" \
  --sha256 "$RCP_SHA256" \
  --host "$GATEWAY_IP" --exclusive-radio --already-in-bootloader --flash
gateway_ssh "kill -TERM $BRIDGE_PID"
gateway_ssh '/tuya/ziggurat-stage/gateway-maintenance check-free /dev/ttyS1'
```

Require successful upload acknowledgment; never flash a raw `.bin`, bootloader,
full-chip image or NVM erase. An unrecognized bootloader menu or partial upload
requires inspection, not an automatic second attempt. The retained NCP image and
fresh logical backup are the recovery assets.

### B3. Install the permanent pair

On the **host**, add `ziggurat_socket` to the minimal native configuration:

```sh
python3 - "$CONFIG_FILE" <<'PY'
import json, sys
from pathlib import Path
p = Path(sys.argv[1])
x = json.loads(p.read_text())
x['ziggurat_socket'] = '/tmp/ziggurat/native.sock'
p.write_text(json.dumps(x, indent=2) + '\n')
PY
cd "$PROJECT_ROOT/microhad"
../deploy.sh \
  --binary build/mips-ziggurat/microhad \
  --config "$CONFIG_FILE" --target /tuya/native
cat "$PRIVATE_DIR/native-ziggurat-state.json" | \
  gateway_ssh 'umask 077; cat > /tuya/native/ziggurat-state.json.new && chmod 600 /tuya/native/ziggurat-state.json.new && mv /tuya/native/ziggurat-state.json.new /tuya/native/ziggurat-state.json'
gateway_ssh 'mv /tuya/ziggurat-stage/ziggurat /tuya/native/ziggurat; mv /tuya/ziggurat-stage/gateway-maintenance /tuya/native/gateway-maintenance'
cat paired_start.sh | gateway_ssh 'cat > /tuya/native/paired_start.sh.new && chmod 755 /tuya/native/paired_start.sh.new && mv /tuya/native/paired_start.sh.new /tuya/native/paired_start.sh'
cat scripts/paired_control.sh | gateway_ssh 'cat > /tuya/native/paired_control.sh.new && chmod 755 /tuya/native/paired_control.sh.new && mv /tuya/native/paired_control.sh.new /tuya/native/paired_control.sh'
cat ziggurat_start.sh | gateway_ssh 'cat > /tuya/tuya_start.sh.new && chmod 755 /tuya/tuya_start.sh.new && mv /tuya/tuya_start.sh.new /tuya/tuya_start.sh'
```

Do **not** add `--activate`: it installs the wrong, native-only launcher for this
mode. Before the first manual start, retire the suspended old supervisor without
letting it respawn the old client. Verify its stopped state/PID again; on the host:

```sh
gateway_ssh "kill -KILL $SUPERVISOR_PID"
gateway_ssh '/tuya/tuya_start.sh'
```

Require `/api/status` backend `ziggurat`, online/formed, preserved coordinator
identity and advanced counters. The native service owns
`/tuya/native/ziggurat-state.json`; Ziggurat owns UART at 460800/RTS-CTS.
The COBS socket is `/tmp/ziggurat/native.sock`, private to the local processes.
Cold startup can wait for kernel entropy; inspect readiness and logs before
diagnosing a timeout. Pair one device from the native
dashboard, verify actual reports/control, reboot, and verify again. Check a sleepy
device by waking it; an inventory row alone does not prove continuity.

**Maintenance/return path:** quiesce the pair, save a newly frozen complete stack
snapshot, prepare the advanced NCP recovery backup, stop the pair, enter the
bootloader through the maintenance helper, restore the pinned NCP application
and current network state,
then restore the archived EZSP application/launcher. Exact commands and failure
checkpoints are in the [rollback plan](ziggurat-deployment-plan.md) and
[recovery tools](recovery-tooling.md). Once Ziggurat transmits, the earlier empty
NCP export is no longer sufficient on its own. A launcher replacement cannot
turn RCP back into NCP. Live reverse restoration remains untested here.

## Mode C: Home Assistant without Ziggurat

1. Complete sections 1–4. Use supported factory NCP firmware or the section-5
   NCP baseline. Build/upload only the serial bridge for normal runtime; the
   native service and Ziggurat do not run.
2. On the **gateway console**, install a supervised bridge launcher:

   ```sh
   cat > /tuya/tuya_start.sh <<'SH'
   #!/bin/sh
   while true; do
       /tuya/serialgateway -p 8888 -b 115200 >/dev/null 2>&1
       sleep 2
   done &
   SH
   chmod 755 /tuya/tuya_start.sh
   reboot
   ```

   Keep the factory backup. Check `ps` after reboot for one bridge and no other
   radio owner. Only one TCP client is supported; a second replaces the first.
3. On the **external Home Assistant instance**, add **Zigbee Home Automation**,
   choose manual connection and radio type **EmberZNet/EZSP**, then enter:

   ```text
   socket://GATEWAY_IP:8888
   ```

   Replace `GATEWAY_IP` with the actual address. The bridge sets radio baud and
   RTS/CTS; the TCP connection does not configure those remotely. Follow ZHA's
   fresh-network setup, then permit joining and pair one test device.
4. Verify a real attribute report/control in ZHA, restart the gateway and ZHA,
   and verify the device again. Back up Home Assistant/ZHA before later changes.

This follows the [original Silvercrest Home Assistant setup](https://paulbanks.org/projects/lidl-zigbee/ha/)
and [ZHA documentation](https://www.home-assistant.io/integrations/zha/).
An EZSP TCP bridge does not expose our native dashboard or native push features.
Keep port 8888 on a trusted wired LAN, never forwarded to the internet.

**Return path:** stop/disable ZHA before stopping the exact bridge supervisor
and process; restore the chosen NCP native or factory launcher and reboot.
Do not run native against a ZHA network without a deliberate application/network
migration. Retain ZHA backup and the radio application version.

## Acceptance and troubleshooting

| Symptom | Check |
| --- | --- |
| No readable console | Correct PCB/pin orientation, common GND, crossed RX/TX, 3.3 V logic, 38400 8N1/no flow |
| SSH script cannot connect | Verify `GATEWAY_IP`, `SSH_PORT`, `SSH_USER`, accepted host key and credential-file mode/readability; factory port 2333 can be configured directly |
| Upload command expects SCP | Use SSH stdin; the old server has no SCP subsystem |
| Native deployment has no dashboard | Run `make webui`; the default deploy requires its generated assets |
| Native Ziggurat fails immediately | Correct enabled binary, initialized private state file, mode 0600, matching RCP identity, paired launcher |
| Wrong protocol/timeouts | EZSP firmware cannot serve HDLC; RCP cannot serve EZSP. Check baud, RTS/CTS and IID 1 |
| Duplicate UART ownership | Stop the actual competing supervisor and child; killing a child alone may respawn it |
| RNG unavailable during Zigbee key creation | Retry after kernel entropy is ready; existing networks start without a Zigbee RNG wait |
| Device listed but silent | Wake a sleepy device and verify an actual report; check [sensor reporting](sensor-reporting.md) |
| HA says unknown radio type | Check installed ZHA support and select EmberZNet/EZSP for Mode C |

For each installed mode, record firmware hash/version, binary revisions,
launcher, ports, state owner, one successful real-device action, and reboot result
privately. Retain backups off the gateway. Factory revisions can differ; verify each preparation step before continuing.
