#!/usr/bin/env python3
"""Serve the dashboard locally while proxying its API requests to a gateway."""
from __future__ import print_function

import argparse
import os
import urllib.request
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class DashboardHandler(SimpleHTTPRequestHandler):
    gateway_url = ''
    webui_directory = ''

    def translate_path(self, path):
        return os.path.join(self.webui_directory, path.lstrip('/').split('?', 1)[0] or 'index.html')

    def do_GET(self):
        if self.path.startswith('/api/'):
            self.proxy()
        else:
            super(DashboardHandler, self).do_GET()

    def do_POST(self):
        if self.path.startswith('/api/'):
            self.proxy()
        else:
            self.send_error(404)

    def proxy(self):
        length = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(length) if length else None
        request = urllib.request.Request(self.gateway_url + self.path, data=body, method=self.command)
        content_type = self.headers.get('Content-Type')
        if content_type:
            request.add_header('Content-Type', content_type)
        try:
            response = urllib.request.urlopen(request, timeout=8)
            payload = response.read()
            self.send_response(response.getcode())
            self.send_header('Content-Type', response.headers.get_content_type())
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except urllib.error.HTTPError as error:
            payload = error.read()
            self.send_response(error.code)
            self.send_header('Content-Type', error.headers.get_content_type())
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except urllib.error.URLError as error:
            self.send_error(502, 'Gateway unavailable: %s' % error.reason)

    def end_headers(self):
        if self.path == '/' or self.path.startswith('/index.html'):
            self.send_header('Cache-Control', 'no-store')
        super(DashboardHandler, self).end_headers()

    def send_head(self):
        if self.path == '/' or self.path.startswith('/index.html'):
            path = os.path.join(self.webui_directory, 'index.html')
            with open(path, 'rb') as page:
                content = page.read().replace(b'<head>', b'<head><script>window.MICROHAD_DASHBOARD_LOCAL_DEV=true;</script>', 1)
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(content)))
            self.end_headers()
            import io
            return io.BytesIO(content)
        return super(DashboardHandler, self).send_head()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', default='http://192.168.178.62', help='gateway base URL')
    parser.add_argument('--port', type=int, default=8000, help='local listening port')
    args = parser.parse_args()
    DashboardHandler.gateway_url = args.device.rstrip('/')
    DashboardHandler.webui_directory = os.path.dirname(os.path.abspath(__file__))
    server = ThreadingHTTPServer(('127.0.0.1', args.port), DashboardHandler)
    print('Dashboard: http://127.0.0.1:%d (API proxy: %s)' % (args.port, DashboardHandler.gateway_url))
    server.serve_forever()


if __name__ == '__main__':
    main()
