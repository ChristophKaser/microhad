#!/usr/bin/env python3
"""Authenticated gateway HTTP transport. Passwords stay in a private host file."""
import argparse
import atexit
import hashlib
import hmac
import json
from pathlib import Path
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request
from workspace_config import settings


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class GatewayClient:
    def __init__(self, base, *, timeout=10, insecure=False, password_file=None):
        parsed = urllib.parse.urlsplit(base)
        if parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username or parsed.password:
            raise ValueError('Invalid gateway origin')
        self.base = urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, '', '', ''))
        self.timeout = timeout
        self.password_file = password_file
        self.token = ''
        self.authenticated = False
        context = ssl._create_unverified_context() if insecure else ssl.create_default_context()
        self.opener = urllib.request.build_opener(NoRedirect(), urllib.request.ProxyHandler({}), urllib.request.HTTPSHandler(context=context))

    def raw(self, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(self.base + path, data=data, headers={'Content-Type': 'application/json'})
        with self.opener.open(request, timeout=self.timeout) as response:
            return json.load(response)

    def login(self):
        try:
            challenge = self.raw('/api/auth/challenge', {'role': 'administrator'})
        except urllib.error.HTTPError as error:
            if error.code == 400:
                try:
                    legacy = json.loads(error.read(1024)) == {'error': 'unknown endpoint'}
                except (ValueError, UnicodeError):
                    legacy = False
                if not legacy:
                    raise
            elif error.code != 404:
                raise
            # Legacy daemons used either 404 or this exact 400 JSON response
            # for unknown API routes. Other auth failures must never downgrade.
            self.authenticated = True
            return
        if challenge.get('open'):
            self.authenticated = True
            return
        path = self.password_file if self.password_file is not None else settings().get('API_ADMIN_PASSWORD_FILE', '')
        if not path:
            raise ValueError('Set API_ADMIN_PASSWORD_FILE in workspace.local.env for protected gateway access')
        password = Path(path).read_bytes().rstrip(b'\r\n')
        if challenge.get('iterations') != 100000 or len(challenge.get('salt', '')) != 32 or len(challenge.get('challenge', '')) != 64:
            raise ValueError('Invalid gateway login challenge')
        verifier = hashlib.pbkdf2_hmac('sha256', password, bytes.fromhex(challenge['salt']), 100000)
        message = f"microhad-login-v1\n{challenge['challenge']}\nadministrator\nbrowser".encode()
        proof = hmac.new(verifier, message, hashlib.sha256).hexdigest()
        self.token = self.raw('/api/auth/login', {'challenge': challenge['challenge'], 'proof': proof, 'mode': 'browser'})['token']
        self.authenticated = True

    def close(self):
        self.token = ''
        self.authenticated = False

    def open(self, request, timeout=None):
        if isinstance(request, str):
            request = urllib.request.Request(request)
        parsed = urllib.parse.urlsplit(request.full_url)
        origin = urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, '', '', ''))
        if origin != self.base:
            raise ValueError('Refusing to send gateway session to another origin')
        # Authenticate before mutations, so failed commands are never retried.
        if not self.authenticated:
            self.login()
        if self.token:
            request.add_header('Authorization', 'Bearer ' + self.token)
        try:
            return self.opener.open(request, timeout=timeout or self.timeout)
        except urllib.error.HTTPError as error:
            if error.code in (401, 403):
                self.token = ''
                self.authenticated = False
            raise


_clients = {}


def urlopen(request, timeout=10):
    url = request if isinstance(request, str) else request.full_url
    parsed = urllib.parse.urlsplit(url)
    base = urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, '', '', ''))
    if base not in _clients:
        _clients[base] = GatewayClient(base, timeout=timeout)
    client = _clients[base]
    return client.open(request, timeout)


def _close_clients():
    for client in _clients.values():
        try:
            client.close()
        except (OSError, ValueError):
            pass


atexit.register(_close_clients)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('url')
    parser.add_argument('--timeout', type=float, default=10)
    parser.add_argument('--insecure', action='store_true')
    parser.add_argument('--headers', action='store_true')
    args = parser.parse_args()
    client = None
    try:
        client = GatewayClient(args.url, timeout=args.timeout, insecure=args.insecure)
        with client.open(args.url) as response:
            if args.headers:
                sys.stdout.write(f'HTTP/1.1 {response.status}\r\n{response.headers}\r\n')
            else:
                while chunk := response.read(65536):
                    sys.stdout.buffer.write(chunk)
    except (OSError, ValueError, KeyError) as error:
        # Do not print request objects, credentials, or server response bodies.
        detail = str(error.code) if isinstance(error, urllib.error.HTTPError) else str(error) if isinstance(error, ValueError) else type(error).__name__
        print('Gateway API request failed: ' + detail, file=sys.stderr)
        return 1
    finally:
        if client:
            try:
                client.close()
            except (OSError, ValueError):
                pass
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
