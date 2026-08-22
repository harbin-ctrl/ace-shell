#!/usr/bin/env python3
"""Exercise LNX's isolated PTY supervisor and preserve its stream path.

Chunk 1 opts into the supervisor explicitly with ACE_LNX_PTY=1.  Production
RunCommand() does not set that marker until the later descriptor-selection
chunk, so the unmarked socket test remains the original characterization.
"""

import errno
import os
import pathlib
import pty
import select
import socket
import signal
import subprocess
import sys
import time


def fail(message, output=b""):
    print(message, file=sys.stderr)
    if output:
        print(repr(output), file=sys.stderr)
    raise SystemExit(1)


def run_socket(lnx, arguments, *, environment=None, input_bytes=b"", timeout=5):
    parent, child = socket.socketpair()
    process = None
    output = bytearray()
    sent = 0
    input_closed = False
    deadline = time.monotonic() + timeout
    try:
        process = subprocess.Popen(
            [str(lnx), *[str(argument) for argument in arguments]],
            stdin=child,
            stdout=child,
            stderr=child,
            env=environment,
            close_fds=True,
        )
        child.close()
        parent.setblocking(False)
        if not input_bytes:
            parent.shutdown(socket.SHUT_WR)
            input_closed = True
        while True:
            if time.monotonic() >= deadline:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=1)
                fail(f"socket LNX command timed out: {arguments}", output)
            readable = [parent]
            writable = [] if input_closed else [parent]
            ready_read, ready_write, _ = select.select(
                readable, writable, [], min(0.1, deadline - time.monotonic())
            )
            if ready_write:
                amount = parent.send(input_bytes[sent:])
                sent += amount
                if sent == len(input_bytes):
                    parent.shutdown(socket.SHUT_WR)
                    input_closed = True
            if ready_read:
                try:
                    data = parent.recv(65536)
                except BlockingIOError:
                    data = None
                if data == b"":
                    break
                if data:
                    output.extend(data)
            if process.poll() is not None and not ready_read:
                # The next select/recv observes the socket EOF.  Keeping the
                # loop here drains bytes already queued by the supervisor.
                continue
        status = process.wait(timeout=1)
        return status, bytes(output)
    except (OSError, subprocess.TimeoutExpired) as error:
        captured = bytes(output)
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=1)
        fail(f"socket LNX command failed: {arguments}: {error}", captured)
    finally:
        parent.close()
        if child.fileno() >= 0:
            child.close()


def run_over_pty(lnx, probe):
    pid, master = pty.fork()
    if pid == 0:
        os.execv(str(lnx), [str(lnx), str(probe), "report"])

    output = bytearray()
    deadline = time.monotonic() + 5
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
                fail("host-PTY LNX probe timed out", output)
            readable, _, _ = select.select([master], [], [], remaining)
            if not readable:
                continue
            try:
                data = os.read(master, 4096)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not data:
                break
            output.extend(data)
    except BaseException:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass
        raise
    finally:
        os.close(master)
    waited, status = os.waitpid(pid, 0)
    if waited != pid or not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
        fail(f"host-PTY LNX probe exited with status {status}", output)
    return bytes(output)


def normalized(output):
    return output.replace(b"\r\n", b"\n")


def main():
    repo = pathlib.Path(__file__).resolve().parent.parent
    lnx = repo / "build" / "LNX"
    probe = repo / "build" / "lnx-pty-probe"
    if not lnx.exists() or not probe.exists():
        fail("LNX PTY binaries are not built")

    baseline_status, baseline = run_socket(lnx, [probe])
    if baseline_status != 0 or b"isatty 0 0 0\n" not in baseline:
        fail("unmarked socket-backed LNX changed its characterization",
             baseline)

    host_pty = normalized(run_over_pty(lnx, probe))
    if b"isatty 1 1 1\n" not in host_pty:
        fail("host-PTY LNX positive control did not expose tty descriptors",
             host_pty)

    pty_environment = os.environ.copy()
    pty_environment["ACE_LNX_PTY"] = "1"
    report_status, report = run_socket(
        lnx, [probe, "report"], environment=pty_environment
    )
    report = normalized(report)
    if report_status != 0:
        fail(f"PTY report exited with {report_status}", report)
    for expected in (
        b"isatty 1 1 1\n",
        b"term xterm-256color\n",
        b"pty-marker (unset)\n",
    ):
        if expected not in report:
            fail(f"PTY report lacked {expected!r}", report)
    identity = next(
        (line for line in report.splitlines() if line.startswith(b"identity ")),
        b"",
    )
    fields = identity.split()
    if len(fields) != 11 or not (
        fields[2] == fields[4] == fields[6] == fields[8] == fields[10]
    ):
        fail("PTY child was not its own session/process-group/tty leader",
             report)

    line_status, line_output = run_socket(
        lnx, [probe, "line"], environment=pty_environment,
        input_bytes=b"interactive line\n",
    )
    line_output = normalized(line_output)
    if line_status != 0 or b"received interactive line\n" not in line_output:
        fail(f"PTY line relay exited with {line_status}", line_output)

    requested = 200000
    emit_status, emit_output = run_socket(
        lnx, [probe, "emit", str(requested)], environment=pty_environment
    )
    if emit_status != 0 or len(emit_output) != requested:
        fail(f"PTY relay truncated large output with status {emit_status}",
             emit_output)
    expected = bytes((ord("A") + index % 26 for index in range(requested)))
    if emit_output != expected:
        fail("PTY relay changed large output bytes", emit_output[:4096])

    for expected_status in (0, 7, 255):
        status, output = run_socket(
            lnx, [probe, "exit", str(expected_status)],
            environment=pty_environment,
        )
        if status != expected_status:
            fail(f"PTY exit status {expected_status} became {status}", output)

    signal_status, signal_output = run_socket(
        lnx, [probe, "signal", str(signal.SIGTERM)],
        environment=pty_environment,
    )
    if signal_status != -signal.SIGTERM:
        fail(f"PTY signal termination became {signal_status}", signal_output)

    sentinel_environment = os.environ.copy()
    sentinel_environment["TERM"] = "ace-sentinel"
    direct = subprocess.run(
        [str(lnx), str(probe), "report"],
        env=sentinel_environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    direct_output = normalized(direct.stdout + direct.stderr)
    if direct.returncode != 0 or b"term ace-sentinel\n" not in direct_output:
        fail("direct LNX mode did not preserve the caller TERM", direct_output)

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

    print("LNX PTY supervisor: descriptors, relay, status, signal, and direct path pass")


if __name__ == "__main__":
    main()
