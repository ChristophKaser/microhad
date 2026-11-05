#!/usr/bin/env python3
"""Host integration test with an in-memory, passive FTP server."""
import os
import socket
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent

class Server:
    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(8)
        self.sock.settimeout(.2)
        self.port = self.sock.getsockname()[1]
        self.files = {}
        self.lock = threading.Lock()
        self.fail_publish = False
        self.running = True
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while self.running:
            try:
                client, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self.handle, args=(client,), daemon=True).start()

    def handle(self, client):
        stream = client.makefile('rwb', buffering=0)
        stream.write(b'220 fixture\r\n')
        data = None
        rename = None
        try:
            while True:
                line = stream.readline()
                if not line:
                    break
                verb, _, value = line.rstrip(b'\r\n').partition(b' ')
                name = value.decode()
                if verb == b'USER': stream.write(b'331 password\r\n')
                elif verb == b'PASS': stream.write(b'230 logged in\r\n')
                elif verb == b'TYPE': stream.write(b'200 type\r\n')
                elif verb == b'CWD': stream.write(b'250 directory\r\n')
                elif verb == b'PASV':
                    data = socket.socket(); data.bind(('127.0.0.1', 0)); data.listen(1)
                    port = data.getsockname()[1]
                    stream.write(f'227 Passive (127,0,0,1,{port//256},{port%256})\r\n'.encode())
                elif verb == b'STOR':
                    stream.write(b'150 open\r\n'); transfer, _ = data.accept(); chunks=[]
                    while True:
                        chunk = transfer.recv(65536)
                        if not chunk: break
                        chunks.append(chunk)
                    transfer.close(); data.close(); data=None
                    with self.lock: self.files[name] = b''.join(chunks)
                    stream.write(b'226 stored\r\n')
                elif verb == b'NLST':
                    stream.write(b'150 list\r\n'); transfer, _ = data.accept()
                    with self.lock: listing = '\r\n'.join(self.files).encode() + b'\r\n'
                    transfer.sendall(listing); transfer.close(); data.close(); data=None
                    stream.write(b'226 listed\r\n')
                elif verb == b'RNFR': rename=name; stream.write(b'350 rename\r\n')
                elif verb == b'RNTO':
                    if self.fail_publish:
                        stream.write(b'550 publication rejected\r\n'); continue
                    with self.lock: self.files[name]=self.files.pop(rename)
                    stream.write(b'250 renamed\r\n')
                elif verb == b'DELE':
                    with self.lock: self.files.pop(name, None)
                    stream.write(b'250 deleted\r\n')
                else: stream.write(b'500 unsupported\r\n')
        except (OSError, KeyError):
            pass
        finally:
            if data: data.close()
            stream.close(); client.close()

    def close(self):
        self.running=False; self.sock.close(); self.thread.join()


def available(kind):
    s=socket.socket(socket.AF_INET,kind);s.bind(('127.0.0.1',0));p=s.getsockname()[1];s.close();return p


def wait_for(predicate):
    end=time.monotonic()+8
    while time.monotonic()<end:
        if predicate(): return
        time.sleep(.05)
    raise AssertionError('timed out waiting for collector')


def main():
    server=Server(); udp=available(socket.SOCK_DGRAM); tcp=available(socket.SOCK_STREAM)
    with tempfile.TemporaryDirectory() as td:
        binary=Path(td)/'collector'; config=Path(td)/'config'
        subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Werror','-pthread',str(ROOT/'main.c'),str(ROOT/'ftp.c'),'-o',str(binary)],check=True)
        config.write_text(f'ftp_host=127.0.0.1\nftp_port={server.port}\nftp_user=user\nftp_password=secret\nftp_directory=/\nudp_port={udp}\ntcp_port={tcp}\nrotate_seconds=1\nlog_files=2\n')
        config.chmod(0o600)
        proc=subprocess.Popen([str(binary),str(config)])
        try:
            wait_for(lambda: proc.poll() is not None or try_connect(tcp))
            assert proc.poll() is None
            s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
            s.sendto(b'<13>test log',( '127.0.0.1',udp));s.close()
            wait_for(lambda: any(k.endswith('.log') for k in server.files))
            assert any(b'test log' in v for k,v in server.files.items() if k.endswith('.log'))
            with socket.create_connection(('127.0.0.1',tcp),timeout=5) as client:
                frame=lambda data:struct.pack('!I',len(data))+data
                client.sendall(b'GCORE1 123 11 1700000000\n'+frame(b'exe=test\n')+frame(b'ELF fixture')+frame(b'')+b'C')
                client.shutdown(socket.SHUT_WR)
                assert client.recv(1)==b'K'
            assert any(v==b'ELF fixture' for k,v in server.files.items() if k.endswith('.core'))
            assert any(b'pid=123\n' in v for k,v in server.files.items() if k.endswith('.metadata'))
            with socket.create_connection(('127.0.0.1',tcp),timeout=5) as client:
                client.sendall(b'GCORE1 124 11 1700000000\n'+frame(b'exe=test\n')+frame(b'partial')+frame(b'')+b'T')
                client.shutdown(socket.SHUT_WR)
                assert client.recv(1)==b'K'
            assert any(v==b'partial' for k,v in server.files.items() if k.endswith('.truncated'))
            server.fail_publish=True
            with socket.create_connection(('127.0.0.1',tcp),timeout=5) as client:
                client.sendall(b'GCORE1 125 11 1700000000\n'+frame(b'exe=test\n')+frame(b'bad')+frame(b'')+b'C')
                client.shutdown(socket.SHUT_WR)
                assert client.recv(1)==b''
            server.fail_publish=False
            for i in range(2):
                s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
                s.sendto(f'<13>rotation {i}'.encode(),('127.0.0.1',udp));s.close()
                wait_for(lambda: any(f'rotation {i}'.encode() in v for k,v in server.files.items() if k.endswith('.log')))
            wait_for(lambda: len([k for k in server.files if k.endswith('.log')]) == 2)
            print('FTP log streaming, rotation, crash capture and rejected publication: passed')
        finally:
            proc.terminate()
            try:proc.wait(timeout=8)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
    server.close()


def try_connect(port):
    try:
        s=socket.create_connection(('127.0.0.1',port),timeout=.1);s.close();return True
    except OSError:return False

if __name__=='__main__':main()
