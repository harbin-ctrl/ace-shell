# LNX interactive PTY integration: worker plan

## Instructions to every worker

This document is an implementation plan meant to be consumed one chunk at a
time by an AI worker. It is also affirmative permission to edit every file
listed for the assigned chunk in "Authorized files by chunk" below. The
worker does not need to ask again before editing one of those files. Do not
begin by editing `src/lnx.c`; first orient yourself in the project and verify
the state you inherited.

Before working on any chunk, read these base documents completely, in this
order:

1. `README.md` -- the product boundary, build/run conventions, the current
   description of LNX, and the live console behavior.
2. `HANDOFF.md` -- especially "Current-console roadmap", "Child-side console
   input and raw mode", the live-resize discussion, and the notes about shell
   process groups and console shutdown.
3. `TODO.md` -- outstanding work and the warning that Makefile recipe changes
   do not automatically invalidate existing objects.
4. `docs/FMM_CRM_SECURITY_HANDOFF.md` -- the privilege boundary. LNX is a
   Linux escape hatch, remains a user process, does not enter ACE's device
   view, and must never be routed through the privileged FMM/CRM service.
5. This document from beginning through the chunk you were assigned. Read the
   earlier chunk completion notes too; do not assume an earlier worker
   implemented the design exactly as proposed.

Then inspect, rather than overwrite, the current implementation and tests:

- `src/lnx.c`
- `src/native_command.c`
- `src/ace_shell_break.h`
- `src/native_dos.c` and `src/native_host.h`
- `src/console_channel.c` and `src/console_channel.h`
- the keyboard, resize, process-launch, and shutdown portions of
  `src/amiga_console.c`
- the size-query and resize-report portions of
  `src/console_device_bridge.c`
- `tests/break_signal_test.py`, `tests/shell_redirection_test.sh`,
  `tests/tine_console_query_test.py`, and
  `tests/tine_screen_trace_test.py`
- the LNX compile/link rules and test targets in `Makefile`

At the start of the worker run:

```sh
git status --short
git diff -- src/lnx.c src/native_command.c src/ace_shell_break.h \
  src/native_dos.c src/native_host.h Makefile tests docs
```

Treat every pre-existing modification as user work. Do not reset, discard, or
rewrite unrelated changes. Do not edit `third_party/`. Do not alter the FMM,
CRM, broker privilege protocol, device-view model, or root policy as part of
this feature.

Execute the numbered implementation chunks in order, one chunk at a time.
Finish and verify the current chunk, report its result, and stop at that
boundary before beginning the next chunk. A chunk is complete only when its
focused tests pass and the ordinary noninteractive LNX path still passes.
Record what was changed, the exact test commands, and any design deviation in
the progress ledger at the end of this file. Do not mark later chunks complete
because scaffolding for them happened to be added.

## Worker autonomy

The worker has broad autonomy inside the assigned chunk. The user expects to
intervene only when genuinely needed. Do not pause for routine implementation
choices, naming, helper placement, buffer sizes, test-fixture details, error
handling, or other minor decisions that can be resolved from current source,
the base documents, established project style, and observed tests. Inspect the
relevant code, choose the smallest sound design, implement it, and continue.

The authorized-file table is sufficient authority for normal edits, new
helpers, tests, build-rule changes, documentation updates, and corrective work
needed to complete the chunk. The worker may revise details proposed by this
plan when source evidence proves another implementation is safer or simpler,
provided the externally visible contracts and security boundaries remain
intact. Record material deviations in the progress ledger; do not turn every
small judgment into a request for approval.

Continue autonomously through the entire chunk and its delivery sequence. In
particular, a compile error, failed assertion, race, flaky test, stale object,
or first unsuccessful design is a reason to diagnose and fix the problem, not
a reason to hand the work back. Use multiple evidence-driven repair rounds,
rerun the affected and common regression tests, and keep going while concrete
progress is possible.

Stop before the chunk boundary only when one of these conditions is real:

- repeated diagnosis-and-fix rounds still leave the same substantive failure
  and no new evidence-backed approach remains;
- completion requires authority outside this document, such as changing the
  privilege model, editing an unlisted security component, or obtaining
  credentials/access that are unavailable;
- current source or tests expose a material product decision that conflicts
  with the settled requirements here and either choice would change user-
  visible behavior beyond this feature;
- pushing, installing, or stopping privileged ACE services is impossible
  after exhausting the documented in-scope mechanisms;
- continuing would risk unrelated user work or non-ACE system state.

When stopping early, report the exact blocker, evidence, attempts already made,
current repository/process state, and the smallest decision or external change
needed to resume. Difficulty, uncertainty, a long chunk, or a test that has
failed only once are not blockers.

Every chunk ends with this mandatory delivery sequence, in this order:

1. **Build.** Build the changed production targets and test helpers from the
   current tree. Account for the Makefile invalidation caveat below.
2. **Commit.** Review `git diff`, `git diff --check`, and `git status`, then
   commit only the completed chunk and its plan-ledger update. Preserve all
   unrelated user work.
3. **Test thoroughly.** Run the chunk's focused tests and every regression
   target named by that chunk. A failure is not left for the next chunk: fix
   it, rebuild, amend or add a corrective commit as appropriate, and rerun the
   complete chunk test set. Keep working through failures rather than stopping
   after the first attempt. If several evidence-driven repair rounds still do
   not produce a passing chunk, stop, leave the failure unpushed, and report
   the exact blocker, attempts, and output. The commit that is eventually
   pushed must be the tested state.
4. **Push to `main`.** Push the tested chunk commit directly to the configured
   upstream `main` branch. Do not create a worker branch and do not push a
   known failing state.
5. **Install everything.** Run the repository's full documented `make install`
   path after every chunk, including Vim, Regina, and LhA. The installed ACE
   set must be the same coherent companion set as the tested source. A failure
   in an optional component is an install failure to repair or report, not a
   reason to substitute a partial core-only install.
