#!/usr/bin/env python3
"""Characterize LNX's current stream boundary before PTY integration.

Chunk 0 deliberately asserts the behavior that the later chunks will replace:
LNX can carry bytes through a socket, but the Linux child does not see a tty.
The same probe run through a host PTY establishes the positive control.
"""

import errno
import os
import pathlib
import pty
import socket
import subprocess
import sys


def fail(message, output=b""):
    print(message, file=sys.stderr)
    if output:
        print(repr(output), file=sys.stderr)
    raise SystemExit(1)


def run_over_socket(lnx, probe):
    parent, child = socket.socketpair()
    try:
        process = subprocess.Popen(
            [str(lnx), str(probe)],
            stdin=child,
            stdout=child,
            stderr=child,
            close_fds=True,
        )
    finally:
        child.close()

    output = bytearray()
    try:
        while True:
            data = parent.recv(4096)
            if not data:
                break
            output.extend(data)
    finally:
        parent.close()
    status = process.wait(timeout=5)
    if status != 0:
        fail(f"socket LNX probe exited with {status}", output)
    return bytes(output)


def run_over_pty(lnx, probe):
    pid, master = pty.fork()
    if pid == 0:
        os.execv(str(lnx), [str(lnx), str(probe)])

    output = bytearray()
    try:
        while True:
            try:
                data = os.read(master, 4096)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not data:
                break
            output.extend(data)
    finally:
        os.close(master)
    waited, status = os.waitpid(pid, 0)
    if waited != pid or not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
        fail(f"host-PTY LNX probe exited with status {status}", output)
    return bytes(output)


def main():
    repo = pathlib.Path(__file__).resolve().parent.parent
    lnx = repo / "build" / "LNX"
    probe = repo / "build" / "lnx-pty-probe"
    if not lnx.exists() or not probe.exists():
        fail("LNX characterization binaries are not built")

    socket_output = run_over_socket(lnx, probe)
    if b"isatty 0 0 0\n" not in socket_output:
        fail("socket-backed LNX no longer has the expected non-tty baseline",
             socket_output)

    pty_output = run_over_pty(lnx, probe).replace(b"\r\n", b"\n")
    if b"isatty 1 1 1\n" not in pty_output:
        fail("host-PTY positive control did not expose tty descriptors",
             pty_output)

    payload = b"LNX direct stream regression\n"
    completed = subprocess.run(
        [str(lnx), "/bin/cat"],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0 or completed.stdout != payload:
        fail("direct LNX cat did not preserve stream bytes or status",
             completed.stdout + completed.stderr)

    missing = subprocess.run(
        [str(lnx), "/ace/lnx-characterization-command-does-not-exist"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if missing.returncode == 0 or b"LNX:" not in missing.stderr:
        fail("missing direct LNX command did not fail diagnostically",
             missing.stdout + missing.stderr)

    print("LNX characterization: socket stream, host PTY, direct bytes, and failure status pass")


if __name__ == "__main__":
    main()
