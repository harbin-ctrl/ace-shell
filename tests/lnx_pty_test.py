#!/usr/bin/env python3
"""Exercise LNX's ACE-console PTY protocol and shell activation boundary."""

import errno
import os
import pathlib
import pty
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


CSI = b"\x9b"
BOUNDS_QUERY = CSI + b"0 q"
RESIZE_ENABLE = CSI + b"12{"
RESIZE_DISABLE = CSI + b"12}"


def fail(message, output=b""):
    print(message, file=sys.stderr)
    if output:
        print(repr(output), file=sys.stderr)
    raise SystemExit(1)


def protocol_output(output):
    """Return target output after removing LNX's public console protocol."""
    return (output.replace(BOUNDS_QUERY, b"")
                  .replace(RESIZE_ENABLE, b"")
                  .replace(RESIZE_DISABLE, b""))


def run_socket(lnx, arguments, *, environment=None, input_bytes=b"",
               input_chunks=None, input_delay=0, send_after=None,
               geometry_responses=None, close_after_input=False, timeout=8):
    """Run one LNX invocation against a responsive fake ACE console."""
    parent, child = socket.socketpair()
    process = None
    output = bytearray()
    controls = bytearray()
    pending = bytearray()
    chunks = list(input_chunks) if input_chunks is not None else [input_bytes]
    if not chunks or chunks == [b""]:
        chunks = []
    geometry_responses = list(geometry_responses or [])
    chunk_index = 0
    geometry_index = 0
    input_started = send_after is None
    next_chunk_at = 0
    input_closed = False
    deadline = time.monotonic() + timeout
    try:
        process = subprocess.Popen(
            [str(lnx), *[str(argument) for argument in arguments]],
            stdin=child, stdout=child, stderr=child, env=environment,
            close_fds=True,
        )
        child.close()
        parent.setblocking(False)
        while True:
            now = time.monotonic()
            if now >= deadline:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=1)
                fail(f"socket LNX command timed out: {arguments}", bytes(output))
            if (input_started and not pending and chunk_index < len(chunks) and
                    now >= next_chunk_at):
                pending.extend(chunks[chunk_index])
                chunk_index += 1

            wait = min(0.05, deadline - now)
            if input_started and not pending and chunk_index < len(chunks):
                wait = min(wait, max(0, next_chunk_at - now))
            readable = [parent]
            writable = [parent] if pending else []
            ready_read, ready_write, _ = select.select(readable, writable, [],
                                                        wait)
            if ready_read:
                try:
                    data = parent.recv(65536)
                except BlockingIOError:
                    data = None
                if data == b"":
                    break
                if data:
                    output.extend(data)
                    scan = bytes(controls) + data
                    position = 0
                    while True:
                        found = scan.find(BOUNDS_QUERY, position)
                        if found < 0:
                            break
                        if geometry_index < len(geometry_responses):
                            rows, cols = geometry_responses[geometry_index]
                            pending.extend(
                                CSI + f"1;1;{rows};{cols} r".encode()
                            )
                            geometry_index += 1
                        position = found + len(BOUNDS_QUERY)
                    controls[:] = scan[-(len(BOUNDS_QUERY) - 1):]
                    if not input_started and send_after in output:
                        input_started = True
                        next_chunk_at = time.monotonic()
            if ready_write and pending:
                try:
                    amount = parent.send(pending)
                except BlockingIOError:
                    amount = 0
                if amount:
                    del pending[:amount]
                    if not pending and chunk_index < len(chunks):
                        next_chunk_at = time.monotonic() + input_delay
            if (close_after_input and not input_closed and input_started and
                    chunk_index == len(chunks) and not pending):
                parent.shutdown(socket.SHUT_WR)
                input_closed = True
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


def assert_protocol(output, queries, *, context):
    if output.count(BOUNDS_QUERY) != queries:
        fail(f"{context} emitted the wrong number of bounds queries", output)
    if output.count(RESIZE_ENABLE) != 1 or output.count(RESIZE_DISABLE) != 1:
        fail(f"{context} did not set and reset raw resize mode exactly once",
             output)


def raw_bytes_line(payload):
    return b"bytes" + b"".join(b" %02x" % byte for byte in payload) + b"\n"