6. **Kill every previous ACE run.** Unconditionally terminate all existing ACE
   Shell/console processes, all ACE brokers from every build or installation,
   and all of their mediators/FMM and mount workers. This includes ACE runs
   from other checkouts and installed copies. Use the repository lifecycle
   shutdown where it applies, then terminate every remaining process with an
   ACE executable identity; stop privileged mediator children through the
   available privileged shutdown/kill path. Do not leave a prior run alive
   merely because it was not started by the current worker. Leave no ACE
   shell, console, broker, mediator, FMM, mount worker, or test broker running.

The worker stops after reporting that sequence. The next chunk starts from the
installed, pushed, quiescent result.

The minimum focused regression set after every chunk is:

```sh
make test-shell-redirection
python3 tests/break_signal_test.py
make test-native-input test-native-console-handle
make test-console-channel test-console-device-bridge
make test-shell-return-code
```

Run `make test-lnx-pty` as soon as Chunk 0 creates that target. Chunk 0 uses a
passing characterization assertion that the current socket-backed LNX target
is not a tty; it does not add a red or skipped expected-failure test. Later
chunks replace that assertion as the behavior changes. Add each chunk's own
tests to this common set. The broad project suite is reserved for Chunk 5;
focused does not mean cursory, and every affected boundary is regression-tested
after every chunk.

When a Makefile recipe or link dependency changes, remember the known build
limitation from `TODO.md`: the Makefile is not itself a prerequisite of the
objects it builds. Remove only the specific stale object or binary being
tested, or use an appropriate narrow clean target. Do not use a destructive
repository-wide cleanup to force a rebuild.

## Documentation authority and consistency

The root `README.md`, `HANDOFF.md`, and `TODO.md` describe ACE as a whole. The
documents under `docs/` are component plans and detailed handoffs. This plan
has been checked against all current Markdown documents under `docs/`:

- `docs/FMM_CRM_SECURITY_HANDOFF.md` is authoritative for LNX's security
  boundary. LNX remains unprivileged, never becomes an FMM/CRM operation, and
  gives Linux programs the ordinary Linux view rather than ACE's device view.
- `docs/regina-arexx-plan.md` records an important console transport fact: an
  abstract ACE console handle does not necessarily have one descriptor that
  can be handed to another process. PTY activation therefore requires usable
  inherited standard descriptors as well as matching endpoint identity. This
  work does not broaden ARexx stream descriptor passing or general `CON:`
  handle export.
- `docs/regina-amiga-port.md` is historical Regina orientation and introduces
  no competing LNX or terminal contract.
- `docs/clipboard-system.md` owns clipboard protocol and client migration.
  LNX PTY integration must not change console copy/paste or clipboard-device
  behavior.

When those documents change during this fast-moving project, current source
and tests are evidence, but a worker must call out a contradiction rather than
silently choosing one document over another.

## Goal

Make a Linux program launched from a live ACE console through LNX see a real
Unix terminal. The primary user-visible acceptance case is bare `LNX bash`;
Bash should infer interactive mode from the PTY without needing `-i`:

```text
AMIGA> LNX bash
bash$ echo hello
hello
bash$ exit
AMIGA>
```

Automated tests may use `bash --noprofile --norc -i` to remove user startup
files and make the prompt deterministic, but the final manual test must include
bare `LNX bash`.

The completed feature must also support terminal process groups, Ctrl-C,
Ctrl-D, Ctrl-Z, live window-size changes, and clean teardown. It must preserve
the current direct-exec behavior for scripts, pipes, redirection, and direct
invocation from an ordinary Linux terminal.

This is not a request to make ACE's Unix socket pretend to be a tty. LNX will
be a small PTY supervisor only when the ACE command runner explicitly tells it
that both selected streams are the live ACE console. The Linux program will
own the PTY slave; LNX will retain the PTY master and bridge it to ACE's
existing console byte channel.

## Existing behavior and the actual gap

The live ACE console is deliberately a full-duplex Unix stream socket:

```text
GTK keyboard -> ACE console channel -> shell/command stdin
GTK renderer <- ACE console channel <- shell/command stdout/stderr
```

`RunCommand()` forks an ACE command, duplicates the selected AmigaDOS
`Input()` and `Output()` streams onto file descriptors 0 and 1, and waits for
the command. LNX then searches the Linux `PATH` itself and replaces its process
with `execv()`. This is correct for `LNX ls`, but the inherited descriptor is
a socket rather than a terminal, so `isatty()` is false and Bash disables its
normal interactive and job-control behavior.

The console stream already carries more than plain text, but all of it is
public Amiga console protocol:

- Output accepts both C1 CSI (`0x9b`) and `ESC [` control sequences through
  AROS's real console parser.
- GUI navigation keys use Amiga C1 CSI sequences such as `0x9b A`. Those are
  not in general the key sequences expected by the generic Linux terminal
  identity the PTY target will receive.
- `CSI 0 q` asks the console for its bounds. The response is
  `CSI 1;1;ROWS;COLS r`.
- `CSI 12{` enables `IECLASS_SIZEWINDOW` reports and `CSI 12}` disables them.
  A report is `CSI 12;...|`; after receiving it, the program asks for the new
  bounds with `CSI 0 q`.
- Ctrl-C, Ctrl-D, Ctrl-E, and Ctrl-F are not sent as bytes. The GTK console
  turns them into ACE break signals delivered first to the shell and then to
  its foreground command. Ctrl-Z and the other ordinary control characters
  already remain bytes.

Those facts determine the implementation. PTY mode must not retain
`TERM=amiga`. Do not add a second GUI control socket or bypass the existing
console-device query/resize mechanism. On the target Debian 13 Trixie,
rpd-labwc system, the observed terminal default is `TERM=xterm-256color`; PTY
targets use that fixed Debian-facing contract as described below.

