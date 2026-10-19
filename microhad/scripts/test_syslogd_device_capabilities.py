#!/usr/bin/env python3
"""Read-only capability checks for the stock development gateway BusyBox."""

import os
import re
import subprocess
import unittest
from pathlib import Path


import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from workspace_config import setting, ssh_command

HOST = os.environ.get("SYSLOGD_TEST_HOST", setting('GATEWAY_IP'))
PASSWORD_FILE = os.environ.get("SYSLOGD_TEST_PASSWORD_FILE")


def remote(command):
    return subprocess.run(ssh_command(HOST, PASSWORD_FILE) + [command],
                          check=True, text=True, capture_output=True).stdout


class SyslogdDeviceCapabilities(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.remote_destination = remote("/tuya/native/microhad --print-config syslog_remote --config /tuya/native/config.json").strip()
        cls.version = remote("busybox")
        cls.applets = remote("busybox").split("Currently defined functions:", 1)[-1]
        processes = remote("ps")
        cls.command_line = "\n".join(
            line for line in processes.splitlines() if "busybox syslogd -C" in line
        )
        cls.shared_memory = remote("cat /proc/sysvipc/shm")

    def test_stock_busybox_version(self):
        self.assertIn("BusyBox v1.13.4", self.version)

    def test_syslogd_remote_forwarding_is_active(self):
        self.assertIn("syslogd", self.applets)
        self.assertIn("-R", self.command_line)
        self.assertTrue(self.remote_destination, "Device configuration must enable forwarding for this check")
        self.assertIn(self.remote_destination, self.command_line)

    def test_syslogd_shared_memory_ring_is_active(self):
        self.assertRegex(self.command_line, r"-C(?:\x00)?\d+")
        self.assertRegex(self.shared_memory, r"\b65536\b")

    def test_logread_and_logger_applets_are_absent(self):
        self.assertNotIn("logread", self.applets)
        self.assertNotIn("logger", self.applets)


if __name__ == "__main__":
    unittest.main()