def test_isolated_supervisor(lnx, probe):
    baseline_status, baseline = run_socket(lnx, [probe], close_after_input=True)
    if baseline_status != 0 or b"isatty 0 0 0\n" not in baseline:
        fail("unmarked socket-backed LNX changed its characterization", baseline)

    host_pty = normalized(run_over_pty(lnx, probe))
    if b"isatty 1 1 1\n" not in host_pty:
        fail("host-PTY LNX positive control did not expose tty descriptors",
             host_pty)

    pty_environment = os.environ.copy()
    pty_environment["ACE_LNX_PTY"] = "1"
    pty_environment["COLORTERM"] = "truecolor"
    report_status, report = run_socket(
        lnx, [probe, "report"], environment=pty_environment,
        geometry_responses=[(37, 113)],
    )
    assert_protocol(report, 1, context="PTY report")
    report = normalized(protocol_output(report))
    if report_status != 0:
        fail(f"PTY report exited with {report_status}", report)
    for expected in (
        b"isatty 1 1 1\n", b"term xterm-256color\n",
        b"winsize 37 113\n", b"pty-marker (unset)\n",
        b"colorterm (unset)\n",
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
        fail("PTY child was not its own session/process-group/tty leader", report)

    line_status, line_output = run_socket(
        lnx, [probe, "line"], environment=pty_environment,
        input_bytes=b"interactive line\n", geometry_responses=[(24, 80)],
        send_after=BOUNDS_QUERY,
    )
    assert_protocol(line_output, 1, context="PTY line relay")
    line_output = normalized(protocol_output(line_output))
    if line_status != 0 or b"received interactive line\n" not in line_output:
        fail(f"PTY line relay exited with {line_status}", line_output)

    translated = (
        b"plain\x9bA\x9bB\x9bC\x9bD\x9bT\x9bS\x9b A\x9b @\x9bZ"
        b"\x9b40~\x9b41~\x9b42~\x9b44~\x9b45~\x9b50~\x9b54~\x9b55~"
        b"\x9b0~\x9b1~\x9b2~\x9b3~\x9b4~\x9b5~\x9b6~\x9b7~\x9b8~\x9b9~"
        b"\x9b10~\x9b11~\x9b12~\x9b13~\x9b14~\x9b15~\x9b16~\x9b17~\x9b18~\x9b19~"
        b"\b\x7f\033[A\xe0\xa0\x9b\x9b?after\x9b12;bad|\x9b" + b"1" * 63 + b"Z"
    )
    expected_input = (
        b"plain\033[A\033[B\033[C\033[D\033[1;2A\033[1;2B\033[1;2D\033[1;2C\033[Z"
        b"\033[2~\033[5~\033[6~\033OH\033OF\033[2;2~\033[1;2H\033[1;2F"
        b"\033OP\033OQ\033OR\033OS\033[15~\033[17~\033[18~\033[19~\033[20~\033[21~"
        b"\033[1;2P\033[1;2Q\033[1;2R\033[1;2S\033[15;2~\033[17;2~\033[18;2~\033[19;2~\033[20;2~\033[21;2~"
        b"\x7f\033[3~\033[A\xe0\xa0\x9b\x9b?after\x9b12;bad|\x9b" + b"1" * 63 + b"Z"
    )
    bytes_status, bytes_output = run_socket(
        lnx, [probe, "bytes", str(len(expected_input))],
        environment=pty_environment,
        input_chunks=[bytes([byte]) for byte in translated], input_delay=0.003,
        geometry_responses=[(24, 80)], send_after=BOUNDS_QUERY, timeout=12,
    )
    assert_protocol(bytes_output, 1, context="PTY input adaptation")
    bytes_output = normalized(protocol_output(bytes_output))
    if bytes_status != 0 or raw_bytes_line(expected_input) not in bytes_output:
        fail("PTY input adaptation changed or lost a key sequence", bytes_output)

    resize_status, resize_output = run_socket(
        lnx, [probe, "resize"], environment=pty_environment,
        input_bytes=CSI + b"12;1;1;0;0;0;0;0|",
        geometry_responses=[(24, 80), (55, 132)],
        send_after=b"resize-ready", timeout=8,
    )
    assert_protocol(resize_output, 2, context="PTY live resize")
    resize_plain = normalized(protocol_output(resize_output))
    if (resize_status != 0 or b"resize-ready 24 80\n" not in resize_plain or
            b"resize 55 132\n" not in resize_plain or b"12;1;1;" in resize_plain):
        fail("PTY resize did not reach the target as SIGWINCH", resize_plain)

    terminal_status, terminal_output = run_socket(
        lnx, [probe, "terminal-output"], environment=pty_environment,
        geometry_responses=[(24, 80)],
    )
    assert_protocol(terminal_output, 1, context="PTY output adaptation")
    terminal_output = protocol_output(terminal_output)
    expected_terminal = (
        b"plain\x9b1;1H\x9bJ\x9b1;1Hcursorredindexedbackground"
        b"\x9b1;1H\x9bJalt\x9b1;1H\x9bJrest\r\n"
    )
    if terminal_status != 0 or expected_terminal not in terminal_output:
        fail("PTY output adaptation did not reduce xterm output predictably",
             terminal_output)
    if b"\033[31m" in terminal_output or b"\033[38;5;196m" in terminal_output:
        fail("PTY output adaptation leaked unsupported xterm SGR", terminal_output)

    requested = 200000
    emit_status, emit_output = run_socket(
        lnx, [probe, "emit", str(requested)], environment=pty_environment,
        geometry_responses=[(24, 80)],
    )
    assert_protocol(emit_output, 1, context="PTY large relay")
    emit_output = protocol_output(emit_output)
    expected = bytes((ord("A") + index % 26 for index in range(requested)))
    if emit_status != 0 or emit_output != expected:
        fail(f"PTY relay truncated or changed large output with status {emit_status}",
             emit_output[:4096])

    for expected_status in (0, 7, 255):
        status, output = run_socket(
            lnx, [probe, "exit", str(expected_status)],
            environment=pty_environment, geometry_responses=[(24, 80)],
        )
        assert_protocol(output, 1, context=f"PTY exit {expected_status}")
        if status != expected_status:
            fail(f"PTY exit status {expected_status} became {status}", output)

    signal_status, signal_output = run_socket(
        lnx, [probe, "signal", str(signal.SIGTERM)],
        environment=pty_environment, geometry_responses=[(24, 80)],
    )
    assert_protocol(signal_output, 1, context="PTY signal")
    if signal_status != -signal.SIGTERM:
        fail(f"PTY signal termination became {signal_status}", signal_output)

    sentinel_environment = os.environ.copy()
    sentinel_environment["TERM"] = "ace-sentinel"
    direct = subprocess.run(
        [str(lnx), str(probe), "report"], env=sentinel_environment,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    direct_output = normalized(direct.stdout + direct.stderr)
    if direct.returncode != 0 or b"term ace-sentinel\n" not in direct_output:
        fail("direct LNX mode did not preserve the caller TERM", direct_output)

    payload = b"LNX direct stream regression\n"
    completed = subprocess.run(
        [str(lnx), "/bin/cat"], input=payload, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False,
    )
    if completed.returncode != 0 or completed.stdout != payload:
        fail("direct LNX cat did not preserve stream bytes or status",
             completed.stdout + completed.stderr)

    missing = subprocess.run(
        [str(lnx), "/ace/lnx-characterization-command-does-not-exist"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if missing.returncode == 0 or b"LNX:" not in missing.stderr:
        fail("missing direct LNX command did not fail diagnostically",
             missing.stdout + missing.stderr)


def run_interactive_shell(repo, lnx, probe):
    """Exercise RunCommand's live-console path through a private broker."""
    with tempfile.TemporaryDirectory(prefix="ace-lnx-shell-", dir=repo) as temp:
        root = pathlib.Path(temp)
        sys_dir = root / "sys"
        command_dir = sys_dir / "C"
        runtime_dir = root / "run"
        socket_path = root / "broker.sock"
        command_dir.mkdir(parents=True)
        (sys_dir / "S").mkdir()
        (sys_dir / "Prefs" / "Env-Archive").mkdir(parents=True)
        (runtime_dir / "ace" / "t").mkdir(parents=True)
        for binary in (lnx, repo / "build" / "EndCLI", probe):
            shutil.copy2(binary, command_dir / binary.name)

        environment = os.environ.copy()
        environment.update({
            "ACE_SYS_DIR": str(sys_dir),
            "ACE_BROKER_SOCKET": str(socket_path),
            "ACE_SESSION": "lnx-pty-test",
            "XDG_RUNTIME_DIR": str(runtime_dir),
            "ACE_CONSOLE_INTERACTIVE": "1",
            "TERM": "amiga",
        })
        broker = subprocess.Popen(
            [str(repo / "build" / "ace-broker"), str(socket_path)],
            env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        shell = None
        parent = child = None
        try:
            for _ in range(200):
                if socket_path.exists():
                    break
                if broker.poll() is not None:
                    fail("interactive LNX test broker exited before startup")
                time.sleep(0.01)
            if not socket_path.exists():
                fail("interactive LNX test broker did not start")

            parent, child = socket.socketpair()
            shell = subprocess.Popen(
                [str(repo / "build" / "ace-user-shell")], stdin=child,
                stdout=child, stderr=child, env=environment, close_fds=True,
            )
            child.close()
            child = None
            parent.setblocking(False)
            parent.sendall(b"LNX bash --noprofile --norc -i\n")
            output = bytearray()
            controls = bytearray()
            queries = 0
            bash_input_sent = False
            endcli_sent = False
            console_closed = False
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if shell.poll() is not None:
                    break
                readable, _, _ = select.select(
                    [] if console_closed else [parent], [], [], 0.05
                )
                if readable:
                    data = parent.recv(65536)
                    if data == b"":
                        console_closed = True
                    else:
                        output.extend(data)
                        scan = bytes(controls) + data
                        position = 0
                        while True:
                            found = scan.find(BOUNDS_QUERY, position)
                            if found < 0:
                                break
                            parent.sendall(CSI + b"1;1;30;100 r")
                            queries += 1
                            position = found + len(BOUNDS_QUERY)
                        controls[:] = scan[-(len(BOUNDS_QUERY) - 1):]
                        if queries and not bash_input_sent:
                            parent.sendall(
                                b"export PS1='LNX-PTY> '\necho LNX-INTERACTIVE-OK\nexit\n"
                            )
                            bash_input_sent = True
                        if (bash_input_sent and not endcli_sent and
                                b"LNX-INTERACTIVE-OK" in output and
                                output.count(RESIZE_DISABLE) == 1):
                            parent.sendall(b"EndCLI\n")
                            # The fake socket has no GUI owner to close it
                            # after EndCLI; this is the normal teardown.
                            parent.shutdown(socket.SHUT_WR)
                            endcli_sent = True
            if shell.poll() is None:
                shell.kill()
                shell.wait(timeout=1)
                fail("interactive ACE shell did not finish", bytes(output))
            if shell.returncode != 0:
                fail(f"interactive ACE shell exited with {shell.returncode}",
                     bytes(output))
            if (queries != 1 or output.count(RESIZE_ENABLE) != 1 or
                    output.count(RESIZE_DISABLE) != 1 or
                    b"LNX-INTERACTIVE-OK" not in output):
                fail("RunCommand did not activate one interactive LNX PTY",
                     bytes(output))
            lower = bytes(output).lower()
            if b"no job control" in lower or b"cannot set terminal process group" in lower:
                fail("interactive Bash did not acquire LNX job control",
                     bytes(output))
        finally:
            if parent is not None:
                parent.close()
            if child is not None:
                child.close()
            if shell is not None and shell.poll() is None:
                shell.kill()
                shell.wait(timeout=1)
            if broker.poll() is None:
                broker.terminate()
                broker.wait(timeout=2)


def main():
    repo = pathlib.Path(__file__).resolve().parent.parent
    lnx = repo / "build" / "LNX"
    probe = repo / "build" / "lnx-pty-probe"
    if not lnx.exists() or not probe.exists():
        fail("LNX PTY binaries are not built")
    test_isolated_supervisor(lnx, probe)
    run_interactive_shell(repo, lnx, probe)
    print("LNX PTY protocol, adaptation, activation, and direct path pass")


if __name__ == "__main__":
    main()