## Target architecture

Only a live, unredirected ACE-console invocation takes the new branch:

```text
ace-console
    | existing Amiga console byte stream
ace-user-shell
    | RunCommand: marks this LNX invocation as host-terminal mode
LNX supervisor
    | poll loop                         | public ACE console control
    |                                  | - query bounds
    | ACE input -> translate -> PTY     | - consume resize reports
    | ACE output <- PTY output          | - reset resize events on exit
    |
PTY master
    |
PTY slave: controlling terminal, fd 0/1/2
    |
bash / ssh / top / other Linux program
```

All other invocations retain the existing path:

```text
LNX command < file > file  -> direct exec with selected descriptors
piped/scripted ACE shell   -> direct exec
./build/LNX command        -> direct exec; the host terminal is already real
```

The PTY supervisor remains an ordinary user process. It does not talk to the
FMM/CRM service and does not gain ACE filesystem translation or device-view
access. The target program sees the normal Linux filesystem view and user ID,
as LNX does today.

## Non-negotiable design decisions

1. **Opt in from `RunCommand()`, not by guessing in LNX.** Add a private
   environment marker, named in one shared header, only when the command is
   the ACE LNX command, the shell is GUI-interactive, and selected input and
   output refer to the same console channel. LNX removes this marker before
   executing the Linux target. Environment inheritance alone must not let a
   nested or redirected LNX accidentally allocate another PTY.

   Every LNX command with the live, shared, unredirected ACE console is opted
   in automatically. There is no command-name allowlist for interactive Linux
   targets and no user-facing PTY switch. Consequently `LNX ls` sees a tty and
   may use terminal-oriented formatting; that is intended.

2. **Preserve direct exec when PTY mode is not selected.** The current PATH
   search and `execv()` semantics are useful and simple. Do not turn every LNX
   invocation into a supervisor. This preserves redirection bytes, exit-by-
   signal behavior, and direct use from a host terminal.

   Any selected input or output redirection disables PTY mode entirely,
   including mixed cases such as console input with output redirected to a
   file. Do not build a split-terminal mode for this feature.

3. **Give the Linux target the platform's generic terminal-emulator
   identity.** PTY mode must not depend on an `amiga` terminfo entry. This
   Debian 13 Trixie/rpd-labwc environment reports `TERM=xterm-256color`, and
   Raspberry Pi OS Trixie is the same intended lower system for this ACE
   hybrid. Set the PTY target's TERM explicitly to `xterm-256color`. This is a
   platform contract, not a runtime probe and not a dependency on the launcher's
   TERM (desktop-launched sessions commonly have no TERM before ACE creates its
   console). Direct LNX invocation outside an ACE PTY session preserves its
   caller environment.

   TERM is a claim about the terminal ACE presents, not merely a string that
   makes applications become interactive. Audit representative output from
   the Trixie `xterm-256color` entry against AROS's real console parser. Where
   the renderer cannot represent an advertised capability, add a bounded LNX
   output adaptation or an ACE-console implementation in an authorized file
   and test it; do not silently advertise a capability that produces corrupt
   output. Do not forward `COLORTERM=truecolor` unless truecolor support is
   actually established. The input bridge translates ACE's Amiga key
   sequences into the concrete sequences described by the
   `xterm-256color` contract.

   Apply this Debian-facing TERM to every Linux target reached through ACE's
   official LNX command, including a direct/redirection invocation for which
   PTY allocation is disabled. Redirection changes descriptors, not the lower
   operating-system identity. A standalone `./build/LNX` invoked outside the
   ACE command runner preserves the caller's TERM. Use an ACE-private marker
   from `RunCommand()` to distinguish those cases; do not infer them from an
   arbitrary inherited command name.

4. **Use the public in-band geometry protocol.** Initial sizing and dynamic
   resizing use `CSI 0 q`, `CSI 12{`, resize reports, and `CSI 12}`. Do not add
   ACE-only row/column environment variables or a new socket merely for LNX.

5. **Keep protocol parsing bounded and lossless.** A malformed or incomplete
   control sequence must never make LNX consume input forever. User bytes that
   are not a recognized ACE bounds response or resize report must reach the
   PTY in order. Reads and writes may split anywhere.

6. **Do not corrupt UTF-8.** Byte `0x9b` can occur as a UTF-8 continuation
   byte. Treat it as C1 CSI only at a character/sequence boundary; maintain
   enough UTF-8 state to pass a continuation byte unchanged.

7. **Translate breaks at the PTY boundary.** In host-terminal mode the four
   intercepted ACE breaks become terminal input bytes: Ctrl-C `0x03`, Ctrl-D
   `0x04`, Ctrl-E `0x05`, Ctrl-F `0x06`. Write them to the PTY master so the
   terminal line discipline and current PTY foreground process group decide
   what they mean. Do not signal only the original Bash PID.

   This remains true when LNX is invoked by an ACE script: Ctrl-D exits or
   sends EOF to the Linux terminal program, then the ACE script continues.
   Ctrl-D retains its script-break meaning for every ordinary ACE foreground
   command.

8. **The supervisor owns cleanup.** The PTY child starts a new session, so it
   no longer belongs to the shell's original process group. When the ACE
   socket closes, the window sends HUP, LNX is terminated, or the target exits,
   the supervisor must close the PTY and terminate/reap the target session as
   appropriate. No process may survive with a dead ACE console.

9. **No unbounded buffering.** Use fixed-size bounded queues or buffers with
   backpressure. Stop polling a source for `POLLIN` while its destination
   queue is full. Do not drop terminal output under load.

10. **One owner writes each direction.** In PTY mode, the LNX supervisor is
    the only process that reads ACE input, reads the PTY master, and emits
    console queries/event setup. The target owns only the PTY slave. This
    prevents size replies from racing a second reader.

