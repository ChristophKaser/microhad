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
already has a network, use the [existing-network migration plan](ziggurat-deployment-plan.md)
instead of creating a fresh network or following the destructive reset steps.

## Choose your installation

| Mode | Radio application | Gateway Linux services | Zigbee application/state owner | Status |
| --- | --- | --- | --- | --- |
| [A: Native without Ziggurat](#mode-a-native-service-without-ziggurat) | EZSP NCP | Native | Native dashboard; radio stores network security | Previously deployed here; both backends tested |
| [B: Native with Ziggurat](#mode-b-native-service-with-ziggurat) | HDLC RCP | Native + Ziggurat | Native dashboard; native persists stack state | Real gateway deployment verified, device limitations remain |
| [C: Home Assistant without Ziggurat](#mode-c-home-assistant-without-ziggurat) | EZSP NCP | TCP serial bridge | External Home Assistant ZHA | Established upstream Silvercrest setup |
| [D: Home Assistant with Ziggurat](#mode-d-home-assistant-with-ziggurat) | HDLC RCP | Ziggurat WebSocket server | External Home Assistant ZHA | Upstream-supported client; this device combination untested |

Choose A for standalone operation with the simplest stack, or C if you already
use Home Assistant. B and D require replacing the radio application. None of
these modes runs Home Assistant itself on the small gateway. ZHA and native are
alternative coordinator clients: do not start both. The default build also supports a Rust Thread/Matter service; this guide
covers the Zigbee modes. See [Matter runtime](matter-service-runtime.md) and
[maintenance deployment](../microhad/AGENTS.md) for the paired stack. The HDLC
image alone does not provide a Thread border router or Matter controller.

## 1. Equipment, sources and host variables

Use a Linux development computer, Ethernet, the gateway's normal power supply,
and a **3.3 V TTL USB-to-UART adapter**. Connect only GND, TX and RX. Do not
connect the adapter's VCC, use 5 V logic or a PC RS-232 port. Keep serial-console
access until SSH and your selected boot launcher work.

Follow [build setup](building.md) to obtain the source checkouts, stock RSDK,
serial bridge and the binaries needed for your chosen mode. The RSDK is a
separately acquired artifact; neither repository supplies a verified download.
For UART tools on the host:

```sh
python3 -m venv "$WORKSPACE/console-venv"
"$WORKSPACE/console-venv/bin/pip" install pyserial cryptography
```

Host Bash variables used throughout, after changing the address to yours:

```sh
export GATEWAY_IP=192.168.1.50
export SERIAL_PORT=/dev/ttyUSB0
export PRIVATE_DIR="$HOME/silvercrest-private"
umask 077
mkdir -p "$PRIVATE_DIR"
chmod 700 "$PRIVATE_DIR"
export PASSWORD_FILE="$PRIVATE_DIR/ssh-password.txt"
export CONFIG_FILE="$PRIVATE_DIR/native-config.json"
```

`WORKSPACE`, `PROJECT_ROOT`, `RSDK`, `MIGRATION_PYTHON`, `NCP_IMAGE` and `RCP_IMAGE` come from
[build setup](building.md). Only Ziggurat modes need `RCP_IMAGE` built.
Store all credentials and backups outside Git. The address above is an example;
this guide does not use the development gateway's address.

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

The recovered password is device-specific; save it in `PASSWORD_FILE` with mode
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
ssh -p 2333 -o HostKeyAlgorithms=+ssh-rsa \
  -o PubkeyAcceptedAlgorithms=+ssh-rsa root@"$GATEWAY_IP"
```

Do not repeatedly guess passwords: factory SSH has a persistent failure lockout.
If port 2333 is unavailable, use the serial console to inspect the running
Dropbear/monitor scripts. Do not assume it already behaves like our test device.

## 4. Normalize SSH and preserve the startup files

The deployment and migration tools use **port 22** and do not offer a port flag.
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

On the **host**, register/verify the port-22 host key interactively, then define:

```sh
ssh -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa root@"$GATEWAY_IP"
gateway_ssh() {
    sshpass -f "$PASSWORD_FILE" ssh -o ConnectTimeout=8 \
      -o StrictHostKeyChecking=yes -o HostKeyAlgorithms=+ssh-rsa \
      -o PubkeyAcceptedAlgorithms=+ssh-rsa \
      -o PreferredAuthentications=password -o PubkeyAuthentication=no \
      root@"$GATEWAY_IP" "$@"
}
gateway_ssh 'tar -cf - /tuya/tuya_start.sh.factory-backup /tuya/ssh_monitor.sh.factory-backup' \
  > "$PRIVATE_DIR/factory-startup.tar"
gateway_ssh 'uname -a; cat /proc/meminfo; df -k /tuya; ps'
```

Transfer through SSH stdin, not SCP (old Dropbear has no SCP subsystem):

```sh
cat "$WORKSPACE/lidl-gateway-freedom/gateway/serialgateway" | \
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
preparation. Keep executables on `/tuya` and transient logs in RAM; large executable
uploads to `/tmp` previously caused suspected memory pressure.

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
existing working hook rather than overwriting it. Clock setup was already present
on the development device, so this factory prerequisite remains revision-dependent.

## 5. Establish a compatible EZSP baseline

Modes A/B use this project's tested NCP **7.5.1 / EZSP v13**, 115200 baud with
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
chip erase. The factory-to-baseline step has not been retested on a newly purchased
unit here. If a bootloader menu/upgrade is unavailable, retain console access and
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
   ../deploy.sh --host "$GATEWAY_IP" --password-file "$PASSWORD_FILE" \
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
network creation. Build Ziggurat, the maintenance helper, HDLC GBL and the enabled
native binary using [build setup](building.md).

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
  --host "$GATEWAY_IP" --password-file "$PASSWORD_FILE" \
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
  --host "$GATEWAY_IP" --password-file "$PASSWORD_FILE" \
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

The radio is now in its bootloader. On the **host**, restart the exact bridge
with explicitly disabled hardware flow for the 115200-baud bootloader:

```sh
gateway_ssh "kill -TERM $BRIDGE_PID"
gateway_ssh '/tuya/ziggurat-stage/gateway-maintenance check-free /dev/ttyS1'
gateway_ssh '/tuya/serialgateway -f -p 8888 -b 115200 >/dev/null 2>&1 & echo $! > /tmp/silvercrest-bridge.pid'
BRIDGE_PID=$(gateway_ssh 'cat /tmp/silvercrest-bridge.pid')
"$MIGRATION_PYTHON" scripts/ezsp_flash.py --image "$RCP_IMAGE" \
  --sha256 5e835cf429dbc25ded9e6217f620d0d2243c11c98935de0e29e387657063bb48 \
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
../deploy.sh --host "$GATEWAY_IP" --password-file "$PASSWORD_FILE" \
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
Allow up to three minutes before diagnosing a cold-start timeout: one verified
boot needed 121 seconds, with cause unresolved. Pair one device from the native
dashboard, verify actual reports/control, reboot, and verify again. Check a sleepy
device by waking it; an inventory row alone does not prove continuity.

**Maintenance/return path:** quiesce the pair, save a newly frozen complete stack
snapshot, prepare the advanced NCP recovery backup, stop the pair, enter the
bootloader using IID 1, restore pinned NCP application and current network state,
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

## Mode D: Home Assistant with Ziggurat

**Experimental on this gateway.** Upstream documents Ziggurat radio support in
**Home Assistant 2026.7.0 or newer**; this exact patched MIPS server/client
combination has not been exercised here. Confirm your installed ZHA offers the
Ziggurat radio type. See [upstream setup](https://github.com/zigpy/ziggurat#setup).

1. Complete sections 1–5. Build the patched MIPS Ziggurat, helper and two-instance
   RCP image. Native is not the runtime client for this mode.
2. With the idle launcher still installed and exactly one maintenance bridge
   owning UART, enter the bootloader on the **host**:

   ```sh
   cd "$PROJECT_ROOT/microhad"
   "$MIGRATION_PYTHON" scripts/ezsp_preflight.py bootloader-roundtrip \
     --host "$GATEWAY_IP" --exclusive-radio
   ```

   This command returns to NCP; it proves menu/run before replacement. Recheck
   the radio, then validate and flash the pinned RCP application on the host:

   ```sh
   "$MIGRATION_PYTHON" scripts/ezsp_flash.py --image "$RCP_IMAGE" \
     --sha256 5e835cf429dbc25ded9e6217f620d0d2243c11c98935de0e29e387657063bb48
   "$MIGRATION_PYTHON" scripts/ezsp_flash.py --image "$RCP_IMAGE" \
     --sha256 5e835cf429dbc25ded9e6217f620d0d2243c11c98935de0e29e387657063bb48 \
     --host "$GATEWAY_IP" --exclusive-radio --flash
   ```
   On this **fresh network only**, no native bootstrap/state import is needed:
   ZHA creates and subsequently persists the network through Ziggurat's API.
   If flow switching does not expose the menu, use the verified no-flow bridge
   procedure in B2 after explicit bootloader entry; do not continue without a
   recognized menu. Stop the exact maintenance bridge before starting Ziggurat.
3. On the **host**, upload the MIPS server and a dedicated boot launcher:

   ```sh
   gateway_ssh 'mkdir -p /tuya/ziggurat; chmod 700 /tuya/ziggurat'
   cat "$PROJECT_ROOT/build/gateway-maintenance" | \
     gateway_ssh 'cat > /tuya/ziggurat/gateway-maintenance.new && chmod 700 /tuya/ziggurat/gateway-maintenance.new && mv /tuya/ziggurat/gateway-maintenance.new /tuya/ziggurat/gateway-maintenance'
   cat "$PROJECT_ROOT/build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat.stripped" | \
     gateway_ssh 'cat > /tuya/ziggurat/ziggurat.new && chmod 700 /tuya/ziggurat/ziggurat.new && mv /tuya/ziggurat/ziggurat.new /tuya/ziggurat/ziggurat'
   gateway_ssh 'cat > /tuya/tuya_start.sh.new' <<'SH'
   #!/bin/sh
   while true; do
       /tuya/ziggurat/ziggurat --device /dev/ttyS1 --baudrate 460800 \
           --flow-control hardware --spinel-iid 1 --api ws \
           --listen 0.0.0.0:9999 --log-level error >/dev/null 2>&1
       sleep 3
   done &
   SH
   gateway_ssh 'chmod 755 /tuya/tuya_start.sh.new; mv /tuya/tuya_start.sh.new /tuya/tuya_start.sh'
   ```

   This example launcher is for the experimental ZHA mode; it is not the
   validated native paired supervisor. It restarts a crashed server but does
   not certify network recovery. Start it once from the gateway console after
   UART is free, then perform the reboot test below. No native service, TCP
   serial bridge or second Ziggurat server should run alongside it.
4. In **external Home Assistant ZHA**, choose manual connection, **Ziggurat**
   radio type and `ws://GATEWAY_IP:9999`. Create a new network, pair one device,
   and verify actual reports/control. ZHA owns durable network state and pushes
   configuration on startup; do not deploy native's private state into ZHA.
   Take a Home Assistant backup, reboot the gateway, restart/reconnect ZHA if
   needed, and require the same device to resume communication before acceptance.

The patched server's flags are `--device`, `--api ws` and `--listen`; do not copy
an upstream invocation with different historical CLI flags. Its API is binary
WebSocket protocol v2, not EZSP. Keep port 9999 on a trusted wired LAN because
this example has no TLS/authentication. The normal HA app URL
`ws://local-ziggurat:9999` refers to an app-hosted server, **not this gateway**.
This guide deliberately runs Ziggurat on the gateway and does not install that
second app server. The COBS socket from Mode B is not ZHA's endpoint.

**Return path:** stop ZHA and the exact Ziggurat supervisor/server; preserve ZHA
backups. RCP must be replaced with NCP before Mode C can work again. Preserving
an active ZHA/Ziggurat network requires current state/counters and a validated
conversion/restore procedure; this project's native freeze CLI is specific to
Mode B and does not directly freeze the WebSocket-only setup. Do not claim a
seamless Mode D rollback. For this disposable fresh test network, deliberately
start a new NCP/ZHA network and re-pair the test device after radio restoration.
After disabling ZHA and stopping the verified server **and its supervisor**, use
these host commands to restore the radio application only:

```sh
gateway_ssh '/tuya/ziggurat/gateway-maintenance check-free /dev/ttyS1'
gateway_ssh '/tuya/ziggurat/gateway-maintenance bootloader-enter /dev/ttyS1 --exclusive-radio'
gateway_ssh '/tuya/serialgateway -f -p 8888 -b 115200 >/dev/null 2>&1 & echo $! > /tmp/silvercrest-bridge.pid'
BRIDGE_PID=$(gateway_ssh 'cat /tmp/silvercrest-bridge.pid')
cd "$PROJECT_ROOT/microhad"
"$MIGRATION_PYTHON" scripts/ezsp_flash.py --image "$NCP_IMAGE" \
  --sha256 05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d \
  --host "$GATEWAY_IP" --exclusive-radio --already-in-bootloader --flash
gateway_ssh "kill -TERM $BRIDGE_PID"
```

Install the Mode C launcher, reboot, then deliberately create the replacement
ZHA network. This procedure discards continuity of the disposable Mode D test
network; do not apply it as a preserving rollback for paired production devices.
If the RCP does not respond to bootloader entry, software recovery is unproven;
keep the backups and diagnose instead of erasing the radio.

## Acceptance and troubleshooting

| Symptom | Check |
| --- | --- |
| No readable console | Correct PCB/pin orientation, common GND, crossed RX/TX, 3.3 V logic, 38400 8N1/no flow |
| SSH script cannot connect | Port 22 verified, accepted host key, password file mode/readability; factory port 2333 needs the earlier transition |
| Upload command expects SCP | Use SSH stdin; the old server has no SCP subsystem |
| Native deployment has no dashboard | Run `make webui`; the default deploy requires its generated assets |
| Native Ziggurat fails immediately | Correct enabled binary, initialized private state file, mode 0600, matching RCP identity, paired launcher |
| Wrong protocol/timeouts | EZSP firmware cannot serve HDLC; RCP cannot serve EZSP. Check baud, RTS/CTS and IID 1 |
| Duplicate UART ownership | Stop the actual competing supervisor and child; killing a child alone may respawn it |
| Slow native cold start | Observe `/api/logs` and RAM; one boot needed 121 seconds. Do not weaken entropy checks |
| Device listed but silent | Wake a sleepy device and verify an actual report; inspect TH01 reporting limitations |
| HA says unknown radio type | Check HA/ZHA version, select EmberZNet for Mode C or Ziggurat for Mode D |

For each installed mode, record firmware hash/version, binary revisions,
launcher, ports, state owner, one successful real-device action, and reboot result
privately. Retain backups off the gateway. None of the installation paths has
been rerun from a newly purchased device for this documentation change; verified
existing-device evidence and upstream-derived steps are distinguished above.
