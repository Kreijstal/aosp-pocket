#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the production Java endpoint with the production Python client."""
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import stat
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('pocketctl', ROOT/'tools/pocketctl.py')
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


def main():
    fixture = subprocess.Popen(['java', '-cp', str(ROOT/'out/admin-tests'),
                                'dev.kreijstal.aosppocket.runtime.AdminServerFixture'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    checks = []
    try:
        port = int(fixture.stdout.readline())
        config = {'port': port, 'token': '11'*32}

        def request(op, argv=(), source=None, cwd='/'):
            out, err = io.BytesIO(), io.BytesIO()
            code = client.request(config, op, argv, cwd=cwd, source=source, stdout=out, stderr=err)
            return code, out.getvalue(), err.getvalue()

        wrong = dict(config, token='00'*32)
        try:
            client.connect(wrong)
            raise AssertionError('Wrong token was accepted')
        except OSError as error:
            assert 'rejected' in str(error), error
        assert json.loads(request(1)[1])['executed'] == 0
        checks.append('wrong token rejected before any command executes')

        # A keyless process binding the port must not impersonate Pocket.
        import threading
        fake = socket.socket()
        fake.bind(('127.0.0.1', 0));fake.listen()

        def impersonate():
            with fake:
                peer, _ = fake.accept()
                with peer:
                    peer.sendall(client.MAGIC+b'\0'*32)
                    client.exact(peer, 32)
                    peer.sendall(b'\1'+b'\0'*32)

        thread = threading.Thread(target=impersonate);thread.start()
        try:
            client.connect(dict(config, port=fake.getsockname()[1]))
            raise AssertionError('Fake server accepted')
        except OSError as error:
            assert 'server authentication failed' in str(error), error
        thread.join(2)
        checks.append('client authenticates server as well as server authenticating client')

        data = bytes(range(256))*2048
        code, out, err = request(2, ['/bin/sh', '-c', 'cat; printf diagnostic >&2; exit 7'], io.BytesIO(data))
        assert (code, out, err) == (7, data, b'diagnostic')
        checks.append('binary stdin/stdout, separate stderr and exit code')
        for source in (b'x', data):
            code, out, err = request(2, ['/bin/sh', '-c', 'exec 0<&-; sleep .2; printf done'], io.BytesIO(source))
            assert (code, out, err) == (0, b'done', b''), (code, out, err)
        checks.append('a command closing stdin may continue producing output')
        argument = "spaces ' quotes \" ; $(literal)"
        assert request(2, ['/usr/bin/printf', '%s', argument]) == (0, argument.encode(), b'')
        checks.append('argument boundaries preserved without an extra host shell')

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            assert request(2, ['/bin/pwd'], cwd=directory)[1].strip() == os.fsencode(directory)
            cfg = base/'config.json';client.save_config(cfg, config)
            assert stat.S_IMODE(cfg.stat().st_mode) == 0o600
            local, remote, pulled = base/'input', base/'guest file', base/'output'
            local.write_bytes(data)
            for args in [('push', str(local), str(remote)), ('pull', str(remote), str(pulled))]:
                subprocess.run(['python3', str(ROOT/'tools/pocketctl.py'), '--config', str(cfg), *args], check=True, timeout=10)
            assert pulled.read_bytes() == data
            # Failed pulls must preserve an existing destination.
            result = subprocess.run(['python3', str(ROOT/'tools/pocketctl.py'), '--config', str(cfg),
                                     'pull', str(base/'missing'), str(pulled)], capture_output=True, timeout=10)
            assert result.returncode != 0 and pulled.read_bytes() == data
        checks.append('CLI file transfer, failed-pull preservation and private token config')

        assert request(4)[0] == 0
        assert request(5)[:2] == (0, b'module-view-reset\n')
        assert request(2, ['/bin/sh', '-c', 'printf offline-owner-access'])[:2] == (0, b'offline-owner-access')
        assert request(3)[0] == 0
        assert request(5)[0] != 0
        assert request(2, ['/bin/true'])[0] == 0
        checks.append('guest lifecycle commands and authenticated owner execution while stopped')

        def sleeping():
            peer = client.connect(config)
            argv = ['/bin/sh', '-c', 'echo $$; exec sleep 30']
            peer.sendall(b'\2'+client.string('/')+struct.pack('!I', len(argv))+b''.join(map(client.string, argv)))
            peer.sendall(b'\2\0\0\0\0')  # EOF on stdin, but retain connection.
            kind, count = struct.unpack('!BI', client.exact(peer, 5))
            assert kind == 1
            return peer, int(client.exact(peer, count).strip())

        def gone(pid):
            for _ in range(40):
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    return
                time.sleep(.05)
            raise AssertionError('Foreground command survived disconnection/revocation')

        peer, pid = sleeping();peer.close();gone(pid)
        checks.append('disconnect stops a silent command even after stdin EOF')
        peer, pid = sleeping()
        fixture.stdin.write('rotate\n');fixture.stdin.flush()
        config['port'] = int(fixture.stdout.readline())
        gone(pid);peer.close()
        try:
            client.connect(config)
            raise AssertionError('Revoked token accepted')
        except OSError:
            pass
        config['token'] = '22'*32
        assert request(1)[0] == 0
        checks.append('revocation disconnects active commands and rejects the old token')

        report = {'scope': 'host protocol tests; not Android root validation', 'passed': checks}
        (ROOT/'out/admin-tests/protocol-validation.json').write_text(json.dumps(report, indent=2)+'\n')
        for check in checks:
            print('PASS:', check)
        print('Admin protocol:', len(checks), 'checks passed')
    finally:
        if fixture.poll() is None:
            fixture.stdin.close()
            try:
                fixture.wait(timeout=5)
            except subprocess.TimeoutExpired:
                fixture.kill();fixture.wait()
        assert fixture.returncode == 0


if __name__ == '__main__':
    main()