## Chunk 0: baseline, contract tests, and scaffolding

### Purpose

Establish reproducible evidence for the current failure before changing
process behavior. Build a fake ACE console harness that later chunks can
reuse without requiring GTK automation.

### Work

1. Build the existing relevant targets and run the existing tests:

   ```sh
   make build/LNX build/ace-user-shell build/ace-broker
   make test-shell-redirection
   python3 tests/break_signal_test.py
   make test-native-input test-console-channel test-console-device-bridge
   ```

2. Add `tests/lnx_pty_test.py` and a focused `test-lnx-pty` Makefile target.
   The initial test should be capable of creating a Unix `socketpair()`,
   giving one endpoint to LNX as descriptors 0/1/2, and acting as the ACE
   console on the other endpoint. Keep timeouts short and deterministic; every
   failure must kill and reap children and print the captured byte stream.

3. Record the gap with a passing characterization test, not an expected
   failure, skip, or red target. The test must demonstrate:

   - plain LNX with a socket on fd 0/1/2 gives its target `isatty(0) == 0`;
   - direct Linux-terminal invocation still uses its real tty;
   - redirected `LNX cat` preserves exact bytes and status;
   - a nonexistent command still returns `RETURN_FAIL` with the current
     diagnostic.

4. Prefer a small C test helper built into `build/` over scraping shell text
   for low-level assertions. The helper should be able to report `isatty()`
   for all three descriptors, `tcgetsid()`, `tcgetpgrp()`, `TIOCGWINSZ`, bytes
   read in raw mode, received `SIGWINCH`, and its own exit mode. Keep it a test
   program; do not add diagnostic behavior to production LNX.

### Acceptance

- Existing focused tests pass unchanged.
- `test-lnx-pty` reliably proves the pre-feature descriptor is not a tty.
- The harness has bounded waits and leaves no process or broker behind.
- No production behavior changes in this chunk.

### Handoff note

Record the exact fake-console framing and helper output in the progress
ledger. Later workers should extend this harness rather than create a second
one.

## Chunk 1: PTY allocation, child session, and basic relay

### Purpose

Implement an opt-in PTY supervisor inside LNX while leaving its ordinary path
untouched. At the end of this chunk, an isolated fake-console test can launch
an interactive target with real tty descriptors, exchange lines, and recover
the target status. Amiga key translation and live resize come later.

### Work

1. Define the private opt-in marker in an ACE-owned shared header. Give it a
   narrow name such as `ACE_LNX_PTY`, document that only `RunCommand()` sets
   it, and ensure LNX unsets it before target `execv()`.

2. Split `src/lnx.c` into clear responsibilities without changing the direct
   branch:

   - Linux PATH search and target exec;
   - PTY allocation;
   - PTY child setup;
   - supervisor relay;
   - exit-status propagation and cleanup.

   In the PTY child, set `TERM=xterm-256color` before executing the target.
   Chunk 3 extends the same target environment to official ACE LNX direct mode
   while preserving standalone LNX. Do not probe the host for an Amiga
   terminfo entry and do not advertise `COLORTERM=truecolor` without proven
   support.

3. Prefer the explicit POSIX sequence so the binary does not acquire an
   unnecessary `libutil` dependency:

   - `posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC)`;
   - `grantpt()` and `unlockpt()`;
   - obtain the slave path with a bounded API;
   - `fork()`;
   - child: `setsid()`, open the slave, `ioctl(TIOCSCTTY)`, duplicate it onto
     fd 0/1/2, close unrelated descriptors, restore signal dispositions and
     mask, remove private ACE supervisor variables, and run the existing PATH
     search/exec routine;
   - parent: close the slave side and retain the master.

4. Initialize a sane `struct termios`. Starting from the PTY driver's defaults
   is preferable to copying termios from the ACE socket, because the socket
   has none. Verify canonical input, echo, `ISIG`, and ordinary output newline
   processing. Do not hand-construct a novel terminal mode if the PTY defaults
   already provide it.

5. Implement a `poll()`-based full-duplex relay with separate bounded pending
   buffers for ACE-to-PTY and PTY-to-ACE. Correctly handle partial writes,
   `EINTR`, `EAGAIN`, `POLLHUP`, and Linux's conventional `EIO` from a PTY
   master after the slave closes. Continue draining target output after ACE
   input reaches EOF when doing so is meaningful.

6. Reap the target with `waitpid()`. If it exits normally, return its exact
   status. If it dies from a signal, clean up and then reproduce signal
   termination in the LNX supervisor by restoring the default disposition,
   unblocking the signal, and raising it. That preserves what the parent
   `RunCommand()` observed when LNX directly execed the target.

7. Keep the opt-in test-only for this chunk. Do not yet modify
   `RunCommand()` to set the marker.

### Tests

Extend `test-lnx-pty` to set the private marker explicitly and verify:

- target descriptors 0, 1, and 2 all satisfy `isatty()`;
- the PTY target sees `TERM=xterm-256color`, while direct mode preserves a sentinel
  TERM supplied by the caller;
- the target is a session/process-group leader with a controlling terminal;
- a complete input line crosses ACE socket -> LNX -> PTY and echo/output
  crosses back;
- output larger than one relay buffer is byte-complete;
- normal statuses including 0 and a nonzero value survive;
- signal termination remains visible as signal termination;
- direct mode, redirection, and not-found behavior remain unchanged.

### Acceptance

The isolated supervisor is correct under ordinary I/O and teardown, but no
claim is yet made that Bash works from the real ACE shell.

## Chunk 2: Amiga input adaptation and console geometry

### Purpose

Bridge the semantic differences between ACE's Amiga console protocol and the
Linux PTY. At the end of this chunk, navigation keys are Linux-compatible and
the target receives correct initial and live PTY dimensions without adding a
new transport.

### Work: initial geometry

