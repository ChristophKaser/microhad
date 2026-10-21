#!/usr/bin/env python3
"""Radio release tests: temporary files, loopback sockets and mocked SSH only."""
import argparse
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import importlib.util
import io
from pathlib import Path
import socket
import struct
import subprocess
import tarfile
import tempfile
import threading
import unittest
from unittest.mock import AsyncMock, Mock, patch
import zlib

ROOT = Path(__file__).resolve().parents[2]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FLASH = load("radio_flash_test", "firmware/release/flash.py")
BUILD = load("radio_build_test", "firmware/scripts/build-release.py")
NOTICES = load("radio_notices_test", "firmware/scripts/release-notices.py")


def image(tags=None):
    def tag(identifier, value):
        return struct.pack("<II", identifier, len(value)) + value
    if tags is None:
        tags = [(0xFD0303FD, struct.pack("<I", 0x4000) + b"\0" * 16)]
    data = tag(0x03A617EB, struct.pack("<II", 0x03000000, 0))
    data += b"".join(tag(identifier, value) for identifier, value in tags)
    data += struct.pack("<II", 0xFC0404FC, 4)
    return data + struct.pack("<I", zlib.crc32(data))


def args(**kwargs):
    values = dict(host="127.0.0.1", radio="hdlc", port=8888, ssh_port=22,
                  identity=None, password_file=None, serialgateway=None, frozen_snapshot_saved=False,
                  already_in_bootloader=False)
    values.update(kwargs)
    return argparse.Namespace(**values)


def process(exe, parent=1, owner=False, start="123"):
    return dict(exe=exe, parent=parent, owner=owner, start=start)


