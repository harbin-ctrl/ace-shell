#!/usr/bin/env python3
"""Exercise Linux's ACE-console PTY protocol and shell activation boundary."""

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
    """Return target output after removing Linux's public console protocol."""
    return (output.replace(BOUNDS_QUERY, b"")
                  .replace(RESIZE_ENABLE, b"")
                  .replace(RESIZE_DISABLE, b""))


def run_socket(lnx, arguments, *, environment=None, input_bytes=b"",
               input_chunks=None, input_delay=0, send_after=None,
               geometry_responses=None, close_after_input=False,
               close_after_geometry=False, delay_output_read=0,
               signals_after_input=None, timeout=8):
    """Run one Linux invocation against a responsive fake ACE console."""
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
    read_block_until = 0
    signals_sent = False
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
                fail(f"socket Linux command timed out: {arguments}", bytes(output))
            if (input_started and not pending and chunk_index < len(chunks) and
                    now >= next_chunk_at):
                pending.extend(chunks[chunk_index])
                chunk_index += 1

            wait = min(0.05, deadline - now)
            if input_started and not pending and chunk_index < len(chunks):
                wait = min(wait, max(0, next_chunk_at - now))
            readable = [] if now < read_block_until else [parent]
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
                            if delay_output_read and not read_block_until:
                                read_block_until = (time.monotonic() +
                                                    delay_output_read)
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
            if (signals_after_input and not signals_sent and input_started and
                    chunk_index == len(chunks) and not pending):
                for signal_number in signals_after_input:
                    os.kill(process.pid, signal_number)
                signals_sent = True
            if (close_after_input and not input_closed and input_started and
                    chunk_index == len(chunks) and not pending):
                parent.shutdown(socket.SHUT_WR)
                input_closed = True
            if (close_after_geometry and not input_closed and
                    geometry_index and chunk_index == len(chunks) and
                    not pending):
                parent.shutdown(socket.SHUT_WR)
                input_closed = True
        status = process.wait(timeout=1)
        return status, bytes(output)
    except (OSError, subprocess.TimeoutExpired) as error:
        captured = bytes(output)
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=1)
        fail(f"socket Linux command failed: {arguments}: {error}", captured)
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
                fail("host-PTY Linux probe timed out", output)
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
        fail(f"host-PTY Linux probe exited with status {status}", output)
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
        fail("unmarked socket-backed Linux changed its characterization", baseline)

    host_pty = normalized(run_over_pty(lnx, probe))
    if b"isatty 1 1 1\n" not in host_pty:
        fail("host-PTY Linux positive control did not expose tty descriptors",
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
        b"\x9b1;1H\x9bJalt\x9b1;1H\x9bJrest"
        b'Bot"filename" 123L, 456B'
        b"pi@frambo:~ $prompt\r\n"
    )
    if terminal_status != 0 or expected_terminal not in terminal_output:
        fail("PTY output adaptation did not reduce xterm output predictably",
             terminal_output)
    if b"\033[31m" in terminal_output or b"\033[38;5;196m" in terminal_output:
        fail("PTY output adaptation leaked unsupported xterm SGR", terminal_output)
    if b"\033[1;28r" in terminal_output or b"[1;28r" in terminal_output:
        fail("PTY output adaptation leaked xterm scrolling margins",
             terminal_output)

    incomplete_status, incomplete_output = run_socket(
        lnx, [probe, "terminal-output-incomplete"],
        environment=pty_environment, geometry_responses=[(24, 80)],
    )
    assert_protocol(incomplete_output, 1, context="PTY incomplete output")
    incomplete_output = protocol_output(incomplete_output)
    if incomplete_status != 0 or b"incomplete\033[" not in incomplete_output:
        fail("PTY did not flush an incomplete terminal sequence at EOF",
             incomplete_output)

    c1_status, c1_output = run_socket(
        lnx, [probe, "c1-output"], environment=pty_environment,
        geometry_responses=[(24, 80)],
    )
    assert_protocol(c1_output, 1, context="PTY target C1 output")
    c1_output = normalized(protocol_output(c1_output))
    if c1_status != 0 or b"target\x9b99~output\n" not in c1_output:
        fail("PTY target output was mistaken for ACE input controls", c1_output)

    requested = 1024 * 1024
    emit_status, emit_output = run_socket(
        lnx, [probe, "emit", str(requested)], environment=pty_environment,
        geometry_responses=[(24, 80)], delay_output_read=0.75, timeout=12,
    )
    assert_protocol(emit_output, 1, context="PTY backpressure relay")
    emit_output = protocol_output(emit_output)
    expected = bytes((ord("A") + index % 26 for index in range(requested)))
    if emit_status != 0 or emit_output != expected:
        fail(f"PTY backpressure relay truncated or changed output with status {emit_status}",
             emit_output[:4096])

    close_status, close_output = run_socket(
        lnx, [probe, "emit", "300000"], environment=pty_environment,
        geometry_responses=[(24, 80)],
        close_after_geometry=True, timeout=12,
    )
    assert_protocol(close_output, 1, context="PTY output after input EOF")
    close_output = protocol_output(close_output)
    close_expected = bytes((ord("A") + index % 26 for index in range(300000)))
    if close_status != 0 or close_output != close_expected:
        fail("PTY lost target output when ACE input closed", close_output[:4096])

    timeout_status, timeout_output = run_socket(
        lnx, [probe, "report"], environment=pty_environment,
        geometry_responses=[], timeout=8,
    )
    assert_protocol(timeout_output, 1, context="PTY geometry timeout")
    timeout_output = normalized(protocol_output(timeout_output))
    if timeout_status != 0 or b"winsize 24 80\n" not in timeout_output:
        fail("PTY geometry timeout did not use the safe fallback", timeout_output)

    exec_status, exec_output = run_socket(
        lnx, ["/ace/lnx-pty-child-does-not-exist"],
        environment=pty_environment, geometry_responses=[(24, 80)],
    )
    assert_protocol(exec_output, 1, context="PTY child exec failure")
    exec_output = normalized(protocol_output(exec_output))
    if exec_status == 0 or b"Linux: /ace/lnx-pty-child-does-not-exist:" not in exec_output:
        fail("PTY child exec failure was not reported or propagated",
             exec_output)

    eof_status, eof_output = run_socket(
        lnx, [probe, "bytes-ready", "1"], environment=pty_environment,
        input_bytes=b"\x9b", geometry_responses=[(24, 80)],
        send_after=b"raw-ready", close_after_input=True,
    )
    assert_protocol(eof_output, 1, context="PTY incomplete input EOF")
    eof_output = normalized(eof_output)
    if (eof_status != 0 or b"raw-ready\n" not in eof_output or
            raw_bytes_line(b"\x9b") not in eof_output):
        fail("PTY did not flush an incomplete ACE input sequence at EOF",
             eof_output)

    queued_input = b"A" * 65536
    queued_signal_count = 32
    signal_status, signal_output = run_socket(
        lnx, [probe, "delay-bytes",
              str(len(queued_input) + queued_signal_count)],
        environment=pty_environment, input_bytes=queued_input,
        geometry_responses=[(24, 80)], send_after=b"raw-ready",
        signals_after_input=[signal.SIGRTMIN] * queued_signal_count, timeout=12,
    )
    assert_protocol(signal_output, 1, context="PTY queued signal relay")
    signal_output = normalized(protocol_output(signal_output))
    if (signal_status != 0 or signal_output.count(b" 41") != len(queued_input) or
            signal_output.count(b" 05") != queued_signal_count):
        fail("PTY lost break events while its input queue was full",
             signal_output[:4096])

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
        fail("direct Linux mode did not preserve the caller TERM", direct_output)

    payload = b"Linux direct stream regression\n"
    completed = subprocess.run(
        [str(lnx), "/bin/cat"], input=payload, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False,
    )
    if completed.returncode != 0 or completed.stdout != payload:
        fail("direct Linux cat did not preserve stream bytes or status",
             completed.stdout + completed.stderr)

    missing = subprocess.run(
        [str(lnx), "/ace/lnx-characterization-command-does-not-exist"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if missing.returncode == 0 or b"Linux:" not in missing.stderr:
        fail("missing direct Linux command did not fail diagnostically",
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
                    fail("interactive Linux test broker exited before startup")
                time.sleep(0.01)
            if not socket_path.exists():
                fail("interactive Linux test broker did not start")

            parent, child = socket.socketpair()
            shell = subprocess.Popen(
                [str(repo / "build" / "ace-user-shell")], stdin=child,
                stdout=child, stderr=child, env=environment, close_fds=True,
            )
            child.close()
            child = None
            parent.setblocking(False)
            parent.sendall(b"Linux bash --noprofile --norc -i\n")
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
                                b"export PS1='Linux-PTY> '\necho Linux-INTERACTIVE-OK\nexit\n"
                            )
                            bash_input_sent = True
                        if (bash_input_sent and not endcli_sent and
                                b"Linux-INTERACTIVE-OK" in output and
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
                    b"Linux-INTERACTIVE-OK" not in output):
                fail("RunCommand did not activate one interactive Linux PTY",
                     bytes(output))
            lower = bytes(output).lower()
            if b"no job control" in lower or b"cannot set terminal process group" in lower:
                fail("interactive Bash did not acquire Linux job control",
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


class LiveShell:
    """Private ACE console fixture with protocol responses and signal access."""

    def __init__(self, repo, lnx, probe, command=None):
        self.repo = repo
        self.temp = tempfile.TemporaryDirectory(prefix="ace-lnx-break-",
                                                  dir=repo)
        root = pathlib.Path(self.temp.name)
        sys_dir = root / "sys"
        command_dir = sys_dir / "C"
        runtime_dir = root / "run"
        self.socket_path = root / "broker.sock"
        command_dir.mkdir(parents=True)
        (sys_dir / "S").mkdir()
        (sys_dir / "Prefs" / "Env-Archive").mkdir(parents=True)
        (runtime_dir / "ace" / "t").mkdir(parents=True)
        for binary in (lnx, repo / "build" / "EndCLI", probe):
            shutil.copy2(binary, command_dir / binary.name)

        self.environment = os.environ.copy()
        self.environment.update({
            "ACE_SYS_DIR": str(sys_dir),
            "ACE_BROKER_SOCKET": str(self.socket_path),
            "ACE_SESSION": "lnx-break-test",
            "XDG_RUNTIME_DIR": str(runtime_dir),
            "ACE_CONSOLE_INTERACTIVE": "1",
            "TERM": "amiga",
            "HOME": str(root),
        })
        self.broker = subprocess.Popen(
            [str(repo / "build" / "ace-broker"), str(self.socket_path)],
            env=self.environment, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.shell = None
        self.parent = None
        self.output = bytearray()
        self.controls = bytearray()
        self.queries = 0
        self.console_closed = False
        try:
            for _ in range(200):
                if self.socket_path.exists():
                    break
                if self.broker.poll() is not None:
                    fail("break test broker exited before startup")
                time.sleep(0.01)
            if not self.socket_path.exists():
                fail("break test broker did not start")
            self.parent, child = socket.socketpair()
            self.shell = subprocess.Popen(
                [str(repo / "build" / "ace-user-shell")], stdin=child,
                stdout=child, stderr=child, env=self.environment, close_fds=True,
            )
            child.close()
            self.parent.setblocking(True)
            self.send(command or b"Linux bash --noprofile --norc -i\n")
        except BaseException:
            self.close(force=True)
            raise

    def send(self, payload):
        if self.parent is None or self.console_closed:
            fail("attempted to send to a closed fake console", bytes(self.output))
        self.parent.sendall(payload)

    def pump(self, timeout=0.05):
        if self.console_closed:
            return
        readable, _, _ = select.select([self.parent], [], [], timeout)
        if not readable:
            return
        data = self.parent.recv(65536)
        if data == b"":
            self.console_closed = True
            return
        self.output.extend(data)
        scan = bytes(self.controls) + data
        position = 0
        while True:
            found = scan.find(BOUNDS_QUERY, position)
            if found < 0:
                break
            self.parent.sendall(CSI + b"1;1;30;100 r")
            self.queries += 1
            position = found + len(BOUNDS_QUERY)
        self.controls[:] = scan[-(len(BOUNDS_QUERY) - 1):]

    def wait_for(self, predicate, timeout=8, message="live shell timed out"):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate(bytes(self.output)):
                return
            if self.shell is not None and self.shell.poll() is not None:
                fail(f"{message}; shell exited {self.shell.returncode}",
                     bytes(self.output))
            self.pump()
        fail(message, bytes(self.output))

    def close_console(self):
        if self.parent is not None and not self.console_closed:
            self.parent.close()
            self.console_closed = True

    def close(self, force=False):
        if self.parent is not None:
            try:
                self.parent.close()
            except OSError:
                pass
            self.parent = None
        if self.shell is not None and self.shell.poll() is None:
            if force:
                self.shell.kill()
            try:
                self.shell.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.shell.kill()
                self.shell.wait(timeout=1)
        if self.broker.poll() is None:
            self.broker.terminate()
            try:
                self.broker.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.broker.kill()
                self.broker.wait(timeout=1)
        self.temp.cleanup()


def run_interactive_break_tests(repo, lnx, probe, foreground_spoof):
    fixture = LiveShell(repo, lnx, probe)
    prompt = b"Linux-PTY> "
    try:
        fixture.wait_for(lambda output: fixture.queries >= 1,
                         message="Bash PTY query was not answered")
        fixture.send(b"export PS1='Linux-'P'TY> '\nprintf 'BREAK-'R'-READY\\n'\n")
        fixture.wait_for(
            lambda output: b"BREAK-R-READY" in output and
            output.count(prompt) >= 2,
            message="Bash did not reach its deterministic prompt",
        )

        fixture.send(b"printf 'SLEEP-'C'-START\\n'; sleep 30\n")
        fixture.wait_for(lambda output: b"SLEEP-C-START" in output,
                         message="Bash sleep command was not started")
        time.sleep(0.5)
        spoof = subprocess.Popen(
            [str(foreground_spoof)], env=fixture.environment,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            ready = spoof.stdout.readline()
            if ready != b"foreground-spoof-ready\n":
                fail("foreground decoy did not start",
                     ready + spoof.stderr.read())
            os.kill(fixture.shell.pid, signal.SIGUSR1)
            fixture.wait_for(
                lambda output: output.count(prompt) >= 3,
                message=("SIGUSR1 followed stale broker foreground state "
                         "instead of interrupting Bash"),
            )
        finally:
            if spoof.poll() is None:
                spoof.terminate()
            spoof.wait(timeout=2)

        fixture.send(b"printf 'SLEEP-'Z'-START\\n'; sleep 30\n")
        fixture.wait_for(lambda output: b"SLEEP-Z-START" in output,
                         message="second Bash sleep command was not started")
        time.sleep(0.5)
        fixture.send(b"\x1a")
        fixture.wait_for(
            lambda output: b"Stopped" in output and output.count(prompt) >= 4,
            message="Ctrl-Z did not stop Bash's foreground job",
        )
        stopped_output = bytes(fixture.output)
        fixture.send(b"jobs\n")
        fixture.wait_for(lambda output: output.count(prompt) >= 5,
                         message="Bash jobs command did not return")
        if b"Stopped" not in bytes(fixture.output):
            fail("Bash jobs output lost the stopped job", bytes(fixture.output))
        fixture.send(b"fg\n")
        fixture.wait_for(lambda output: output.count(b"fg") >= 1,
                         message="Bash fg command was not accepted")
        time.sleep(0.5)
        os.kill(fixture.shell.pid, signal.SIGUSR1)
        fixture.wait_for(lambda output: output.count(prompt) >= 6,
                         message="Ctrl-C did not interrupt the resumed job")
        if b"Stopped" not in stopped_output:
            fail("Ctrl-Z output did not report a stopped job", bytes(fixture.output))

        fixture.send(b"exit\n")
        fixture.wait_for(lambda output: output.count(RESIZE_DISABLE) == 1,
                         message="job-control Bash did not exit cleanly")
        fixture.send(f"Linux {probe} bytes 2\n".encode())
        fixture.wait_for(lambda output: fixture.queries >= 2,
                         message="raw-byte probe PTY query was not answered")
        time.sleep(0.5)
        os.kill(fixture.shell.pid, signal.SIGRTMIN)
        os.kill(fixture.shell.pid, signal.SIGRTMIN + 1)
        fixture.wait_for(lambda output: b"bytes 05 06" in output,
                         message="Ctrl-E/Ctrl-F did not reach the raw PTY target")
        if fixture.output.count(RESIZE_DISABLE) != 2:
            fixture.wait_for(lambda output: output.count(RESIZE_DISABLE) == 2,
                             message="raw-byte probe did not close its PTY")

        fixture.send(b"Linux bash --noprofile --norc -i\n")
        fixture.wait_for(lambda output: fixture.queries >= 3,
                         message="EOF Bash PTY query was not answered")
        fixture.send(b"export PS1='Linux-'P'TY> '\nprintf 'EOF-'R'-READY\\n'\n")
        fixture.wait_for(
            lambda output: b"EOF-R-READY" in output and output.count(prompt) >= 8,
            message="EOF Bash did not reach an empty prompt",
        )
        os.kill(fixture.shell.pid, signal.SIGUSR2)
        fixture.wait_for(lambda output: output.count(RESIZE_DISABLE) == 3,
                         message="SIGUSR2 did not deliver terminal EOF to Bash")
        if b"Shell: ***Break" in bytes(fixture.output):
            fail("Linux Ctrl-D was misclassified as an ACE script break",
                 bytes(fixture.output))
        fixture.send(b"EndCLI\n")
        fixture.parent.shutdown(socket.SHUT_WR)
        deadline = time.monotonic() + 5
        while fixture.shell.poll() is None and time.monotonic() < deadline:
            fixture.pump()
        if fixture.shell.poll() is None:
            fail("ACE shell did not finish after EOF Bash",
                 bytes(fixture.output))
        if fixture.shell.returncode != 0:
            fail(f"interactive break shell exited {fixture.shell.returncode}",
                 bytes(fixture.output))
    finally:
        fixture.close(force=True)


def run_bare_bash_acceptance(repo, lnx, probe):
    fixture = LiveShell(repo, lnx, probe, command=b"Linux bash\n")
    prompt = b"BARE-PTY> "
    try:
        fixture.wait_for(lambda output: fixture.queries >= 1,
                         message="bare Bash PTY query was not answered")
        fixture.send(b"export PS1='BARE-'P'TY> '\nprintf 'BARE-'R'-READY\\n'\n")
        fixture.wait_for(
            lambda output: b"BARE-R-READY" in output and
            output.count(prompt) >= 2,
            message="bare Linux bash did not infer interactive mode",
        )
        fixture.send(b"tty\nstty size\n")
        fixture.wait_for(
            lambda output: b"/dev/pts/" in output and b"30 100" in output,
            message="bare Linux bash did not expose a usable PTY",
        )
        fixture.send(b"exit\n")
        fixture.wait_for(lambda output: output.count(RESIZE_DISABLE) == 1,
                         message="bare Linux bash did not exit cleanly")

        # Do not pre-queue the next command.  The ACE shell must remain alive
        # while its console is idle after every Linux target, including a
        # nonzero target; otherwise a leaked O_NONBLOCK flag turns EAGAIN into
        # an apparent shell EOF and races ahead of ordinary tests.
        time.sleep(0.5)
        if fixture.shell.poll() is not None:
            fail("ACE shell exited when bare Linux bash returned",
                 bytes(fixture.output))
        fixture.send(f"Linux {probe} exit 7\n".encode())
        fixture.wait_for(
            lambda output: fixture.queries >= 2 and
            output.count(RESIZE_DISABLE) == 2,
            message="nonzero Linux target did not return to ACE shell",
        )
        time.sleep(0.5)
        if fixture.shell.poll() is not None:
            fail("ACE shell exited when nonzero Linux target returned",
                 bytes(fixture.output))
        fixture.send(f"Linux {probe} report\n".encode())
        fixture.wait_for(
            lambda output: fixture.queries >= 3 and
            b"isatty 1 1 1" in output and
            output.count(RESIZE_DISABLE) == 3,
            message="ACE shell did not accept Linux after prior targets",
        )
        time.sleep(0.5)
        if fixture.shell.poll() is not None:
            fail("ACE shell exited when successful Linux target returned",
                 bytes(fixture.output))

        fixture.send(b"EndCLI\n")
        fixture.parent.shutdown(socket.SHUT_WR)
        fixture.wait_for(lambda output: fixture.shell.poll() is not None,
                         timeout=5,
                         message="explicit EndCLI did not exit ACE shell")
        if fixture.shell.returncode != 0:
            fail(f"ACE shell exited {fixture.shell.returncode} after EndCLI",
                 bytes(fixture.output))
    finally:
        fixture.close(force=True)


def run_console_shutdown_test(repo, lnx, probe):
    fixture = LiveShell(repo, lnx, probe)
    try:
        import re

        fixture.wait_for(lambda output: fixture.queries >= 1,
                         message="shutdown Bash PTY query was not answered")
        fixture.send(b"export PS1='SHUT-'D'OWN> '\nprintf 'SHUT-'D'-READY\\n'\n")
        fixture.wait_for(lambda output: b"SHUT-D-READY" in output,
                         message="shutdown Bash did not reach its prompt")
        fixture.send(
            b"printf 'BASH-PID=%s\\n' \"$BASHPID\"; "
            b"sleep 30 & printf 'SLEEP-PID=%s\\n' \"$!\"; wait\n"
        )
        fixture.wait_for(
            lambda output: re.search(rb"BASH-PID=\d+", output) and
            re.search(rb"SLEEP-PID=\d+", output),
            message="shutdown job PIDs were not reported",
        )
        bash_match = re.search(rb"BASH-PID=(\d+)", bytes(fixture.output))
        sleep_match = re.search(rb"SLEEP-PID=(\d+)", bytes(fixture.output))
        if not bash_match or not sleep_match:
            fail("shutdown test could not parse child PIDs", bytes(fixture.output))
        pids = [int(bash_match.group(1)), int(sleep_match.group(1))]
        fixture.close_console()
        deadline = time.monotonic() + 5
        while fixture.shell.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
        if fixture.shell.poll() is None:
            fixture.shell.kill()
            fixture.shell.wait(timeout=1)
            fail("closing the console left the ACE shell alive",
                 bytes(fixture.output))
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and any(
                pathlib.Path(f"/proc/{pid}").exists() for pid in pids):
            time.sleep(0.02)
        leftovers = [pid for pid in pids if pathlib.Path(f"/proc/{pid}").exists()]
        if leftovers:
            fail(f"console shutdown left target PIDs alive: {leftovers}",
                 bytes(fixture.output))
    finally:
        fixture.close(force=True)


def main():
    repo = pathlib.Path(__file__).resolve().parent.parent
    lnx = repo / "build" / "Linux"
    probe = repo / "build" / "lnx-pty-probe"
    foreground_spoof = repo / "build" / "foreground-spoof"
    if not lnx.exists() or not probe.exists() or not foreground_spoof.exists():
        fail("Linux PTY binaries are not built")
    test_isolated_supervisor(lnx, probe)
    run_interactive_shell(repo, lnx, probe)
    run_bare_bash_acceptance(repo, lnx, probe)
    run_interactive_break_tests(repo, lnx, probe, foreground_spoof)
    run_console_shutdown_test(repo, lnx, probe)
    print("Linux PTY protocol, activation, break translation, job control, and shutdown pass")


if __name__ == "__main__":
    main()