1. Before forking the target, emit the public Amiga bounds query
   `0x9b "0 q"` to the inherited ACE output descriptor.

2. Read and parse a bounded response accepting either C1 CSI or `ESC [`:

   ```text
   CSI 1;1;ROWS;COLS r
   ```

   Use `poll()` with a finite timeout. A missing or malformed response is not
   fatal; use a documented 80x24 fallback. Preserve any unrelated typeahead
   bytes read while looking for the response and deliver them to the target
   after the PTY is ready.

3. Apply the resulting dimensions with `TIOCSWINSZ` before the target starts
   doing terminal layout. The focused test must show the dimensions through
   the target's `TIOCGWINSZ`, not merely through LNX's local state.

### Work: input sequence adaptation

4. Add a bounded streaming parser on the ACE-to-PTY path. It must survive a
   control sequence split across arbitrary reads. Translate the finite set of
   sequences emitted by `amiga_console.c` into the fixed
   `TERM=xterm-256color` input
   contract. This needs an explicit, reviewed table rather than a blanket C1
   prefix replacement. At minimum cover arrows, shifted arrows where the
   Linux contract has an equivalent, Insert, Delete, Home, End, Page Up,
   Page Down, Backspace, and F1-F10. Preserve ordinary text and existing input
   that is already valid for the selected target terminal.

   Pay particular attention to Backspace and Delete: ACE emits distinct `\b`
   and `0x7f` bytes, while the Linux terminal contract uses DEL for Backspace
   and a CSI sequence for Delete. Translate them without collapsing the two
   keys.

5. Make the parser UTF-8 aware. If `0x9b` arrives where a UTF-8 continuation
   byte is expected, pass it unchanged. Test with a valid multibyte character
   whose encoding contains `0x9b`; do not settle for ASCII-only tests.

6. Bound the longest candidate sequence. If the bound is exceeded or the
   sequence is invalid, flush it as user input instead of waiting forever or
   dropping it.

### Work: advertised output capabilities

7. Exercise representative output generated from Trixie's
   `xterm-256color` terminfo: clearing, cursor movement, ordinary attributes,
   8/16-color SGR, indexed-color SGR, and the screen-mode sequences emitted by
   at least one available curses application. Feed those bytes through ACE's
   real console parser test seam.

8. If an advertised sequence is unsupported or rendered incorrectly, adapt
   it in the PTY-to-ACE direction inside LNX using a bounded streaming parser,
   or implement the missing host-console behavior in an authorized ACE console
   file where that is the cleaner project boundary. Preserve plain output
   byte-for-byte and do not modify imported AROS source. Indexed colors may be
   deterministically reduced to ACE's palette if full indexed-color rendering
   is outside this feature, but the mapping and tests must be explicit.

9. Do not pass through or synthesize `COLORTERM=truecolor` unless truecolor
   output is implemented and tested. TERM alone is the contract for this
   chunk.

### Work: live resize

10. After initial geometry is known, enable the existing Amiga raw resize event
   with `CSI 12{`. On every exit path where the ACE output stream remains
   writable, restore it with `CSI 12}`.

11. Recognize and consume only `IECLASS_SIZEWINDOW` input reports ending in
   `|`. Do not send those reports to the Linux program. In response, issue a
   new `CSI 0 q` bounds query, recognize and consume its reply, apply
   `TIOCSWINSZ` to the PTY, and allow the kernel to deliver `SIGWINCH` to the
   PTY's current foreground process group.

12. Preserve ordering around resize traffic. User keys arriving before or
   after a report must reach the target exactly once. A query response may be
   split across reads or adjacent to user bytes. Never assume one `read()` is
   one sequence.

13. Coalesce resize work only if needed, and only after correctness. ACE
    already emits once per character-grid change, so an additional timer is
    probably unnecessary.

### Tests

Extend the fake console to answer queries and inject reports. Verify:

- initial `stty size`/`TIOCGWINSZ` matches the fake response;
- every mapped ACE navigation/editing/function key arrives as the sequence
  expected by `TERM=xterm-256color`;
- existing valid Linux-terminal input is not doubled or corrupted;
- fragmented sequences work at every split point;
- UTF-8 containing byte `0x9b` is unchanged;
- resize setup and reset sequences are emitted exactly once;
- a resize report causes one new bounds query;
- the target sees new dimensions and `SIGWINCH`;
- neither resize report nor bounds reply leaks into target input;
- malformed sequences do not hang or lose following input.
- the target does not receive `COLORTERM=truecolor` without proven support;
- representative `xterm-256color` output is either rendered correctly by the
  existing console path or adapted predictably before it reaches that path;
- output adaptation, if required, survives arbitrary read boundaries and does
  not alter ordinary UTF-8 text.

### Acceptance

The isolated LNX supervisor now presents a coherent `TERM=xterm-256color`
terminal over ACE's existing Amiga console stream. Direct mode outside the ACE
PTY branch still preserves the caller's TERM.

## Chunk 3: shell activation and redirection boundary

### Purpose

Select PTY mode only for the real case that needs it. This chunk connects the
working supervisor to `RunCommand()` and delivers the first end-to-end
`LNX bash -i` session.

### Work

1. Add an explicit native-DOS helper that answers whether two BPTR handles
   refer to the same console channel/endpoint. Implement it where
   `native_channel_for_handle()` already understands current aliases and
   parameterized `CON:` handles; expose only the narrow declaration needed by
   `native_command.c`. Do not infer console identity merely from
   `ACE_CONSOLE_INTERACTIVE`, and do not treat arbitrary pipes or sockets as
   ACE consoles. Endpoint identity alone is insufficient: confirm that the
   selected streams have usable descriptors that `RunCommand()` can actually
   duplicate across `execv()`. An abstract console handle may deliberately
   have no single exportable descriptor, as documented in
   `docs/regina-arexx-plan.md`; leave that case out of PTY mode rather than
   broadening console-handle transport in this feature.

