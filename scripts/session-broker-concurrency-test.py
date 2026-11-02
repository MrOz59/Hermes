#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the real broker's accept loop without accounts, devices or systemd."""
import fcntl
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time


def request(path, text, timeout=2):
    peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    peer.settimeout(timeout)
    peer.connect(str(path))
    peer.sendall(text.encode())
    peer.shutdown(socket.SHUT_WR)
    return peer


def reply(peer):
    data = b""
    while b"\n" not in data:
        chunk = peer.recv(4096)
        if not chunk:
            break
        data += chunk
    return data.decode().strip()


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="hermes-concurrency-") as directory:
        root = Path(directory)
        path, lock = root / "socket", root / "mutation.lock"
        (root / "allow").write_text(f"{os.getuid()}\n")
        (root / "map").write_text("")
        (root / "map").chmod(0o600)
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(path))
        listener.listen(16)
        # systemd normally provides fd 3 and LISTEN_PID. Supply only our own
        # socket; all paths are private fixtures and no ENSURE/START is sent.
        launcher = """
import os, sys
fd = int(sys.argv[1])
os.dup2(fd, 3, inheritable=True)
if fd != 3: os.close(fd)
os.environ['LISTEN_PID'] = str(os.getpid())
os.environ['LISTEN_FDS'] = '1'
os.execv(sys.argv[2], sys.argv[2:])
"""
        with (root / "journal").open("w+") as journal:
            process = subprocess.Popen(
                [sys.executable, "-c", launcher, str(listener.fileno()), binary,
                 "--allow-file", str(root / "allow"), "--state-file", str(root / "map"),
                 "--home-root", str(root / "homes"), "--lock-file", str(lock)],
                pass_fds=(listener.fileno(),), start_new_session=True,
                stdout=journal, stderr=journal,
            )
            listener.close()
            try:
                with request(path, "LIST\n") as peer:
                    assert reply(peer) == "OK", "fixture broker did not start"

                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as slow:
                    slow.connect(str(path))
                    slow.sendall(b"START abcd\nARG /usr/bin/false\n")
                    # Allow the incomplete request to reach a worker first.
                    time.sleep(0.05)
                    started = time.monotonic()
                    with request(path, "LOOKUP dcba\n") as peer:
                        assert reply(peer).startswith("ERR notfound")
                    assert time.monotonic() - started < 2
                print("PASS: an incomplete request does not block read-only RPCs")

                descriptor = os.open(lock, os.O_RDWR | os.O_CREAT, 0o600)
                try:
                    fcntl.flock(descriptor, fcntl.LOCK_EX)
                    with request(path, "STOP abcd\n") as first, request(path, "STOP dcba\n") as second:
                        assert not select.select([first, second], [], [], 0.05)[0], "mutations escaped their lock"
                        with request(path, "LIST\n") as peer:
                            assert reply(peer) == "OK", "queued mutations blocked liveness traffic"
                        fcntl.flock(descriptor, fcntl.LOCK_UN)
                        assert reply(first).startswith("ERR notfound")
                        assert reply(second).startswith("ERR notfound")
                    print("PASS: mutations queue while read-only RPCs remain available")

                    os.fchmod(descriptor, 0o666)
                    started = time.monotonic()
                    with request(path, "STOP abcd\n") as peer:
                        assert reply(peer).startswith("ERR busy")
                    assert time.monotonic() - started < 2, "an unsafe lock was retried"
                    print("PASS: an unsafe mutation lock fails immediately")
                finally:
                    os.close(descriptor)
            except BaseException:
                journal.flush()
                journal.seek(0)
                sys.stderr.write(journal.read())
                raise
            finally:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=3)


if __name__ == "__main__":
    main()
