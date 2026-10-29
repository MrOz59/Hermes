#!/usr/bin/env python3
"""Build and test the actual private Wayland helper without KDE or a GPU.

--no-listener runs only the socketpair/failure tests for restricted sandboxes.
This does not replace the packaged-Hermes KWin/Artemis acceptance test.
"""
import argparse
import array
import os
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import tempfile
import time


def checked(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-listener", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="hermes-wayland-peer-") as temporary:
        work = Path(temporary)
        helper = work / "hermes-detached-session"
        probe = work / "hermes-wayland-peer-check"
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        for source, binary in (("hermes-detached-session.cpp", helper), ("hermes-wayland-peer-check.cpp", probe)):
            checked(compiler + ["-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", str(root / "tools" / source), "-o", str(binary)])
        print("PASS: actual supervisor/helper and production connection consumer compile")

        # Exercise descriptor receipt, handshake lifetime and reaping through
        # the production client using a fresh exec and socketpair (no listener).
        fixture = work / "peer-fixture"
        fixture.write_text('''#!/usr/bin/python3
import array, os, socket
channel = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET, fileno=3)
assert set(os.listdir('/proc/self/fd')) <= {'0', '3'}
assert not any(k.startswith(('LD_', 'WAYLAND_', 'PIPEWIRE_', 'HERMES_TEST_')) for k in os.environ)
client, server = socket.socketpair()
channel.sendmsg([b'\\x01'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [client.fileno()]))])
client.close()
server.settimeout(5)
assert server.recv(5) == b'ready'
server.sendall(b'ok')
channel.recv(1)
''')
        fixture.chmod(0o700)
        # Deliberately leave a non-CLOEXEC fd open in the caller. The fixture
        # asserts it did not survive helper exec.
        leak = os.open('/dev/null', os.O_RDONLY)
        os.set_inheritable(leak, True)
        try:
            result = subprocess.run([str(probe), 'relay', str(fixture), str(work / 'unused')], pass_fds=(leak,), capture_output=True, text=True, timeout=10,
                                    env={**os.environ, 'HERMES_TEST_SECRET': 'must-not-reach-helper'})
            assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        finally:
            os.close(leak)
        print('PASS: production FD handoff, helper lifetime/reaping, descriptor/environment isolation, parent state preserved')

        for executable, path in ((helper, 'relative'), (helper, str(work / 'missing')), (work / 'missing-helper', str(work / 'socket'))):
            checked([str(probe), 'failure', str(executable), path])
        print('PASS: invalid/missing socket and missing helper fail and reap without host fallback')

        if args.no_listener:
            print('NOT RUN: real AF_UNIX connect and peer executable identity (listener tests disabled)')
            return

        for mode in ('direct', 'relay'):
            path = str(work / mode)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(path)
                listener.listen(4)
                listener.settimeout(5)
                process = subprocess.Popen([str(probe), mode, str(helper), path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(5)
                        assert connection.recv(5) == b'ready'
                        pid, uid, gid = struct.unpack('3i', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
                        assert (uid, gid) == (os.getuid(), os.getgid())
                        try:
                            executable = os.readlink(f'/proc/{pid}/exe')
                        except PermissionError:
                            executable = None
                        if mode == 'direct':
                            assert pid == process.pid and executable is None
                        else:
                            assert pid != process.pid and executable == str(helper)
                            fields = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines() if ':' in line)
                            assert all(int(fields[key].strip(), 16) == 0 for key in ('CapPrm', 'CapEff', 'CapInh', 'CapAmb'))
                            assert fields['NoNewPrivs'].strip() == '1'
                        connection.sendall(b'ok')
                    out, err = process.communicate(timeout=10)
                    assert process.returncode == 0, (process.returncode, out, err)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
            print(f'PASS: {mode} peer identity; parent permissions unchanged')

        path = str(work / 'full')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(path)
            listener.listen(0)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as queued:
                queued.connect(path)
                started = time.monotonic()
                checked([str(probe), 'failure', str(helper), path])
                assert time.monotonic() - started < 5
        print('PASS: full socket backlog fails promptly without fallback')


if __name__ == '__main__':
    main()