2. In `RunCommand()`, calculate host-terminal mode before `fork()` using all
   of these conditions:

   - `ACE_CONSOLE_INTERACTIVE=1`;
   - the loaded command is ACE's LNX command;
   - selected `Input()` and `Output()` share the same console endpoint;
   - both selected streams expose the inherited descriptors required by the
     LNX bridge;
   - the shell is not executing with redirected input or output.

   Keep the test for the LNX command narrow. Use the loaded segment's resolved
   command identity/path, not the user's untrusted argument text. Document why
   the basename or companion-path check is sufficient in ACE's command drawer
   model.

3. In the child branch, set the private PTY marker only for that invocation;
   explicitly unset it otherwise so a stale inherited value cannot force PTY
   mode. Do this before `execv(segment->path, argv)`.

   Separately mark every invocation of ACE's official LNX command so LNX sets
   the Linux target's TERM to `xterm-256color` even when redirection selects
   direct mode. LNX must remove both private markers before the target exec.
   A standalone LNX process without the command-runner marker preserves its
   caller's TERM.

4. In the parent branch, remember whether the foreground child is a host
   terminal command. Chunk 4 will use the distinction for Ctrl-D. Do not alter
   ordinary ACE break behavior yet.

5. Update README's LNX section only enough to describe the now-supported
   interactive console case and the direct/redirection split. Leave detailed
   signal/job-control claims until their tests pass in Chunk 4.

### End-to-end test

Extend `tests/lnx_pty_test.py` with a private broker and temporary `SYS:C`
containing the required `LNX`, `EndCLI`, and any probe commands. Start
`ace-user-shell` with one end of a Unix socketpair as fd 0/1/2 and
`ACE_CONSOLE_INTERACTIVE=1`; the test owns the other end and implements the
size-query responses.

Run:

```text
LNX bash --noprofile --norc -i
echo LNX-INTERACTIVE-OK
exit
EndCLI
```

Use a deterministic `PS1` and bounded reads. Assert that Bash does not print
the "no job control" diagnostics, the marker is printed, Bash exits, the ACE
prompt returns, and the shell exits cleanly after `EndCLI`.

Also prove mode selection:

- `LNX cat < input > output` remains direct and byte-exact;
- a piped/scripted `ace-user-shell` does not activate PTY mode;
- mixed or separate console handles do not silently merge;
- a normal one-shot LNX command in the live console may observe tty output
  formatting, which is correct because it is now genuinely attached to a
  terminal;
- direct `./build/LNX` from a host terminal remains direct exec.

### Acceptance

Interactive Bash starts, runs commands, exits, and returns to the ACE prompt.
Ordinary scripts and redirections remain unchanged.

## Chunk 4: break translation, job control, and session lifetime

### Purpose

Complete the behavior that distinguishes a real terminal from a merely
bidirectional stream: terminal control characters affect the PTY foreground
job, jobs can stop/resume, and closing ACE tears down the new session.

### Work: shell break mode

1. Replace the foreground PID-only API in `ace_shell_break.h` with a PID plus
   an explicit foreground kind, for example ordinary ACE command versus LNX
   host terminal. Keep the type private to the shell/native command seam.

2. Preserve current behavior for ordinary commands exactly:

   - Ctrl-C/E/F route through the broker/native task bridge;
   - Ctrl-D remains a CLI script-break request, observed by `Shell.c` after
     the current command boundary.

3. For a host-terminal foreground LNX command, route Ctrl-D through the same
   broker/host-signal path as C/E/F instead of raising the shell's
   `SIGBREAKF_CTRL_D`. This distinction is required so `Ctrl-D` can mean EOF
   to Bash without breaking the ACE script containing the LNX invocation.

4. Keep `tests/break_signal_test.py` passing. Add a case proving Ctrl-D during
   an ordinary ACE script still stops the script at the command boundary.
   Existing behavior must not regress merely because LNX has a terminal mode.

### Work: LNX signal intake

5. In PTY mode, install async-signal-safe handlers for `SIGUSR1`, `SIGUSR2`,
   `SIGRTMIN`, and `SIGRTMIN+1`. Handlers should write a one-byte event to a
   nonblocking self-pipe; the relay loop performs the PTY write. Do not mutate
   complex parser/queue state in signal context.

6. Translate events into `0x03`, `0x04`, `0x05`, and `0x06` written to the
   PTY master. This deliberately asks the PTY line discipline to signal its
   current foreground process group. Do not `kill()` only the initial target
   PID: after Bash runs `fg`, that PID may not own the foreground terminal.

7. Ctrl-Z already arrives as input byte `0x1a`; verify it remains untouched
   and that the PTY line discipline stops the current job. The Bash session
   must be able to run `jobs`, `fg`, and receive a later Ctrl-C.

### Work: shutdown

8. Handle inherited-console EOF/HUP, `SIGHUP`, `SIGTERM`, supervisor failure,
   and ordinary target exit deliberately. The target is in a new session, so
   ACE's existing `kill(-shell_pid, SIGHUP)` no longer reaches it directly.
   LNX must close the PTY master (causing the controlling-terminal hangup),
   forward HUP/termination to the known target/foreground process group where
   needed, and reap its direct child. Do not assume `kill(-target_pid, ...)`
   reaches every job after an interactive shell has created additional process
   groups; closing the controlling PTY and allowing the shell to clean up its
   jobs are part of the shutdown contract.

9. Use bounded escalation only when needed: graceful HUP, then TERM, then
   KILL after short monotonic deadlines. Do not block forever waiting for a
   target that ignores HUP. Conversely, do not kill background jobs merely
   because the interactive shell exited normally if the normal controlling-
   terminal hangup semantics have already handled them; inspect and test the
   actual process tree.

