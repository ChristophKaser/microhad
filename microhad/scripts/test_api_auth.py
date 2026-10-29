#!/usr/bin/env python3
"""Synthetic localhost access-control tests. Never contacts a gateway."""
import hashlib
import hmac
import json
import os
from pathlib import Path
import sys
import time
import unittest
import urllib.error
import urllib.request

import test_ziggurat_native_peer as peer_fixture
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from gateway_api import GatewayClient

ROLES = ['guest', 'user', 'administrator']


class AccessTests(unittest.TestCase):
    def setUp(self):
        self.fixture = peer_fixture.NativeTests(methodName='runTest')
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.base = f'http://127.0.0.1:{self.fixture.port}'
        self.passwords = {}

    @property
    def state_path(self):
        return self.fixture.directory / 'api-access.json'

    def restart(self):
        self.fixture.process.terminate()
        self.fixture.process.wait(timeout=10)
        import subprocess
        self.fixture.process = subprocess.Popen([str(peer_fixture.NATIVE_BINARY), '--config', str(self.fixture.config)], stdout=self.fixture.log, stderr=self.fixture.log)
        peer_fixture.wait_for(lambda: self.call('/api/auth/status')[0] == 200)

    def signing_key(self):
        records = {row['role']: row for row in map(json.loads, self.state_path.read_text().splitlines()) if 'token' not in row}
        material = 'microhad-session-key-v2\n'
        for role in range(3):
            record = records[role]
            material += f"{ROLES[role]}\n{record['salt']}\n{record['verifier']}\n"
        return hashlib.sha256(material.encode()).digest()

    def mint(self, role, expires=0, nonce=b'\x42' * 16, key=None):
        payload = (bytes([2, role]) + expires.to_bytes(8, 'big') + nonce).hex()
        signature = hmac.new(self.signing_key() if key is None else key, ('microhad-session-token-v2\n' + payload).encode(), hashlib.sha256).hexdigest()
        return payload + signature

    def call(self, path, body=None, token='', method=None, headers=None):
        data = None if body is None else json.dumps(body).encode()
        supplied = {'Content-Type': 'application/json', **(headers or {})}
        if token:
            supplied['Authorization'] = 'Bearer ' + token
        request = urllib.request.Request(self.base + path, data=data, headers=supplied, method=method)
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)

    def login(self, role, password=None, mode='persistent'):
        code, challenge = self.call('/api/auth/challenge', {'role': ROLES[role]})
        self.assertEqual(code, 200)
        if challenge.get('open'):
            return ''
        effective = max(i for i in self.passwords if i <= role)
        password = self.passwords[effective] if password is None else password
        key = hashlib.pbkdf2_hmac('sha256', password.encode(), bytes.fromhex(challenge['salt']), 100000)
        proof = hmac.new(key, f"microhad-login-v1\n{challenge['challenge']}\n{ROLES[role]}\n{mode}".encode(), hashlib.sha256).hexdigest()
        body = {'challenge': challenge['challenge'], 'proof': proof, 'mode': mode}
        code, result = self.call('/api/auth/login', body)
        self.assertEqual(code, 200, result)
        self.assertEqual(self.call('/api/auth/login', body)[0], 401, 'challenge replay')
        token = result['token']
        self.assertEqual(len(token), 116)
        expiry = int(token[4:20], 16)
        if mode == 'persistent':
            self.assertEqual(expiry, 0)
        else:
            self.assertLessEqual(abs(expiry - (int(time.time()) + 43200)), 2)
        self.assertEqual(token, self.mint(role, expiry, bytes.fromhex(token[20:52])))
        return token

    def change(self, role, password):
        token = self.login(2)
        salt = os.urandom(16).hex() if password else ''
        verifier = hashlib.pbkdf2_hmac('sha256', password.encode(), bytes.fromhex(salt), 100000).hex() if password else ''
        self.assertEqual(self.call('/api/auth/password', {'role': ROLES[role], 'salt': salt, 'verifier': verifier}, token)[0], 200)
        if password:
            self.passwords[role] = password
        else:
            self.passwords.pop(role, None)
        return token

    def check_mask(self, mask):
        self.assertEqual(self.call('/api/auth/status')[1]['role'], 2)
        # Exercise every configured-password combination, including gaps.
        for role in range(3):
            self.change(role, f'pássword-{role}' if mask & (1 << role) else '')
        open_role = min(self.passwords) - 1
        self.assertEqual(self.call('/api/auth/status')[1]['role'], open_role)
        for role in range(3):
            token = self.login(role)
            effective_role = max(role, open_role)
            self.assertEqual(self.call('/api/auth/status', token=token)[1]['role'], effective_role)
            for path, needed in [('/api/status', 0), ('/api/settings', 2), ('/api/logs', 2)]:
                code, _ = self.call(path, token=token)
                self.assertIn(code, (200, 503) if effective_role >= needed else (401 if effective_role < 0 else 403,))
            self.assertNotEqual(self.call('/api/push/devices/register', {}, token)[0], 403)
            control_code = self.call('/api/devices/invalid/light', {}, token)[0]
            self.assertEqual(control_code == 403, effective_role < 1)
            commissioning = self.call('/api/matter/commissioning/start', {}, token)[0]
            self.assertEqual(commissioning == 403, effective_role < 2)
            if token:
                self.assertEqual(self.call('/api/auth/logout', {}, token)[0], 404)
                self.assertEqual(self.call('/api/auth/status', token=token)[1]['role'], effective_role)
        self.assertEqual(self.call('/api/auth/password', {'role': 'guest', 'salt': '', 'verifier': ''}, headers={'Origin': 'http://evil.invalid'})[0], 403)

    def test_invalid_proofs_and_changed_password_challenges(self):
        self.change(0, 'initial')
        code, challenge = self.call('/api/auth/challenge', {'role': 'guest'})
        self.assertEqual(code, 200)
        key = hashlib.pbkdf2_hmac('sha256', b'initial', bytes.fromhex(challenge['salt']), 100000)
        proof = hmac.new(key, f"microhad-login-v1\n{challenge['challenge']}\nguest\npersistent".encode(), hashlib.sha256).hexdigest()
        body = {'challenge': challenge['challenge'], 'proof': '0' * 64, 'mode': 'persistent'}
        self.assertEqual(self.call('/api/auth/login', body)[0], 401)
        body['proof'] = proof
        self.assertEqual(self.call('/api/auth/login', body)[0], 401)
        code, pending = self.call('/api/auth/challenge', {'role': 'guest'})
        self.assertEqual(code, 200)
        key = hashlib.pbkdf2_hmac('sha256', b'initial', bytes.fromhex(pending['salt']), 100000)
        body = {'challenge': pending['challenge'], 'proof': hmac.new(key, f"microhad-login-v1\n{pending['challenge']}\nguest\npersistent".encode(), hashlib.sha256).hexdigest(), 'mode': 'persistent'}
        self.change(2, 'new-admin')
        self.assertEqual(self.call('/api/auth/login', body)[0], 401)
        self.assertEqual(self.call('/api/auth/password', {'role': 'guest', 'salt': '', 'verifier': ''})[0], 401)

    def test_matter_job_permissions_and_relay_handshake_authentication(self):
        from test_matter_native_worker import MatterPeer, RelaySocket
        from test_ziggurat_native_peer import wait_for
        import socket
        matter = MatterPeer()
        self.addCleanup(matter.close)
        wait_for(lambda: self.call('/api/matter/status')[1].get('available'), timeout=10)
        self.change(0, 'shared-secret')
        guest, user, admin = [self.login(role) for role in range(3)]
        identity = dict(schema=1, node='0000000000000002', endpoint=1)
        for path, body, token, minimum in [
            ('/api/matter/devices/read', dict(identity, cluster=6, attribute=0), guest, 0),
            ('/api/matter/devices/command', dict(identity, command='on'), user, 1),
            ('/api/matter/commissioning/start', dict(schema=1, discriminator=3840, pin=20202021, timeout_seconds=30), admin, 2),
        ]:
            code, accepted = self.call(path, body, token)
            self.assertEqual(code, 202, accepted)
            for role, credential in enumerate([guest, user, admin]):
                code, _ = self.call('/api/matter/jobs/' + str(accepted['job']), token=credential)
                self.assertEqual(code, 200 if role >= minimum else 404)
        for token in [guest, user]:
            self.assertEqual(self.call('/api/matter/relay', token=token)[0], 403)
        def open_relay(token, expected=(101,)):
            relay = socket.create_connection(('127.0.0.1', self.fixture.port), timeout=3)
            self.addCleanup(relay.close)
            relay.sendall(('GET /api/matter/relay HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: microhad-auth.' + token + '\r\n\r\n').encode())
            response = b''
            while not response.endswith(b'\r\n\r\n'):
                response += MatterPeer.exact(relay, 1)
                self.assertLessEqual(len(response), 4096)
            status = int(response.split(b' ', 2)[1])
            self.assertIn(status, expected, response)
            if status != 101:
                relay.close()
                return None
            self.assertIn(('Sec-WebSocket-Protocol: microhad-auth.' + token).encode(), response)
            protocol = RelaySocket.__new__(RelaySocket)
            protocol.peer = relay
            return protocol

        def close_from_worker(protocol):
            # Leave the client's TCP socket open until the worker closes its end.
            protocol.send(8, b'\x03\xe8')
            opcode, _ = protocol.receive()
            self.assertEqual(opcode, 8)
            self.assertEqual(protocol.peer.recv(1), b'')
            protocol.close()

        relay = open_relay(admin)
        self.change(2, 'replacement')
        relay.send(9, b'still connected')
        self.assertEqual(relay.receive(), (10, b'still connected'))
        open_relay(admin, expected=(401,))

        # The previous password must also fail a fresh challenge-response login.
        code, challenge = self.call('/api/auth/challenge', {'role': 'administrator'})
        self.assertEqual(code, 200)
        key = hashlib.pbkdf2_hmac('sha256', b'shared-secret', bytes.fromhex(challenge['salt']), 100000)
        proof = hmac.new(key, f"microhad-login-v1\n{challenge['challenge']}\nadministrator\npersistent".encode(), hashlib.sha256).hexdigest()
        self.assertEqual(self.call('/api/auth/login', {'challenge': challenge['challenge'], 'proof': proof, 'mode': 'persistent'})[0], 401)
        fresh = self.login(2)
        close_from_worker(relay)
        # More than the former eight-slot tracking limit: each worker close must
        # actually release the socket, even when the remote peer stays open.
        for _ in range(10):
            protocol = wait_for(lambda: open_relay(fresh, expected=(101, 503)))
            protocol.send(9, b'new credentials')
            self.assertEqual(protocol.receive(), (10, b'new credentials'))
            close_from_worker(protocol)

    def test_stateless_persistence_no_writes_and_tools(self):
        self.change(0, 'guest-secret')
        before = self.state_path.read_bytes(), self.state_path.stat().st_mtime_ns, self.state_path.stat().st_ino
        app = self.login(2)
        browser = self.login(2, mode='browser')
        self.assertNotEqual(app, self.login(2))
        rows = list(map(json.loads, self.state_path.read_text().splitlines()))
        self.assertEqual(len(rows), 3)
        self.assertTrue(all(set(row) == {'role', 'salt', 'verifier'} for row in rows))
        self.assertEqual(self.state_path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.call('/api/auth/logout', {}, app)[0], 404)
        for token in [app, browser]:
            self.assertEqual(self.call('/api/settings', token=token)[0], 200)
        self.assertEqual(before, (self.state_path.read_bytes(), self.state_path.stat().st_mtime_ns, self.state_path.stat().st_ino))
        self.restart()
        for token in [app, browser]:
            self.assertEqual(self.call('/api/settings', token=token)[0], 200)
        password_file = self.fixture.directory / 'admin-password'
        password_file.write_text('guest-secret\n')
        client = GatewayClient(self.base, password_file=password_file)
        with client.open(self.base + '/api/settings') as response:
            self.assertEqual(response.status, 200)
        client.close()
        self.assertEqual(client.token, '')
        self.assertEqual(before, (self.state_path.read_bytes(), self.state_path.stat().st_mtime_ns, self.state_path.stat().st_ino))

    def test_expiry_and_tampering(self):
        self.change(0, 'secret')
        guest = self.login(0)
        admin = self.login(2, mode='browser')
        for expiry in [1, int(time.time()) - 1, int(time.time())]:
            self.assertEqual(self.call('/api/settings', token=self.mint(2, expiry))[0], 401)
        self.assertEqual(self.call('/api/settings', token=self.mint(2, int(time.time()) + 60))[0], 200)
        self.assertEqual(self.call('/api/settings', token=self.mint(2, 0))[0], 200)
        tampered = [
            '03' + admin[2:],
            guest[:2] + '02' + guest[4:],
            admin[:4] + '0000000000000000' + admin[20:],
            admin[:20] + ('0' if admin[20] != '0' else '1') + admin[21:],
            admin[:-1] + ('0' if admin[-1] != '0' else '1'),
            admin.upper(), admin[:-1], admin + '00', 'g' * 116,
            self.mint(3), self.mint(255), 'a' * 64,
        ]
        for value in tampered:
            with self.subTest(token=value):
                self.assertEqual(self.call('/api/settings', token=value)[0], 401)

    def test_every_password_change_invalidates_all_token_modes(self):
        self.change(0, 'guest-secret')
        for changed in range(3):
            tokens = [self.login(role) for role in range(3)] + [self.login(2, mode='browser')]
            self.change(changed, f'changed-{changed}')
            for token in tokens:
                self.assertEqual(self.call('/api/status', token=token)[0], 401)
        old = self.login(2)
        self.change(0, self.passwords[0])  # A fresh salt also invalidates sessions.
        self.assertEqual(self.call('/api/status', token=old)[0], 401)

    def test_guest_verifier_cannot_sign_admin_tokens_with_separate_password(self):
        self.change(0, 'guest-secret')
        self.change(2, 'admin-secret')
        guest_record = next(row for row in map(json.loads, self.state_path.read_text().splitlines()) if row['role'] == 0)
        forged = self.mint(2, key=bytes.fromhex(guest_record['verifier']))
        self.assertEqual(self.call('/api/settings', token=forged)[0], 401)
        self.assertEqual(self.call('/api/settings', token=self.login(2))[0], 200)

    def test_legacy_session_records_are_ignored_without_migration_writes(self):
        self.change(0, 'secret')
        signed = self.login(2)
        legacy = 'c' * 64
        self.state_path.write_text(self.state_path.read_text() + json.dumps({'token': hashlib.sha256(legacy.encode()).hexdigest(), 'role': 2, 'expires': 0}) + '\n')
        before = self.state_path.read_bytes(), self.state_path.stat().st_ino
        self.restart()
        self.assertEqual(self.call('/api/settings', token=legacy)[0], 401)
        self.assertEqual(self.call('/api/settings', token=signed)[0], 200)
        self.login(2)
        self.assertEqual(before, (self.state_path.read_bytes(), self.state_path.stat().st_ino))
        self.change(1, 'user-secret')
        self.assertTrue(all('token' not in row for row in map(json.loads, self.state_path.read_text().splitlines())))


for _mask in range(1, 8):
    def _test(self, mask=_mask):
        self.check_mask(mask)
    setattr(AccessTests, f'test_password_mask_{_mask}', _test)


if __name__ == '__main__':
    unittest.main()
