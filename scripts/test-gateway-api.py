#!/usr/bin/env python3
"""Synthetic host transport checks; uses localhost only."""
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request

from gateway_api import GatewayClient
from workspace_config import settings


class TransportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.password_file = self.directory / 'private password'
        self.password_file.write_text(' a pássword \n')
        self.calls = []
        self.reject = False
        self.legacy = False
        self.legacy_status = 404
        self.legacy_error = 'unknown endpoint'
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.dispatch()

            def do_POST(self):
                self.dispatch()

            def dispatch(self):
                body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
                owner.calls.append((self.path, dict(self.headers), body))
                code, value = 200, {'ok': True}
                if self.path == '/api/auth/challenge':
                    if owner.legacy:
                        code = owner.legacy_status
                        value = {'error': owner.legacy_error}
                    else:
                        value = {'challenge': 'a' * 64, 'salt': bytes(range(16)).hex(), 'iterations': 100000}
                elif self.path == '/api/auth/login':
                    decoded = json.loads(body)
                    key = hashlib.pbkdf2_hmac('sha256', ' a pássword '.encode(), bytes(range(16)), 100000)
                    proof = hmac.new(key, ('microhad-login-v1\n' + 'a' * 64 + '\nadministrator\nbrowser').encode(), hashlib.sha256).hexdigest()
                    if decoded.get('proof') != proof:
                        code = 401
                    value = {'token': 'b' * 64}
                elif self.path == '/api/redirect':
                    self.send_response(302)
                    self.send_header('Location', '/api/unexpected')
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                    return
                elif owner.reject:
                    code = 401
                elif not owner.legacy and self.headers.get('Authorization') != 'Bearer ' + 'b' * 64:
                    code = 401
                encoded = json.dumps(value).encode()
                self.send_response(code)
                self.send_header('Content-Length', str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.close_server)
        self.base = f'http://127.0.0.1:{self.server.server_port}'

    def close_server(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join()

    def test_workspace_file_and_environment_precedence(self):
        config = self.directory / 'settings.env'
        config.write_text("API_ADMIN_PASSWORD_FILE='private password'\n")
        # Relative paths resolve against the workspace root, not the env file.
        with patch.dict(os.environ, WORKSPACE_CONFIG=str(config), API_ADMIN_PASSWORD_FILE=str(self.password_file)):
            self.assertEqual(settings()['API_ADMIN_PASSWORD_FILE'], str(self.password_file))
            client = GatewayClient(self.base)
            with client.open(self.base + '/api/settings') as response:
                self.assertEqual(response.status, 200)
            client.close()
        self.assertEqual([row[0] for row in self.calls], ['/api/auth/challenge', '/api/auth/login', '/api/settings'])
        self.assertTrue(all(b' a p' not in row[2] for row in self.calls))

    def test_no_mutation_retry_or_redirect(self):
        client = GatewayClient(self.base, password_file=self.password_file)
        client.login()
        self.reject = True
        with self.assertRaises(urllib.error.HTTPError):
            client.open(urllib.request.Request(self.base + '/api/mutate', data=b'command'))
        self.assertEqual(sum(path == '/api/mutate' for path, _, _ in self.calls), 1)
        self.assertFalse(client.authenticated)
        self.reject = False
        with self.assertRaises(urllib.error.HTTPError):
            client.open(self.base + '/api/redirect')
        self.assertFalse(any(path == '/api/unexpected' for path, _, _ in self.calls))
        with self.assertRaises(ValueError):
            client.open('http://another.invalid/api/settings')
        client.close()

    def test_cli_keeps_password_out_of_output_and_drops_session_locally(self):
        result = subprocess.run([sys.executable, str(Path(__file__).with_name('gateway_api.py')), self.base + '/api/settings'],
                                env=dict(os.environ, API_ADMIN_PASSWORD_FILE=str(self.password_file)), capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {'ok': True})
        self.assertEqual([row[0] for row in self.calls], ['/api/auth/challenge', '/api/auth/login', '/api/settings'])
        self.assertNotIn(b'p\xc3\xa1ssword', result.stdout + result.stderr)

    def test_close_is_local_and_works_offline(self):
        client = GatewayClient(self.base, password_file=self.password_file)
        client.login()
        count = len(self.calls)
        with patch.object(client.opener, 'open', side_effect=OSError('offline')):
            client.close()
        self.assertEqual(client.token, '')
        self.assertFalse(client.authenticated)
        self.assertEqual(len(self.calls), count)

    def test_legacy_open_daemon_never_reads_password_file(self):
        self.legacy = True
        client = GatewayClient(self.base, password_file=self.directory / 'does-not-exist')
        with client.open(self.base + '/api/status') as response:
            self.assertEqual(response.status, 200)
        with client.open(self.base + '/api/status'):
            pass
        self.assertEqual(sum(path == '/api/auth/challenge' for path, _, _ in self.calls), 1)

    def test_legacy_unknown_endpoint_400_never_reads_password_file(self):
        self.legacy = True
        self.legacy_status = 400
        client = GatewayClient(self.base, password_file=self.directory / 'does-not-exist')
        with client.open(self.base + '/api/status') as response:
            self.assertEqual(response.status, 200)
        self.assertEqual([row[0] for row in self.calls], ['/api/auth/challenge', '/api/status'])

    def test_other_400_auth_errors_never_downgrade(self):
        self.legacy = True
        self.legacy_status = 400
        self.legacy_error = 'malformed request'
        client = GatewayClient(self.base, password_file=self.password_file)
        with self.assertRaises(urllib.error.HTTPError) as caught:
            client.open(self.base + '/api/status')
        self.assertEqual(caught.exception.code, 400)
        self.assertEqual([row[0] for row in self.calls], ['/api/auth/challenge'])
        self.assertFalse(client.authenticated)


if __name__ == '__main__':
    unittest.main()