10. Reset the resize event before the ACE output descriptor is closed when
    possible. Every error path must close the self-pipe and PTY master and
    reap its direct child.

### Tests

Add deterministic end-to-end cases:

- Run `sleep 30` in interactive Bash, signal the ACE shell with `SIGUSR1`,
  and prove `sleep` is interrupted and Bash returns to its prompt.
- At an empty Bash prompt, signal the ACE shell with `SIGUSR2`; prove Bash
  receives terminal EOF, exits, and the ACE prompt returns rather than
  reporting an ACE script break.
- Run `sleep 30`, send byte `0x1a`, verify Bash reports a stopped job, run
  `jobs`, resume it with `fg`, then interrupt it with Ctrl-C.
- Verify Ctrl-E and Ctrl-F reach an interactive raw-byte probe as bytes rather
  than ACE signals.
- Close the fake console while Bash and a child are active. Assert all known
  PIDs disappear within a bounded timeout and no process spins after losing
  the console.
- Re-run all existing break tests to prove ordinary ACE semantics remain.

### Acceptance

Interactive Bash has working foreground job control and terminal control
characters, while ordinary ACE commands retain Amiga break semantics. Console
shutdown leaves no LNX target session behind.

## Chunk 5: robustness, documentation, and full regression pass

### Purpose

Turn the working feature into maintainable project state: stress the relay,
close edge cases, document the exact boundary, and run the broad suite.

### Work

1. Audit every descriptor and process path in LNX:

   - allocation failure before fork;
   - child failure before and during `execv()`;
   - ACE input closes while output is pending;
   - PTY closes while ACE output is backpressured;
   - target exits with unread PTY output;
   - signal arrives while queues are full;
   - resize query times out;
   - malformed/incomplete sequence at EOF;
   - target emits enough output to wrap buffer indices repeatedly.

2. Run a backpressure/stress test with output substantially larger than socket
   and PTY buffers while delaying the fake console reader. Compare the complete
   payload; checking only process exit is insufficient.

3. Confirm no private ACE bytes can leak to the target and no target bytes are
   mistaken for console input control. The parser applies only on
   ACE-input-to-PTY traffic; Linux output flows directly to ACE output.

4. Update documentation:

   - `README.md`: user behavior and examples (`LNX bash`, direct execution,
     redirection, Linux filesystem/user boundary).
   - `HANDOFF.md`: process tree, PTY ownership, Amiga-to-Linux key translation,
     in-band geometry/resize handling, break translation, and shutdown.
   - `TODO.md`: remove any LNX PTY item if one was added, or add only genuinely
     deferred limitations discovered during implementation.
   - `docs/FMM_CRM_SECURITY_HANDOFF.md`: change the sentence saying PTY
     integration is future work, without weakening the statement that LNX is
     unprivileged and outside device view.

5. Keep comments focused on why the boundaries exist. In particular, explain
   why Ctrl-D needs a foreground kind, why C1 conversion must be UTF-8 aware,
   and why size reports are consumed rather than relayed.

6. Run formatting/compiler warnings at the project's existing strictness. Do
   not introduce a new formatter or reformat unrelated source.

### Required focused regression

```sh
make test-lnx-pty
make test-shell-redirection
make break-signal-test
make test-native-input test-native-console-handle
make test-console-channel test-console-device-bridge
make test-shell-return-code
```

Then run the broad project test command documented by the current Makefile or
base docs. If the entire optional Vim/Regina/LhA build is prohibitively slow,
report exactly what was and was not run; do not describe an unrun suite as
passing.

### Final manual acceptance

From a live uninstalled build started according to `README.md`:

```text
AMIGA> LNX bash
bash$ tty
/dev/pts/N
bash$ stty size
<current rows> <current columns>
bash$ sleep 30
^C
bash$ sleep 30
^Z
[1]+  Stopped                 sleep 30
bash$ fg
sleep 30
^C
bash$ exit
AMIGA>
```

Resize the ACE window while Bash is running and verify `stty size` changes.
Launch at least one curses-style Linux program if available and verify basic
redraw and exit, while recognizing that ACE intentionally implements an Amiga
terminal rather than every xterm extension.

### Acceptance

- Focused and broad tests pass or every unrelated/pre-existing failure is
  clearly identified with evidence.
- Documentation describes implemented behavior, not aspiration.
- No root component, privilege protocol, or device-view behavior changed.
- `git diff --check` passes.
- `git status --short` contains only intended files plus preserved user work.

## Authorized files by chunk

This table is both a routing aid and explicit permission to edit every listed
file for the assigned chunk. The worker should make every in-scope change
needed in those files without asking for file-by-file approval. It does not
authorize unrelated changes merely because a file is listed; edits must still
serve the current chunk and preserve unrelated user work.

| Chunk | Authorized files |
|---|---|
| 0 | `tests/lnx_pty_test.py`, `tests/lnx_pty_probe.c`, `Makefile`, `docs/lnx-pty-worker-plan.md` |
| 1 | `src/lnx.c`, `src/lnx_pty.h`, `tests/lnx_pty_probe.c`, `tests/lnx_pty_test.py`, `Makefile`, `docs/lnx-pty-worker-plan.md` |
| 2 | `src/lnx.c`, `src/amiga_console.c`, `src/console_device_bridge.c`, `src/aros_graphics_runtime.c`, `tests/lnx_pty_probe.c`, `tests/lnx_pty_test.py`, `tests/console_device_bridge_test.c`, `tests/graphics_test.c`, `Makefile`, `docs/lnx-pty-worker-plan.md` |
| 3 | `src/native_command.c`, `src/native_dos.c`, `src/native_host.h`, `src/lnx_pty.h`, `tests/lnx_pty_test.py`, `tests/shell_redirection_test.sh`, `Makefile`, `README.md`, `docs/lnx-pty-worker-plan.md` |
| 4 | `src/ace_shell_break.h`, `src/native_command.c`, `src/lnx.c`, `tests/break_signal_test.py`, `tests/lnx_pty_test.py`, `Makefile`, `docs/lnx-pty-worker-plan.md` |
| 5 | `src/lnx.c`, `src/native_command.c`, `src/native_dos.c`, `src/native_host.h`, `src/ace_shell_break.h`, `src/lnx_pty.h`, `tests/lnx_pty_probe.c`, `tests/lnx_pty_test.py`, `tests/break_signal_test.py`, `tests/shell_redirection_test.sh`, `tests/console_device_bridge_test.c`, `Makefile`, `README.md`, `HANDOFF.md`, `TODO.md`, `docs/FMM_CRM_SECURITY_HANDOFF.md`, `docs/lnx-pty-worker-plan.md` |

