# EZSP firmware maintenance

The native EZSP backend uses the pinned NCP 7.5.1 / EZSP v13 application at
115200 baud with hardware flow control. Radio uploads use the Gecko bootloader
at 115200 without flow control. NCP and HDLC RCP are different applications;
changing a Linux launcher does not switch between them.

- [Fresh installation](../../docs/installation.md#5-establish-a-compatible-ezsp-baseline) provides inspection and NCP upload commands.
- [Build setup](../../docs/building.md#prepare-installation-and-migration-tools) provides dependencies and the pinned NCP image path/hash.
- [Logical backup and conversion](../../docs/network-migration.md) describes current network/security capture.
- [Radio/network rollback](../../docs/ziggurat-deployment-plan.md#rollback) describes return from Ziggurat after transmission.
- [Maintenance helper](../../docs/recovery-tooling.md#radio-maintenance-helper) provides ownership and bootloader checks.

Use `ezsp_preflight.py` to inspect images/radio, `ezsp_flash.py` for guarded
application uploads, and `ezsp_restore.py` for logical restoration. Their defaults
inspect/validate; live flags explicitly enable device operations. Stop competing
radio owners and supervisors first. Never substitute an old logical backup for
current counters/keys or use a raw/full-chip image to repair an application update.
