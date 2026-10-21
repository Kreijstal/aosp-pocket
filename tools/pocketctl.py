#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Owner console for AOSP Pocket. Requires Python 3, with no third-party modules."""
import argparse
import getpass
import hmac
import io
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import sys
import threading

MAGIC = b'PKADMIN1'
CHUNK = 65536


def exact(sock, count):
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count-len(data))
        if not part:
            raise OSError('Pocket closed the connection')
        data.extend(part)
    return bytes(data)


def connect(config):
    key = bytes.fromhex(config['token'])
    if len(key) != 32:
        raise ValueError('Invalid connection token; pair again')
    peer = socket.create_connection(('127.0.0.1', config['port']), timeout=5)
    try:
        if exact(peer, len(MAGIC)) != MAGIC:
            raise OSError('This port is not a Pocket admin endpoint')
        nonce = exact(peer, 32)
        proof = hmac.digest(key, MAGIC+nonce, 'sha256')
        peer.sendall(proof)
        if exact(peer, 1) != b'\x01':
            raise OSError('Connection token rejected; copy the current token and pair again')
        expected = hmac.digest(key, MAGIC+nonce+proof, 'sha256')
        if not hmac.compare_digest(exact(peer, 32), expected):
            raise OSError('Pocket server authentication failed')
        peer.settimeout(None)
        return peer
    except BaseException:
        peer.close()
        raise


def string(value):
    data = value.encode('utf-8')
    if b'\x00' in data or len(data) > CHUNK:
        raise ValueError('Command argument is invalid or too long')
    return struct.pack('!I', len(data))+data


def request(config, operation, argv=(), cwd='/', source=None, stdout=None, stderr=None):
    stdout = sys.stdout.buffer if stdout is None else stdout
    stderr = sys.stderr.buffer if stderr is None else stderr
    with connect(config) as peer:
        message = bytes([operation])
        if operation == 2:
            if not 1 <= len(argv) <= 128:
                raise ValueError('Expected 1–128 arguments')
            message += string(cwd)+struct.pack('!I', len(argv))+b''.join(map(string, argv))
        peer.sendall(message)

        def send_input():
            try:
                if source is not None:
                    try:
                        descriptor = source.fileno()
                    except (AttributeError, io.UnsupportedOperation):
                        descriptor = None
                    while True:
                        data = os.read(descriptor, CHUNK) if descriptor is not None else source.read(CHUNK)
                        if not data:
                            break
                        peer.sendall(b'\x01'+struct.pack('!I', len(data))+data)
                peer.sendall(b'\x02\x00\x00\x00\x00')
            except (OSError, ValueError):
                pass  # Main receiver reports transport failures or the remote exit code.

        if operation == 2:
            threading.Thread(target=send_input, daemon=True).start()
        while True:
            kind, count = struct.unpack('!BI', exact(peer, 5))
            if count > CHUNK*16:
                raise OSError('Invalid Pocket output frame')
            data = exact(peer, count)
            if kind == 3:
                if count != 4:
                    raise OSError('Invalid exit status')
                return struct.unpack('!i', data)[0]
            if kind not in (1, 2, 4):
                raise OSError('Unknown Pocket output frame')
            target = stdout if kind == 1 else stderr
            target.write(data+(b'\n' if kind == 4 else b''))
            target.flush()


def save_config(path, config):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_name(path.name+'.'+secrets.token_hex(8)+'.tmp')
    try:
        with os.fdopen(os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as out:
            json.dump(config, out)
            out.write('\n')
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path.home()/'.config/pocketctl/config.json')
    sub = parser.add_subparsers(dest='operation', required=True)
    pair = sub.add_parser('pair', help='Authorize this Termux client using the token from Pocket settings')
    pair.add_argument('--port', type=int, default=2228)
    for name in ('status', 'start', 'stop', 'reset-module-view'):
        sub.add_parser(name)
    shell = sub.add_parser('shell', help='Guest root shell (pipes; no PTY/job control)')
    shell.add_argument('-c', dest='command')
    shell.add_argument('--cwd', default='/')
    execute = sub.add_parser('exec', help='Execute a guest program, preserving argument boundaries')
    execute.add_argument('--cwd', default='/')
    execute.add_argument('argv', nargs=argparse.REMAINDER)
    push = sub.add_parser('push')
    push.add_argument('local', type=Path)
    push.add_argument('guest')
    pull = sub.add_parser('pull')
    pull.add_argument('guest')
    pull.add_argument('local', type=Path)
    install = sub.add_parser('install', help='Install a single APK using the guest package manager')
    install.add_argument('apk', type=Path)
    args = parser.parse_args()
    if args.operation == 'pair':
        token = getpass.getpass('Pocket connection token: ').strip()
        config = {'port': args.port, 'token': token}
        sink = io.BytesIO()
        code = request(config, 1, stdout=sink)
        if code:
            return code
        save_config(args.config, config)
        print('Paired. Run pocketctl.py shell or pocketctl.py status.')
        return 0
    config = json.loads(args.config.read_text())
    if args.operation in ('status', 'start', 'stop', 'reset-module-view'):
        return request(config, {'status': 1, 'start': 3, 'stop': 4, 'reset-module-view': 5}[args.operation])
    if args.operation == 'shell':
        argv = ['/system/bin/sh', '-i'] if args.command is None else ['/system/bin/sh', '-c', args.command]
        return request(config, 2, argv, args.cwd, sys.stdin.buffer)
    if args.operation == 'exec':
        argv = args.argv[1:] if args.argv[:1] == ['--'] else args.argv
        return request(config, 2, argv, args.cwd, sys.stdin.buffer)
    if args.operation == 'push':
        with args.local.open('rb') as source:
            return request(config, 2, ['/system/bin/sh', '-c', 'cat > "$1"', 'pocket-push', args.guest], source=source)
    if args.operation == 'pull':
        temporary = args.local.with_name(args.local.name+'.'+secrets.token_hex(8)+'.tmp')
        try:
            with temporary.open('xb') as out:
                code = request(config, 2, ['/system/bin/cat', args.guest], stdout=out)
            if code == 0:
                temporary.replace(args.local)
            return code
        finally:
            temporary.unlink(missing_ok=True)
    if args.operation == 'install':
        destination = '/data/local/tmp/pocket-install-'+secrets.token_hex(8)+'.apk'
        try:
            with args.apk.open('rb') as source:
                code = request(config, 2, ['/system/bin/sh', '-c', 'cat > "$1"', 'pocket-push', destination], source=source)
            return code or request(config, 2, ['/system/bin/pm', 'install', '-r', destination])
        finally:
            try:
                request(config, 2, ['/system/bin/rm', '-f', destination])
            except OSError:
                pass


if __name__ == '__main__':
    try:
        status = main()
    except KeyboardInterrupt:
        status = 130
    except (OSError, ValueError, KeyError) as error:
        print('pocketctl:', error, file=sys.stderr)
        status = 1
    sys.exit(status if 0 <= status <= 255 else 1)