class Images(unittest.TestCase):
    def validate(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.gbl"
            path.write_bytes(data)
            return FLASH.validate_image(path, hashlib.sha256(data).hexdigest())

    def test_valid_application_crc_and_ranges(self):
        self.validate(image())
        with self.assertRaises(ValueError):
            self.validate(image()[:-1] + b"X")

    def test_reject_bootloader_nvm_compression_and_no_program(self):
        cases = [[], [(0xF50909F5, b"\0" * 8)], [(0x5EA617EB, b"\0" * 8)],
                 [(0xFD0505FD, b"\0" * 8)], [(0xFD0707FD, b"\0" * 8)],
                 [(0xFD0303FD, struct.pack("<I", 0x3FFC) + b"\0" * 8)],
                 [(0xFD0303FD, struct.pack("<I", 0x36FFC) + b"\0" * 8)],
                 [(0xFD0303FD, b"\0")]]
        for tags in cases:
            with self.subTest(tags=tags), self.assertRaises(ValueError):
                self.validate(image(tags))

    def test_hash_mismatch_precedes_image_parse(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.gbl"
            path.write_bytes(image())
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                FLASH.validate_image(path, "0" * 64)

    def test_ezsp_warning_and_ack_before_dependencies_or_ssh(self):
        output = io.StringIO()
        with patch.object(FLASH, "dependencies") as dependencies, patch.object(FLASH, "Gateway") as gateway:
            with redirect_stderr(output), self.assertRaises(SystemExit):
                FLASH.main(["--radio", "ezsp", "--host", "127.0.0.1", "--flash"])
        self.assertIn("PROBABLY", output.getvalue().upper())
        self.assertIn(FLASH.MIGRATION_URL, output.getvalue())
        dependencies.assert_not_called()
        gateway.assert_not_called()

    def test_offline_cli_never_downloads_or_connects(self):
        with patch.object(FLASH, "dependencies"), patch.object(FLASH, "validate_image"), \
                patch.object(FLASH, "Gateway") as gateway, patch.object(FLASH, "bridge_bytes") as download:
            FLASH.main([])
        gateway.assert_not_called()
        download.assert_not_called()

    def test_host_argument_cannot_inject_ssh_options(self):
        for value in ("-oProxyCommand=evil", "root@host", "host;false", "999.1.1.1"):
            with self.assertRaises(argparse.ArgumentTypeError):
                FLASH.host(value)


class Transport(unittest.TestCase):
    def server(self, handler):
        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        server.settimeout(3)
        errors = []
        def worker():
            try:
                connection, _ = server.accept()
                with connection:
                    connection.settimeout(3)
                    handler(connection)
            except Exception as error:
                errors.append(error)
            finally:
                server.close()
        thread = threading.Thread(target=worker)
        thread.start()
        self.addCleanup(thread.join, 4)
        return server.getsockname()[1], errors

    def test_reset_frame_matches_existing_maintenance_protocol(self):
        simulator = load("radio_simulator", "ziggurat/scripts/simulate-rcp.py")
        self.assertEqual(simulator.decode(FLASH.spinel_reset()[1:-1]), b"\x90\x01\x03")

    def test_bootloader_menu_required_before_upload(self):
        def handler(connection):
            self.assertEqual(connection.recv(1), b"\n")
            connection.sendall(b"Gecko Bootloader v1.8\n1. upload gbl\n2. run\nBL >")
        port, errors = self.server(handler)
        FLASH.menu("127.0.0.1", port)
        self.assertFalse(errors)
        port, _ = self.server(lambda connection: (connection.recv(1), connection.sendall(b"unknown BL >")))
        with self.assertRaisesRegex(ValueError, "menu"):
            FLASH.menu("127.0.0.1", port)

    def test_hdlc_version_requires_valid_crc_and_iid(self):
        def handler(connection):
            self.assertEqual(connection.recv(256), FLASH.hdlc(b"\x91\x02\x02"))
            # Wrong IID and malformed CRC must not satisfy the version check.
            connection.sendall(FLASH.hdlc(b"\x81\x06\x02wrong\0"))
            connection.sendall(b"\x7e\x91\x06\x02invalid\0\0\0\x7e")
            connection.sendall(FLASH.hdlc(b"\x91\x06\x02OPENTHREAD/test\0"))
        port, errors = self.server(handler)
        FLASH.verify_hdlc("127.0.0.1", port)
        self.assertFalse(errors)
        port, _ = self.server(lambda connection: (connection.recv(256), connection.sendall(b"not hdlc")))
        with self.assertRaisesRegex(ValueError, "version query"):
            FLASH.verify_hdlc("127.0.0.1", port)


class GatewaySafety(unittest.TestCase):
    def gateway(self, **kwargs):
        token = patch.object(FLASH.secrets, "token_hex", return_value="test")
        token.start()
        self.addCleanup(token.stop)
        gateway = FLASH.Gateway(args(**kwargs))
        gateway.remote = Mock(return_value="")
        gateway.processes = Mock(return_value={})
        return gateway

    def test_unknown_or_existing_bridge_owner_is_never_killed(self):
        for executable in ("/tuya/serialgateway", "/tuya/factory-daemon", "/tmp/unknown"):
            gateway = self.gateway()
            gateway.processes.return_value = {55: process(executable, owner=True)}
            with self.assertRaisesRegex(ValueError, "UART owner"):
                gateway.stop_services()
            gateway.remote.assert_not_called()

    def test_running_pair_needs_saved_frozen_snapshot(self):
        gateway = self.gateway()
        gateway.processes.return_value = {12: process("/tuya/native/paired_start.sh"),
                                         13: process("/tuya/native/ziggurat", parent=12, owner=True)}
        with self.assertRaisesRegex(ValueError, "frozen-snapshot-saved"):
            gateway.stop_services()
        gateway.remote.assert_not_called()

    def test_pair_checks_supervisor_identity_then_uses_orderly_stop(self):
        gateway = self.gateway(frozen_snapshot_saved=True)
        gateway.processes.side_effect = [
            {12: process("/tuya/native/paired_start.sh"),
             13: process("/tuya/native/ziggurat", parent=12, owner=True)}, {}, {}]
        gateway.remote.side_effect = ["", "12", "", ""]
        gateway.stop_services()
        self.assertTrue(gateway.paired_running)
        self.assertIn("stop --frozen-snapshot-saved", gateway.remote.call_args.args[0])

    def test_native_with_unknown_parent_never_signaled(self):
        gateway = self.gateway(radio="ezsp")
        gateway.processes.return_value = {13: process("/tuya/native/microhad", parent=99, owner=True)}
        with self.assertRaisesRegex(ValueError, "supervisor"):
            gateway.stop_services()
        gateway.remote.assert_not_called()

    def test_prepared_migration_leaves_suspended_supervisor_untouched(self):
        gateway = self.gateway(radio="ezsp", already_in_bootloader=True)
        supervisor = process("/tuya/tuya_start.sh")
        supervisor["status"] = "T"
        gateway.processes.return_value = {12: supervisor}
        gateway.stop_services()
        self.assertEqual(gateway.remote.call_count, 1)
        self.assertNotIn("kill", gateway.remote.call_args.args[0])

    def test_process_parser_includes_start_time_and_stop_status(self):
        processes = FLASH.parse_processes("12\t1 123 T\t0\t/tuya/tuya_start.sh\n")
        self.assertEqual(processes[12]["status"], "T")
        self.assertEqual(processes[12]["start"], "123")
        with self.assertRaises(ValueError):
            FLASH.parse_processes("12\tinvalid 123 T\t0\t/tuya/tuya_start.sh\n")

    def test_legacy_linuxthreads_ancestry_is_accepted_without_accepting_strangers(self):
        processes = {10: process("/tuya/native/paired_start.sh"),
                     20: process("/tuya/native/ziggurat", parent=10),
                     21: process("/tuya/native/ziggurat", parent=20),
                     22: process("/tuya/native/ziggurat", parent=21),
                     30: process("/tmp/stranger", parent=20),
                     40: process("/tuya/native/ziggurat", parent=99)}
        self.assertTrue(FLASH.descended_from(22, 10, processes))
        self.assertFalse(FLASH.descended_from(30, 10, processes))
        self.assertFalse(FLASH.descended_from(40, 10, processes))

    def test_shell_scan_uses_stock_tools_and_exact_supervisor_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "self").mkdir()
            (root / "self/status").write_text("Uid:\t0\t0\t0\t0\n")
            for pid, executable, argv, owns in (
                    (10, "/bin/sh", b"/bin/sh\0/tuya/native/paired_start.sh\0", False),
                    (20, "/tuya/native/ziggurat", b"/tuya/native/ziggurat\0", True),
                    (30, "/bin/sh", b"/bin/sh\0-c\0echo /tuya/native/paired_start.sh\0", False)):
                proc = root / str(pid)
                (proc / "fd").mkdir(parents=True)
                (proc / "exe").symlink_to(executable)
                (proc / "cmdline").write_bytes(argv)
                (proc / "stat").write_text(str(pid) + " (test) S 1 " + "0 " * 17 + "123 0\n")
                (proc / "fd/0").symlink_to("/dev/null" if owns else "/dev/zero")
                (proc / "fd/1").symlink_to("/missing/vanished-fd")
            script = FLASH.SCAN.replace("/proc", str(root)).replace("/dev/ttyS1", "/dev/null")
            output = subprocess.check_output(["/bin/sh", "-c", script], text=True)
            processes = FLASH.parse_processes(output)
            self.assertEqual(set(processes), {10, 20})
            self.assertEqual(processes[10]["exe"], "/tuya/native/paired_start.sh")
            self.assertTrue(processes[20]["owner"])

    def test_password_file_passes_only_its_path_to_sshpass(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "password"
            path.write_text("never-print-this-secret")
            with patch.object(FLASH.shutil, "which", return_value="/usr/bin/sshpass"):
                gateway = FLASH.Gateway(args(password_file=path))
            self.assertEqual(gateway.ssh[:3], ["sshpass", "-f", str(path)])
            self.assertNotIn("never-print-this-secret", " ".join(gateway.ssh))

    def test_reused_bridge_pid_never_signaled(self):
        gateway = self.gateway()
        gateway.bridge = "/tmp/microhad-radio-release.test/serialgateway"
        gateway.bridge_pid = 33
        gateway.bridge_start = "123"
        gateway.processes.return_value = {33: process(gateway.bridge, owner=True, start="999")}
        with self.assertRaisesRegex(ValueError, "identity changed"):
            gateway.stop_bridge()
        gateway.remote.assert_not_called()

    def test_download_hash_failure_precedes_remote_staging(self):
        gateway = self.gateway()
        with patch.object(FLASH.subprocess, "run", return_value=argparse.Namespace(returncode=1)), \
                patch.object(FLASH.urllib.request, "urlopen") as download:
            download.return_value.__enter__.return_value.read.return_value = b"untrusted"
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                gateway.provision()
        self.assertEqual(gateway.remote.call_count, 1)  # read-only platform check

    def test_installed_bridge_reused_without_download(self):
        gateway = self.gateway()
        data = b"test bridge"
        gateway.remote.side_effect = ["", "", "/tmp/microhad-radio-release.test"]
        with patch.object(FLASH.subprocess, "run", return_value=argparse.Namespace(returncode=0, stdout=data)), \
                patch.object(FLASH, "BRIDGE_SHA256", FLASH.digest(data)), \
                patch.object(FLASH, "validate_bridge"), patch.object(FLASH, "bridge_bytes") as download:
            gateway.provision()
        self.assertEqual(gateway.bridge, "/tuya/serialgateway")
        download.assert_not_called()

    def test_missing_bridge_staged_in_private_tmp_not_flash(self):
        gateway = self.gateway()
        gateway.remote.side_effect = ["", "", "/tmp/microhad-radio-release.test", "", ""]
        with patch.object(FLASH.subprocess, "run", return_value=argparse.Namespace(returncode=1)), \
                patch.object(FLASH, "bridge_bytes", return_value=b"bridge"):
            gateway.provision()
        self.assertEqual(gateway.bridge, "/tmp/microhad-radio-release.test/serialgateway")
        self.assertIn("cat > /tmp/", gateway.remote.call_args_list[-2].args[0])
        self.assertIn("md5sum", gateway.remote.call_args.args[0])

    def test_tmp_symlink_uses_physical_executable_identity(self):
        gateway = self.gateway()
        gateway.remote.side_effect = ["", "", "/var/tmp/microhad-radio-release.test", "", ""]
        with patch.object(FLASH.subprocess, "run", return_value=argparse.Namespace(returncode=1)), \
                patch.object(FLASH, "bridge_bytes", return_value=b"bridge"):
            gateway.provision()
        self.assertEqual(gateway.directory, "/var/tmp/microhad-radio-release.test")
        self.assertEqual(gateway.bridge, "/var/tmp/microhad-radio-release.test/serialgateway")

    def test_flash_failure_never_restarts_services_or_retries(self):
        gateway = self.gateway(radio="ezsp", already_in_bootloader=True)
        gateway.provision = Mock()
        gateway.stop_services = Mock()
        gateway.free = Mock()
        gateway.start_bridge = Mock()
        gateway.stop_bridge = Mock()
        with patch.object(FLASH, "menu"), patch.object(FLASH, "upload", new_callable=AsyncMock) as upload:
            upload.side_effect = TimeoutError("upload uncertain")
            with self.assertRaises(TimeoutError):
                gateway.run(object())
        upload.assert_awaited_once()
        self.assertFalse(gateway.success)
        gateway.remote.assert_not_called()

    def test_prepared_ezsp_skips_bootloader_entry_and_verifies_new_hdlc(self):
        gateway = self.gateway(radio="ezsp", already_in_bootloader=True)
        gateway.provision = Mock()
        gateway.stop_services = Mock()
        gateway.free = Mock()
        gateway.start_bridge = Mock()
        gateway.stop_bridge = Mock()
        with patch.object(FLASH, "menu"), patch.object(FLASH, "upload", new_callable=AsyncMock), \
                patch.object(FLASH, "enter_ezsp", new_callable=AsyncMock) as entry, \
                patch.object(FLASH, "verify_hdlc") as verify, patch.object(FLASH.time, "sleep"):
            gateway.run(object())
        entry.assert_not_awaited()
        verify.assert_called_once()
        self.assertEqual(gateway.start_bridge.call_args_list[0].args, (115200, False))
        self.assertTrue(gateway.success)
        gateway.remote.assert_not_called()  # no EZSP service restart

    def test_direct_ezsp_enters_bootloader_once_and_reconfigures_bridge(self):
        gateway = self.gateway(radio="ezsp")
        gateway.provision = Mock()
        gateway.stop_services = Mock()
        gateway.free = Mock()
        gateway.start_bridge = Mock()
        gateway.stop_bridge = Mock()
        with patch.object(FLASH, "menu"), patch.object(FLASH, "upload", new_callable=AsyncMock), \
                patch.object(FLASH, "enter_ezsp", new_callable=AsyncMock) as entry, \
                patch.object(FLASH, "verify_hdlc"), patch.object(FLASH.time, "sleep"):
            gateway.run(object())
        entry.assert_awaited_once_with("127.0.0.1", 8888)
        self.assertEqual([call.args for call in gateway.start_bridge.call_args_list],
                         [(115200, True), (115200, False), (460800, True)])

    def test_hdlc_resets_iid1_then_restarts_previously_running_pair(self):
        gateway = self.gateway()
        gateway.provision = Mock()
        gateway.stop_services = Mock(side_effect=lambda: setattr(gateway, "paired_running", True))
        gateway.free = Mock()
        gateway.start_bridge = Mock()
        gateway.stop_bridge = Mock()
        gateway.processes.return_value = {13: process("/tuya/native/ziggurat", owner=True)}
        with patch.object(FLASH, "menu"), patch.object(FLASH, "upload", new_callable=AsyncMock), \
                patch.object(FLASH, "verify_hdlc") as verify, patch.object(FLASH.time, "sleep"), \
                patch.object(FLASH.socket, "create_connection") as connection:
            gateway.run(object())
        connection.return_value.__enter__.return_value.sendall.assert_called_once_with(FLASH.spinel_reset())
        verify.assert_called_once()
        self.assertTrue(gateway.success)
        self.assertEqual([call.args for call in gateway.start_bridge.call_args_list],
                         [(460800, True), (115200, False), (460800, True)])
        self.assertIn("/tuya/tuya_start.sh", gateway.remote.call_args.args[0])

    def test_verification_failure_never_restarts_pair(self):
        gateway = self.gateway(already_in_bootloader=True)
        gateway.provision = Mock()
        gateway.stop_services = Mock(side_effect=lambda: setattr(gateway, "paired_running", True))
        gateway.free = Mock()
        gateway.start_bridge = Mock()
        gateway.stop_bridge = Mock()
        with patch.object(FLASH, "menu"), patch.object(FLASH, "upload", new_callable=AsyncMock), \
                patch.object(FLASH, "verify_hdlc", side_effect=ValueError("no version")), \
                patch.object(FLASH.time, "sleep"), self.assertRaisesRegex(ValueError, "version"):
            gateway.run(object())
        gateway.remote.assert_not_called()
        self.assertFalse(gateway.success)


class Packaging(unittest.TestCase):
    def test_archive_standalone_reproducible_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "input"
            (build / "build/debug").mkdir(parents=True)
            data = image()
            (build / "build/debug/ot-rcp-multipan.gbl").write_bytes(data)
            notices = Mock()
            notices.collect.return_value = "# Real notices belong here\n"
            original_load = BUILD.load
            def modules(name, path):
                return notices if name == "radio_release_notices" else original_load(name, path)
            with patch.object(BUILD, "load", side_effect=modules):
                for name in ("first.tar.gz", "second.tar.gz"):
                    BUILD.package("1.2.3", root / name, 1000000, "test provenance", build=build)
                first = (root / "first.tar.gz").read_bytes()
                self.assertEqual(first, (root / "second.tar.gz").read_bytes())
                with self.assertRaises(FileExistsError):
                    BUILD.package("1.2.3", root / "first.tar.gz", 1000000, "test provenance", build=build)
                self.assertEqual(first, (root / "first.tar.gz").read_bytes())
            with tarfile.open(root / "first.tar.gz") as archive:
                prefix = "ot-rcp-multipan-1.2.3/"
                self.assertEqual(set(archive.getnames()), {prefix + name for name in
                                 ("ot-rcp-multipan.gbl", "flash.py", "readme.md", "license.md")})
                script = archive.extractfile(prefix + "flash.py").read()
                self.assertNotIn(b"@IMAGE_SHA256@", script)
                self.assertIn(hashlib.sha256(data).hexdigest().encode(), script)
                compile(script, "flash.py", "exec")
                self.assertEqual(archive.getmember(prefix + "flash.py").mode, 0o755)

    def test_missing_license_never_publishes_partial_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "build/debug").mkdir(parents=True)
            (root / "build/debug/ot-rcp-multipan.gbl").write_bytes(image())
            notices = Mock()
            notices.collect.side_effect = ValueError("Unreviewed license")
            original_load = BUILD.load
            with patch.object(BUILD, "load", side_effect=lambda name, path:
                              notices if name == "radio_release_notices" else original_load(name, path)):
                destination = root / "release/app.tar.gz"
                with self.assertRaisesRegex(ValueError, "Unreviewed"):
                    BUILD.package("1.0", destination, 1000000, "test", build=root)
                self.assertFalse(destination.exists())
                self.assertFalse(destination.parent.exists())

    def test_bad_version_rejected_without_build(self):
        for version in ("../escape", "", "v1/x"):
            with patch.object(BUILD.subprocess, "run") as command, redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    BUILD.main(["--version", version])
                command.assert_not_called()

    def test_runtime_license_inputs_pinned(self):
        for name, checksum in NOTICES.SUPPLEMENTS.items():
            self.assertEqual(hashlib.sha256((ROOT / "firmware/release/licenses" / name).read_bytes()).hexdigest(), checksum)

    def test_installer_and_firmware_builder_use_release_sdk_pin(self):
        for script in ("install-silabs.sh", "build-hdlc-firmware.sh"):
            self.assertIn(BUILD.SDK_REVISION, (ROOT / "firmware/scripts" / script).read_text())


if __name__ == "__main__":
    unittest.main()