## Deferred ideas that are not part of this plan

- Running LNX targets through the privileged FMM/CRM service.
- Giving Linux targets ACE's translated device view.
- Replacing ACE's Amiga console protocol with xterm.
- Making every ACE command run under a PTY.
- Adding shell syntax, automatic Linux-command fallback, or invoking
  `/bin/sh -c` inside LNX.
- Supporting PTY mode when input and output are deliberately redirected to
  different destinations. Direct descriptor mode remains the correct behavior
  there.
- Building a general terminal multiplexer. LNX has one supervised target
  session and one existing ACE console channel.

## Progress ledger

Workers: append concise entries here. Include the chunk, files changed, tests
run, and deviations or follow-up risks. Calendar dates are intentionally not
part of this ledger; ordering is the order of entries and chunks. Do not
rewrite previous entries.

- Planning only: Re-audited LNX, `RunCommand()`, console byte
  transport, Amiga size/resize protocol, keyboard break routing, shell process
  groups, redirection tests, and the FMM/CRM security boundary. No production
  implementation has been started.

- Chunk 0 complete: added `tests/lnx_pty_probe.c`, the focused
  `tests/lnx_pty_test.py` characterization driver, and the `test-lnx-pty`
  Makefile target. The passing characterization records that the current LNX
  socket transport produces non-TTY descriptors, while a host PTY produces
  TTY descriptors; it also covers direct byte streaming and missing-command
  status. Build: `make build/LNX build/ace-user-shell build/ace-broker
  test-lnx-pty`. Tests: `make test-shell-redirection break-signal-test
  test-native-input test-native-console-handle test-console-channel
  test-console-device-bridge test-shell-return-code`; all passed. The break
  signal test was run through `make break-signal-test` so its required probe
  is built first. No production behavior was changed; Chunk 1 remains the
  first implementation chunk.

- Chunk 1 complete: added the opt-in `ACE_LNX_PTY=1` contract in
  `src/lnx_pty.h`, implemented PTY allocation/session setup, sane termios,
  bounded nonblocking full-duplex relay, child environment cleanup, inherited
  descriptor cleanup, and exit/signal propagation in `src/lnx.c`, and extended
  the existing probe/harness. The marker remains test-only as required; no
  `RunCommand()` descriptor classification was added yet. Build:
  `make -r /home/pi/repo/ace/build/LNX
  /home/pi/repo/ace/build/ace-user-shell
  /home/pi/repo/ace/build/ace-broker`. Tests:
  `make -r test-lnx-pty test-shell-redirection break-signal-test
  test-native-input test-native-console-handle test-console-channel
  test-console-device-bridge test-shell-return-code`; all passed. The focused
  test now verifies PTY descriptors, `TERM=xterm-256color`, session and
  controlling-terminal identity, marker removal, line relay, output larger
  than one relay buffer, normal statuses, signal termination, and unchanged
  direct/redirection/not-found behavior. The only implementation deviation
  from the proposed allocation sequence is that the slave is opened before
  `fork()` and reused after `setsid()`; `TIOCSCTTY` and all child-side
  terminal/session invariants are still established after `setsid()`.

- Chunks 2 and 3 complete together at the user's direction: `src/lnx.c` now
  queries the public ACE geometry protocol before the PTY target starts,
  applies initial and live `TIOCSWINSZ`, enables/resets raw resize reports,
  preserves query-adjacent typeahead, and translates the finite ACE keyboard
  table (including distinct Backspace/Delete and UTF-8 continuation safety)
  to the fixed `xterm-256color` contract. Its bounded output adapter maps
  xterm clear/alternate-screen transitions to ACE native clear operations and
  strips unsupported SGR, including indexed color, so unsupported bytes do
  not leak into ACE's legacy renderer. `COLORTERM` is removed. Added the
  narrow `native_console_same_endpoint()` helper and `RunCommand()` selection:
  only ACE's resolved LNX command in an interactive session with identical
  selected console endpoints and two exportable descriptors receives the PTY
  marker. Redirected, piped, split-endpoint, and abstract `CON:` cases remain
  direct; every official LNX invocation marks the target for
  `TERM=xterm-256color`, while standalone LNX preserves the caller TERM.
  The focused harness now models the query/resize stream, fragments all input
  bytes, verifies target geometry/SIGWINCH/output adaptation, and drives a
  private `ace-user-shell` through `LNX bash --noprofile --norc -i`; the
  redirection test also proves the scripted direct path. `RunCommand()` also
  recognizes a successful resolved `EndCLI` command in the parent, repairing
  its child-local CLI flag hand-off exposed by the interactive fixture. The
  `test-lnx-pty` target now depends on its real shell, broker, and EndCLI
  binaries so it cannot accidentally test stale activation code. The sole
  small test harness deviation is that its synthetic console sends EOF after
  `EndCLI`, because unlike the GUI it has no owner to close the endpoint.
  Build/tests: `make -r test-lnx-pty test-shell-redirection`; both passed
  before the common regression run. No privileged FMM/CRM or device-view
  component changed.
