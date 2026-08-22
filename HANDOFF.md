# ACE handoff

## Where it is installed

Two hosts are set up, both building and passing the full test suite:

* a Raspberry Pi (aarch64, Debian, labwc) at `~/repo/ace`, run from `build/`;
* `blackberry` (x86_64, Debian 13) at `~/repo/ace`.

`make install` installs into `~/.local/bin` and needs no privileges. A
system-wide install is a `PREFIX` override --
`sudo make PREFIX=/usr/local POLKIT_ACTIONDIR=/usr/share/polkit-1/actions AROS_ROOT="$HOME/aros" install` -- with
`AROS_ROOT` passed explicitly because `sudo` resets `$HOME` and the Makefile
defaults `AROS_ROOT` to `$HOME/aros`; without it the build looks for AROS
under `/root` and the install fails before it copies anything.

**One install per machine.** ACE programs find their companions beside their
own executable, so an install is a set that stays together, and two sets on
one `PATH` drift apart in the quietest possible way: the older set keeps
working, just old, and the newer one is simply never reached. This happened
here -- a `~/.local/bin` install shadowed `/usr/local/bin`, so a fresh
`sudo make install` left the panel icon starting the previous build, with no
symptom beyond a missing command that had only been installed to the root
nothing was reading. Hence the default prefix, the absolute path in the
launcher, and the warning `make install` prints when `PATH` will not select
what it just installed.

Vim is an optional companion because it is built from an external checkout.
After `make CC='ccache cc' vim VIM_SRC=/path/to/untouched/vim`, run
`make install-vim` to place `vim` and its packaged `runtime/` beside the
installed ACE shell. Otherwise test the checkout with `./build/ace-shell`,
not the older `ace-shell` found elsewhere on `PATH`.

## Current state

ACE is committed on `main` and pushed to `https://github.com/harbin-ctrl/ace`.
The current tree runs the real AROS `Shell.c` and AROS shell support code on a
Linux host. The host seam provides DOS state, broker-backed current directory
and variables, direct command loading, the console-device bridge, and the
Wayland/GTK window.

The real AROS `EndCLI.c` is included. Its state change is carried across the
child command process and stops the parent shell. Ordinary host commands are
not searched through `PATH`; `LNX` is the explicit direct Linux executable
escape hatch.

### LNX interactive PTY boundary

When the official `LNX` command is launched from the live ACE console with the
same unredirected console endpoint for input and output, `RunCommand()` marks
that invocation for PTY mode. The resulting process tree is:

```text
ace-user-shell
  -> LNX supervisor (owns the PTY master and relay queues)
       -> Linux target (new session, PTY slave, controlling terminal)
```

The supervisor's standard descriptors remain attached to the ACE console
channel. ACE keyboard bytes travel through the Amiga-to-Linux input adapter:
navigation/function keys become the corresponding xterm sequences, Backspace
and Delete stay distinct, and UTF-8 bytes are forwarded without being parsed
as ACE C1 controls. Target output travels in the other direction through the
bounded xterm-to-ACE output adapter. Geometry is requested with the public
`CSI 0 q` protocol; the supervisor consumes that reply, applies it with
`TIOCSWINSZ`, and enables the ACE resize report before starting the target.
Later size reports repeat the query and `TIOCSWINSZ` operation. The target is
given `TERM=xterm-256color` and no `COLORTERM` claim that ACE cannot render.

The ACE shell's break bridge sends Ctrl-C, Ctrl-D, Ctrl-E, and Ctrl-F events to
the supervisor, which writes the corresponding terminal bytes to the PTY.
Ctrl-D is treated as a PTY input byte only for this LNX foreground kind; the
ordinary ACE shell still treats it as a script boundary. The supervisor
tracks the PTY foreground process group so Ctrl-C/Ctrl-Z and shell job control
reach the active Linux job. On target exit it drains unread PTY output before
returning to the waiting ACE shell; target status never implicitly means
`EndCLI`. The supervisor uses per-call nonblocking socket operations on the
shared console descriptors, because changing their `O_NONBLOCK` file status
would also change the waiting shell's endpoint and make idle input look like
EOF. Console loss and supervisor signals use bounded HUP/TERM/KILL cleanup of
the target and its foreground group.

AmigaDOS redirection, pipes/scripts, split endpoints, and nonexportable
`CON:` handles deliberately stay on LNX's direct descriptor path. They do not
receive a PTY, keyboard translation, or interactive `TERM` contract; direct
mode is the byte-preserving Linux escape hatch for those cases.

The public startup policy is intentionally small. No switch starts an
ordinary user session; `--root` authorizes lazy requests to the separate
privileged mediator. The old `--user`, `--deviceview`, and `--mountview`
switches are retired. The broker selects the ordinary host view or the
mediator-backed device view from that authorization policy internally. Sudo
is tried noninteractively first; when that is unavailable, the mediator uses
the installed `org.ace.Ace.mediator` polkit action.

`Edit`, the AmigaDOS line editor, is the one command here that is written
rather than ported: AROS has no source for it, so `src/edit.c` follows the
AmigaDOS manual's description instead. It is written to `dos.library` and
`exec.library` alone -- no stdio, no `malloc`, no host calls -- so the same
file is meant to compile for AmigaOS and AROS as well as for ACE, which is
also why it is the one place to be careful about adding a host dependency.
`make test-edit` covers it. README has the behaviour, including the two places
where the manual contradicts itself.

The real AROS BOOPSI implementation is built from unmodified AROS source in
`rom/intuition` and `compiler/alib`, on the ACE seam in
`src/aros_boopsi_runtime.c`. This is the first step of moving the display seam
down from `console.device` to Intuition: AROS implements `console.device` as
BOOPSI classes, so nothing above that boundary can run without it.

The real AROS console.device classes that touch pixels -- `stdconclass.c` and
`consoleclass.c` from `rom/devs/console` -- are compiled unmodified against a
real `graphics.library`, `src/aros_graphics_runtime.c`. Unlike every other
seam in ACE, this one is authored rather than compiled from AROS source:
graphics.library is the actual hardware boundary, the point past which "real
AROS code" would mean a HIDD driver stack ACE does not want. It renders
through cairo/fontconfig onto an ACE-owned `RastPort`/`BitMap`, with glyphs
from a host TrueType font rather than a bitmap font ACE would have to ship --
see the file header for what AROS's console classes do and do not require of
a font.

### How graphics.library draws, and why it is shaped that way

The seam started out routing every drawing call through cairo, one character
cell at a time. Measured on this Raspberry Pi with a 900x576 console, that
cost 10.8 ms for each line of output once the console had filled up and
started scrolling -- five and a half seconds for five hundred lines -- and a
typeface or palette change took six seconds. Three things were responsible,
and the file is now organised around avoiding them.

**Scrolling does not copy the console.** `ScrollRaster()` used to snapshot the
whole scroll box into a second cairo surface, fill the box, and blit the
snapshot back: four passes over the console plus a two-megabyte allocation,
for every single line. It was 86% of the profile. A console almost always
scrolls the same shape -- everything from the top-left corner, up by one text
line -- so the surface is allocated taller than the console and that case
moves the *viewing origin* down inside it instead, leaving the pixels alone
and filling only the one line the scroll uncovers. The origin is folded back
to the top of the allocation when the headroom runs out, which is the only
time a scroll copies anything. `ace_gfx_rastport_surface()`'s callers
therefore have to read the console's rows from
`ace_gfx_rastport_origin_y()` rather than from row zero, which is the one
externally visible consequence.

The scroll box does not always reach the surface's right and bottom edges --
a window whose pixel size is not a whole number of cells leaves a margin --
and moving the origin moves that margin too. `scroll_by_origin()` checks that
the margin already holds the colour the scroll would fill with rather than
assuming it, and hands back to the copying path if it does not.

**Rectangles are filled, moved and inverted directly.** `RectFill()`,
`ScrollRaster()` and the COMPLEMENT cursor are rectangle copies and solid
fills; going through cairo meant a pixman composite for work a `memmove`
already does. They write the image surface's pixels between a
`cairo_surface_flush()` / `cairo_surface_mark_dirty_rectangle()` pair, which
is cairo's documented contract for exactly this and costs nothing on an image
surface. Cairo still draws every glyph, which is the part that needs it.

**Glyphs are looked up once.** `Text()` used to call `cairo_set_font_face()`,
`cairo_set_font_size()` and `cairo_show_text()` per character, each of which
re-resolved the scaled font and redid a FreeType character-map lookup. There
is now one `cairo_scaled_font_t` per style built at font load, a cached glyph
index per byte, and one `cairo_show_glyphs()` per run of text with every
glyph positioned explicitly on the console's own integer cell pitch. Two real
bugs fell out of that rewrite: the old code rendered at `tf_YSize` (the line
height) while `tf_XSize` had been measured at the requested pixel size, so
glyphs were wider than the cells they were laid out on; and `INVERSVID`,
which `setabpen()` in `stdconclass.c` sets for reverse video, was ignored
rather than swapping the two pens.

Together these take a line of output from 10.8 ms to 0.34 ms, a resize step
from 7.2 ms to effectively nothing, and a typeface or palette change from six
seconds to about 0.3 -- and, because the retained stream is now bounded, that
last figure no longer grows with how long the shell has been running. Driving
the real window end to end, twenty thousand lines render in six seconds where
they would previously have taken more than three minutes.

The cost is memory: the surface carries scroll headroom (a 6 MB budget,
clamped to between 256 and 2048 rows) and 128 columns of width slack, so it
is roughly twice the console's own size. A resize that still fits inside that
allocation does not reallocate at all, which is what makes a live drag cheap.

`ace_gfx_take_damage()` reports the bounding box of everything drawn since
the last call, so `amiga_console.c` repaints only what changed instead of
handing the compositor a whole window per frame.

The real ANSI/CSI parser, `rom/devs/console/support.c`'s `writeToConsole()`,
is compiled the same way and is now what the live `ace-console` window
actually calls: `src/console_device_bridge.c` builds one real `ConUnit` per
window (the class pair, a font, an `ace_gfx_create_rastport()`-backed
`RastPort`, a real `struct Window`) and `src/amiga_console.c`'s socket output
path calls `writeToConsole()` on it directly, the same call `console.c`'s real
`beginio()`/`CMD_WRITE` would make. `draw_console()` blits the RastPort's
cairo surface straight onto the window. `src/console_terminal.c` -- ACE's own
hand-written ANSI parser and character-cell renderer, the thing this whole
seam exists to retire -- is deleted, along with its test.

`console_device_bridge.c` exists because AROS's headers and GTK/glib's
cannot be included in the same translation unit (both define `struct
timeval`; `console_gcc.h`'s `MAX`/`MIN` collide with glib's) -- the same
reason `aros_graphics_runtime.h` forward-declares `RastPort`/`BitMap`/
`TextFont` rather than including their real headers. `amiga_console.c` only
sees the bridge's opaque `struct ace_console_device`; every real AROS type is
confined to `console_device_bridge.c`.

`make test-graphics` constructs a real `ConUnit` through `NewObjectA()` and
drives it two ways: AROS's own `Console_DoCommand()` macro directly (proving
class construction and plain-text dispatch), and `writeToConsole()` with a
raw CSI byte sequence (proving the escape-sequence *parser* itself -- the
part `Console_DoCommand()` alone never exercises, since that macro dispatches
an already-parsed command). The CSI test drives `CSI n C` (cursor forward),
confirmed present in `support.c`'s real command table. ANSI SGR color and
text rendition are handled by `consoleclass.c` in this AROS checkout and are
covered by the live rendering path.

`Delete` and `Protect` are the first commands that change the filesystem
beyond creating a directory, and every command in the tree now parses its
arguments with AROS's own `ReadArgs()`; both are described in their own
sections below.

The first read-only filesystem-facing command is now ACE's forked `src/dir.c`,
built with the original DOS `patternmatching`, `MatchFirst`/`MatchNext`/
`MatchEnd`, and `ExAll` implementations. `src/native_dos.c` supplies the
host-backed
`Lock`/`Examine`/`ExNext`/`DupLock`/`CurrentDir` seam, Unix metadata conversion,
and the RawDoFmt-compatible `VPrintf` path that `Dir` uses for its formatted
columns. The command has been exercised against regular and nested host
directories, `#?` patterns, `ALL`, `DIRS`, `FILES`, and missing paths. The
compatibility layer intentionally leaves the DOS root's optional `*` wildcard
flag disabled, matching the configured AmigaDOS pattern behavior.

## Both argument parsers are now AROS's

ACE used to have two. `compat/include/aros/shcommands.h` was a 170-line
restatement of AROS's macro header whose `AROS_SHn` expansion called
`native_parse_args()` -- 357 lines of hand-rolled template parsing in
`src/native_args.c` -- while five commands that call `ReadArgs()` themselves
went to the real thing. Nineteen commands against five, decided by nothing but
how each command's author happened to declare its arguments.

Both files are gone. AROS's own `compiler/include/aros/shcommands.h` is
compiled instead, and what it needs from the host is small: `AROS_PROCH` in
`compat/include/aros/asmcall.h` (built from the `AROS_UFH3` already there), a
three-macro `compat/include/aros/symbolsets.h` -- ACE has no link-time library
sets to walk, so opening them vacuously succeeds -- and
`compat/include/ace_shcommand_host.h`, which supplies the `main()` that stands
where `CreateProc()` stands on a real AROS.

Three things had to be true underneath, and only one of them was.

**A command's argument line reaches `ReadArgs()` through the input stream.**
That is AmigaDOS's own mechanism -- the shell leaves the line in the stream and
`ReadArgs()` reads it -- and ACE already had it, as `ACE_COMMAND_ARGUMENTS`
carried across `execv()` by `src/native_command.c` and injected by
`native_load_input_prefix()`. What it did not have was the case with no shell:
a command run straight from a Linux shell never saw its own `argv`, so
`./build/MakeDir WORK:x ALL` -- an invocation README documents -- answered
`required argument missing`. `src/native_shcommand.c` puts `argv` back together
into one AmigaDOS line, quoted the way `readitem.c` will take it apart, and
sets it only if the shell has not already set one, so the shell's own parse
stays authoritative. An invocation with no arguments at all sets an *empty*
line rather than nothing, which is the difference between `ReadArgs()`
reporting a missing `/A` argument and `ReadArgs()` reading on into the next
command in the shell's own input.

**`IoErr()` has to be the process's `pr_Result2`.** ACE kept it in a variable
beside the process. `rom/dos/readargs.c` ends with `me->pr_Result2 = error;`
and never calls `SetIoErr()`, so with two stores every `ReadArgs()` failure
reported no reason at all: the command's `PrintFault(IoErr(), name)` printed
nothing and exited 20. `IoErr()` is now that field. This had been true since
`readargs.c` was first compiled in and was invisible while only five commands
used it; it is the same hazard as the `ErrorReport()` polarity bug in TODO.md,
and worth remembering as the shape rather than the instance.

**A command may own `SysBase` and `DOSBase`.** `Dir.c` sets
`SH_GLOBAL_SYSBASE` and `SH_GLOBAL_DOSBASE` before including the header, which
makes the macro expansion define both at file scope -- on a real AROS build
they are the whole program's only copy. ACE's runtime is linked in beside the
command, so the two collided at link time. ACE's definitions are now weak and
yield to the command's, and `ace_shcommand_start()` passes the entry point a
base from `native_exec_base_pointer()` rather than reading back a symbol the
command has not filled in yet.

Verified by exercising each command with a real template case -- `/S`, `/K`,
`/M`, `/N`, `/A` missing, and the `?` help convention including a repeated `?`
-- and by checking that `nm build/<command>` no longer resolves
`native_parse_args` anywhere.

## Delete and Protect, and the volume aliases they turned up

`Delete` and `Protect` are the first destructive commands. Both were already
on the real parser by construction, and both needed one new seam call,
`SetProtection()` -- the `chmod()` inverse of the `native_protection_from_stat()`
mapping `Examine()` already used, so the bits `Protect` writes are the bits
`Delete` reads back to decide whether it may remove something. `DeleteFile()`
also had to learn that one AmigaDOS call removes either kind of object where
Unix splits `unlink()` from `rmdir()`.

`Protect` additionally wanted `IsDosEntryA()`, so AROS's own
`compiler/arossupport/isdosentrya.c` is compiled too, on an
`AttemptLockDosList()` that is ACE's `LockDosList()` -- nothing else can hold
a list this process builds for itself.

Adding those tests turned up a bug that had nothing to do with them: on this
Raspberry Pi the whole filesystem suite was already failing at HEAD, because
`src/assign_compat.c` registered only a block device's *kernel* name in the
DOS list AROS's `GetDeviceProc()` searches. The broker treats the kernel name,
the filesystem UUID and the filesystem label as interchangeable, and its
host-to-AmigaDOS translation hands back the label when there is one -- so
`rootfs:home/...` failed to lock with ERROR_DEVICE_NOT_MOUNTED while
`sda2:home/...`, the same filesystem, worked. All three spellings are now
registered. This is exactly the Amiga distinction it looks like: the kernel
name is the device, the label is the volume.

One test had `sda2:` written into it, which is this host's root device; it now
takes the expected spelling from the broker.

## Dir's directory behavior, found by comparison against a real Amiga

The forked `src/dir.c` sorts the files in a listing (`qsort(files->entries, ...)`)
but not the directories -- the equivalent call for `dirs->entries` is present
in the source and commented out. `Dir` on ACE therefore listed subdirectories
in raw host filesystem order rather than alphabetically, which was visible
next to the correctly-sorted files in the same listing.

This was found by running `Dir` on the same directory through both ACE and a
real Amiga 1200 under emulation, comparing byte for byte: see "Driving a real
Amiga" below. The real machine sorts directories case-insensitively, the same
as files, so the fork carries that fix. A second,
related defect came from the same comparison: `Dir ALL FILES` did not recurse
into subdirectories on ACE, because the traversal loop in `doDir()` is gated
on `doDirs` -- whether directory *names* are printed -- rather than on `all`
-- whether the tree is *walked*. `FILES` alone sets `doDirs` false, silently
disabling recursion regardless of `ALL`. The fork changes the gate to
`doDirs || all`, which does not change what is printed, only what is walked.

These are kept in ACE's fork because `Dir` is a small, central command and its
behavior must not depend on a manually patched external AROS checkout.

## Driving a real Amiga

`tools/amiga-edit-chassis` (an AmigaDOS script) and `tools/amiga-edit-drive.sh`
(this host's side) turn a directory the emulator shares with the host into a
request/response channel: stage a file and a command, raise a flag, wait for
an answer file. Built for checking `Edit` against AmigaDOS's own EDIT -- see
the file header and README's Edit section -- it generalizes to anything that
can be driven through a `WITH`/`VER`-style file interface, and a second,
ad hoc chassis extended it to run `Dir` directly for the sort/recurse finding
above.

Two things worth knowing before extending it further. First, `EXECUTE` should
not be nested inside a script that is itself run by `Execute` -- every version
that tried it died silently after one task, apparently on the return from the
inner `EXECUTE`; every version that runs the target command directly in the
polling loop has been stable. Second, the emulator's own shared-drive layer
can serve a stale directory listing to the guest for some number of seconds
after a host-side change -- a file deleted from Linux can still read as
present to the Amiga's `IF EXISTS` for a little while. A reboot of the guest
clears it; there is no known way to force a refresh short of that.

## Filenote, and the two things it found

`Filenote` needed one new call, `SetComment()`. An AmigaDOS file comment is
the one piece of Amiga file metadata with no Unix field to map onto -- unlike
protection bits, which are permissions -- so it goes in an extended attribute,
`user.comment`. That puts it on the inode, which is why it survives a rename
without ACE doing anything. The name is deliberately generic rather than
ACE's own; the cost is that a foreign writer is not bound by AmigaDOS's
79-character limit, so `native_fill_fib_comment()` truncates what it reads
instead of trusting it. Writing longer than that is refused rather than
truncated, since a `FileInfoBlock` could not carry it back. A filesystem with
no extended attributes reports ERROR_ACTION_NOT_KNOWN -- AmigaDOS's answer for
a handler that does not implement an action, and not hypothetical, since the
broker registers this Pi's VFAT bootfs as a volume.

`Examine()` and `ExNext()` fill `fib_Comment`, at one `getxattr()` per
directory entry. Nothing ported yet displays a comment -- `List` is the
AmigaDOS command that does -- so `tests/dos_comment_test.c` is the reader that
keeps the read half honest until it lands.

Two things turned up underneath.

**`LockDosList()` returned NULL.** ACE's own `NextDosEntry()` read that as
"start at the beginning", and every AROS caller that assigns the result and
then walks it -- `Assign.c`, `Delete.c` -- survived. AROS's own
`compiler/arossupport/isdosentrya.c` does not: it reads NULL as "the list
could not be locked" and reports that nothing matched. So `Protect` had been
silently accepting a whole volume since it was ported an hour earlier, and
would have chmod'd the volume root. There is now a list header to hand back,
as real AmigaDOS has.

**ACE has two BSTR conventions and the compat macros only matched one.** The
CLI's `cli_Prompt`, `cli_SetName` and `cli_CommandFile` are plain C strings
that AROS's real `Shell.c` and `cliPrompt.c` read through `AROS_BSTR_ADDR`,
so `<dos/dosextens.h>`'s definitions have to leave them alone. A DosList
entry's `dol_Name` is a real length-prefixed BSTR, because `Assign.c` reads
that byte back through a macro of its own that ACE cannot redefine. The
compat macros are now the guarded default and `src/assign_compat.h` overrides
them for the DosList world, which is the only place that disagrees.

Both were invisible until an AROS source read a DosList the way AROS writes
them, which is the same shape as the `IoErr()` storage bug above: an ACE stub
that is merely *wrong* stays harmless until upstream code that depends on it
gets compiled in.

## Current-console roadmap

The console architecture is being built in stages. The target model is the
Amiga one: a Shell/CLI process talks to a CON: handler through DOS file
handles, the handler owns cooked input and window-specific state, and
console.device owns the terminal protocol and rendering. ACE's current GUI
already has a useful subset of that path, but it is a Unix socket between a
shell child and the GTK/AROS console renderer. This roadmap keeps that subset
honest while making it possible to replace pieces incrementally.

### Stage 1: current-console channel (implemented)

Stage 1 is the smallest useful channel boundary for ET and other full-screen
programs. It has one owner, one byte stream, and no new-window semantics:

```text
child stdout/stderr ──► current-console channel ──► console.device renderer
child stdin       ◄─── current-console channel ◄─── GUI keyboard
```

The channel is represented by `src/console_channel.[ch]`. It now owns:

- the stream send/receive operations;
- current character-grid rows and columns, refreshed after opening and
  resizing the ACE console window;
- an explicit raw-mode state field for the later DOS mode-control seam; and
- optional byte tracing at the channel boundary.

The trace is deliberately raw, with no record headers, so it can be compared
directly with the Amiga-side `tools/amiga-debugcon` output:

```sh
ACE_DBGCON=/tmp/et-console.bin ace-shell
ACE_DBGCON_INPUT=/tmp/et-keyboard.bin ace-shell
```

`ACE_DBGCON` records bytes received from the child immediately before ACE
passes them to the console parser. `ACE_DBGCON_INPUT`, when requested,
records bytes sent by the GUI keyboard path. Each file is replaced when the
console process starts. Tracing is disabled unless the variable is set and
never adds bytes to the emulated console stream.

ET no longer receives geometry through the ACE-only `ACE_CONSOLE_ROWS` and
`ACE_CONSOLE_COLS` environment variables. In an ACE console session it sends
the public `CSI 0 q` console size query and parses the reply from the current
console. Its raw-mode state is explicit in the TINE console backend and ends
when `tine_endwin()` restores the stream. The existing resize event remains
the notification path: ACE's console renderer injects the Amiga
`IECLASS_SIZEWINDOW` report, and ET handles it as `KEY_RESIZE`.

Stage 1 intentionally does **not** create a second socket, implement a new
window specification, or claim that the GTK socket is a full CON: handler.
The standard DOS current handles (`CONSOLE:`, `CON:`, and `*`) still use the
native ACE current-console implementation. The point of this stage is to
give that path a named, testable boundary and a byte-level observation point
without changing ET's working stream protocol.

### Stage 2: handler-facing current console

Move the current-console state into the native DOS handle layer. `Open()`,
`Read()`, `Write()`, `SetMode()`, `WaitForChar()`, geometry queries, and
resize notifications should all reach the same channel object. The raw flag
must then be changed by DOS `SetMode()` rather than being independently
remembered by each consumer. Keep `CONSOLE:`/`*` as aliases for the current
console and retain the Stage 1 trace at the channel boundary.

### Stage 3: real CON: instances (implemented)

Add a separate channel instance for each `CON:x/y/w/h/title/...` window
specification. `CON:` opens or creates that instance; `CONSOLE:` and `*`
refer to the current one. The GUI may still provide the actual host window,
but the DOS handle must select the instance rather than silently sharing the
current stream. This is the point at which NEWCLI can be made faithful: it
opens its requested CON: handle and passes it as `SYS_Input`, with output and
error allowed to follow the same handler.

ACE now parses the geometry and title fields, starts an `ace-console` window
with a socket endpoint for each parameterised `CON:` handle, and keeps
`CONSOLE:`/`*` on the current-console channel. `SystemTagList()` duplicates
the selected handle into the new shell's standard input, output, and error
streams; omitted output/error handles follow the input console. A
`SYS_ScriptInput` handle is duplicated into the child through the existing
`ACE_SCRIPT_INPUT` seam, so AROS's `NewCLI` can run its startup file and then
return to interactive input. The remaining handler packet split belongs to
Stage 4.

### Stage 4: Shell/CLI and console.device boundaries (implemented)

The launcher/Shell split is now explicit in the host layout: `ace-shell`
launches the console window, `ace-console` owns the window and its host event
loop, and `ace-user-shell` is the Shell process that owns the DOS CLI state.
The Shell is not the window or the renderer, just as CLI is not CON: on an
Amiga.

`src/native_console_endpoint.[ch]` is the DOS-side adapter for the remaining
boundary. Each current-console alias and each parameterised `CON:` instance
owns (or refers to) an `amiga_console_device`/`amiga_con_handler` pair. Native
DOS `SetMode()`, `Read()`, `FGetC()`, `FGets()`, `FRead()`, `FPutC()`, `FPuts()`,
`PutStr()`, `Printf()`, `VPrintf()`, and the shell editor's echo now pass
through handler operations, which submit `CMD_READ`/`CMD_WRITE` requests to
the packet worker. Current aliases still share raw state; separate `CON:`
windows still have independent handler state.

The endpoint's read callback waits in short, cancellable intervals, so closing
a console cannot strand a worker in a host `read(2)`. The GUI remains the
thread-affine consumer of the channel: its main loop receives the same raw
write bytes and hands them to the AROS console renderer. `ACE_DBGCON` therefore
sees the bytes at the channel boundary without adding a private record format.
The full AROS `console.c` task/`BeginIO` implementation is still unnecessary;
the adapter supplies the `OpenDevice()`/`DoIO()` semantics callers currently
need. The next work is Stage 5's byte and visual comparison against Ed.

### Stage 5: ET fidelity pass (implemented)

Use the staged channel and trace corpus to compare ET against AmigaOS 3.1
ED: startup, raw-mode entry/exit, public size queries, resize reports, mode
switching, cursor movement, screen redraws, and shutdown. Any byte-level
exception should be documented as either an intentional ACE transport detail
or corrected in the appropriate channel/handler/device layer, not patched
inside ET to compensate for a lower-layer mismatch.

The saved-buffer comparison is real rather than inferred. The fresh FS-UAE
AmigaOS 3.1 ED 2.00 probe in `tools/ed-amiga-probe` established that a missing
file starts with one blank line, that `X` saves that line, and that `U` does
not partially undo an `I`/`A` structural line insertion. ET matches those
bytes at the editor/buffer boundary. `tests/tine_test.sh` replays the golden
cases for a new file, `I`/`A`, `U` after `I`, and the existing ED compatibility
corpus.

The screen/channel pass is covered by
`tests/tine_screen_trace_test.py`. It drives ET through a PTY while observing
the same raw child-to-console boundary that `ACE_DBGCON` records. The fake
console answers the public `CSI 0 q` query and injects one native
`IECLASS_SIZEWINDOW` report, so the test exercises startup, raw-mode event
setup, form-feed and row clearing, status-pen transitions, retained redraws,
resize geometry, command-mode entry, and shutdown. ET now resets every raw
event class it enables (`12`, `2`, `10`, and `11`) instead of leaving the
shared CON: configured for the next command. The test is part of `make
test-tine`; the real-Amiga `tools/amiga-debugcon` format remains the external
reference when a 68K DBGCON run is available.

## Child-side console input and raw mode

The GUI now sends each key sequence directly to the child socket. The AROS
console editor is no longer owned by `ace-console`: `native_dos.c` enables it
only for the GUI-launched session, feeds complete CSI sequences to AROS's
`process_input()`, and writes its echo through the child process's normal
`Output()` stream. This keeps editing, history, and echo in the process that
owns `Input()`, and means a child can take over the stream without a second
IPC path.

`SetMode(Input(), 1)` disables cooked editing and makes `Read()` expose raw
bytes. `WaitForChar()` is backed by `select()` with AmigaDOS's microsecond
timeout, which is the input contract Vim's Amiga backend uses. The focused
`test-native-input` target checks cooked history, complete CSI handling, raw
reads, and readiness. The editor/runtime objects are part of the DOS runtime;
`native-list-compat.c` supplies the external Exec list symbols expected by
the imported handler while ACE's own list callers keep their inline helpers.

The GUI sets `ACE_CONSOLE_INTERACTIVE=1`; ordinary pipes and scripts do not,
so their output remains byte-for-byte stream behavior rather than acquiring
interactive echo sequences.

The first Vim slice is now reproducibly buildable without changing Vim. Point
the external-source target at a fresh checkout:

```sh
make CC='ccache cc' vim VIM_SRC=/path/to/untouched/vim
```

`tools/build-vim-ace.sh` compiles Vim's normal-feature core, Amiga backend,
and six xdiff objects into `build/vim-objects`, then links `build/vim` with
ACE's raw-input, broker, process, DOS-path, and pattern seams. It refuses to
generate files in the Vim tree. The `compat/vim/include/devices/conunit.h`
forward declaration is kept in ACE because Vim includes that header
unconditionally while its `__AROS__` shell-size path does not access
`struct ConUnit`.

The five platform link seams are now ACE-owned: `Delay`, `SetWindowTitles`,
`mch_get_cmd_output_direct`, `vim_fsync`, and the file/path wrappers used by
Vim's Amiga POSIX mappings. The ACE build also packages the untouched
checkout's `runtime/` beside `build/vim`, translates the Amiga `PROGDIR:`,
`VIM:`, and `VIMRUNTIME:` runtime aliases, and supplies the directory probes
needed by Vim's runtime search. `xdl_diff` comes from Vim's own six xdiff
objects.
The current source did not require `Info`, `DateStamp`, or `Rename` at this
boundary. `IsInteractive()` also recognizes ACE's wrapped standard handles,
which lets the untouched backend take the raw terminal path instead of trying
to open `NIL:`.

Validated with the external checkout: Vim launched in a pseudo-terminal,
accepted raw input, created and saved a file, reloaded it, and quit cleanly.
The same untouched binary now starts in the live `ace-console`, loads
`defaults.vim` and syntax support, creates a new buffer, inserts text, writes
25 bytes, and exits cleanly. The source checkout remained clean. The first
smoke run with a host absolute path exposed Amiga leading-slash semantics;
ACE's Vim-only file wrappers now preserve explicit host absolute paths while
routing Amiga-relative paths and assigns through the broker.

One trap worth knowing, since it wasted time twice in a row: the Makefile is
not a prerequisite of anything it builds, so changing a rule's recipe does not
rebuild the object. Both times the symptom was a fix that appeared not to
work. Making it a prerequisite is not a one-line change -- roughly twenty
recipes pass `$^` straight to the compiler or linker and would need the
Makefile filtered back out -- so it is still a trap. Delete the object by hand
after editing its rule.

## Build on another host

The ACE Makefile uses `AROS_ROOT`, defaulting to `$HOME/aros`, and selects the
AROS CPU include directory from the host architecture:

```sh
git clone https://github.com/aros-development-team/AROS.git "$HOME/aros"
cd "$HOME/aros"
git checkout 53af6e419b
cd "$HOME/repo/ace"
patch -p1 -d "$HOME/aros" < patches/aros-console-seam.patch
make -j2 all
make test-aros-console-editor test-aros-exec-runtime \
     test-console-device test-exec-compat test-boopsi test-graphics
```

`test-graphics` additionally needs `cairo` and `fontconfig` development files,
and at least one complete monospace family on the host (regular, bold, italic
and bold-italic faces of the same family) -- `Liberation Mono`, `DejaVu Sans
Mono`, or the `monospace` fontconfig alias are tried in that order.

For an unusual host, override both variables explicitly, for example
`make AROS_ROOT=/opt/aros AROS_CPU_ARCH=aarch64-all -j2 all`.

Built and tested on two hosts: a Raspberry Pi (aarch64, Debian, labwc) and an
x86_64 Debian 13 machine. The second one turned up a portability bug worth
knowing about, because it is the kind this project will keep meeting. Real
AROS declares `STRPTR` as `UBYTE *`; ACE's `compat/include/exec/types.h`
declares it as plain `char *`, which is **unsigned on ARM and signed on
x86**. A parsed AmigaDOS pattern stores its tokens as `P_ANY` and friends,
`0x80..0x88` in `dos/dosasl.h`, so on a signed-char host every one of
`patternmatching.c`'s `case P_ANY:` labels becomes unreachable and `#?` quietly
stops matching anything. gcc rejects it outright as
`-Werror=switch-outside-range`, which is how it surfaced. The AROS DOS
pattern sources and `Dir` are compiled with `-funsigned-char`
(`AROS_DOSPAT_CFLAGS`) to give plain `char` the signedness AROS wrote them
against. Anywhere else ACE hands a real AROS source a `char` that AROS
declared `UBYTE` is a candidate for the same bug, and it will not always be
loud.

The patch is required for the AROS console handler to compile without
GadTools, Workbench AppWindow, and completion support. It leaves the original
console editing and history code in place. Apply it once to a clean checkout;
`patch` will report that it is already applied if repeated. It touches only
`rom/filesys/console_handler`; the BOOPSI sources need no patch.

GTK 3 development files, `blkid` development files, a C compiler, `make`,
`git`, `bison`, and a Wayland session are required for the complete
build/window.  `bison` generates `Eval`'s parser from AROS's
`workbench/c/evalParser.y`; a tree copied from another host may carry the
generated files in `build/` and hide the fact that the tool is missing, until
something forces them to be regenerated.

The console patch is required, and `patch` reports "previously applied" if
repeated, so applying it to an existing checkout is safe. It is worth checking
that it took: a `cp -a` of this tree between hosts copies ACE but not
`$HOME/aros`, and `git -C "$HOME/aros" status` should show only
`rom/filesys/console_handler/con_handler.c` and `.../support.c` modified.

The Regina port has a third external checkout, `$HOME/stash/aros-contrib`,
which is not needed for `make all`. See `docs/regina-amiga-port.md` for what
it is and how to recreate it.

## Launch

From the ACE checkout:

```sh
./build/ace-broker
ACE_SESSION=main-shell ./build/ace-shell
```

The real shell starts a sibling broker automatically when the configured
socket is unavailable. `NewCLI` opens another ACE console. `EndCLI` ends the
current AROS shell.

## The broker

Not a systemd service and not socket-activated: a plain `AF_UNIX` socket the
broker binds itself. It starts two ways -- explicitly through `broker-start`,
or implicitly, when any ACE command finds the socket unreachable and forks one
from the `ace-broker` sitting beside its own executable. Either way it is
detached, and it runs until something sends it SIGTERM. There is deliberately
no idle timeout.

**What it holds.** Per session -- keyed by `ACE_SESSION` -- the current
directory, command paths, assigns, local variables, RC/Result2, fail level,
prompt, and foreground task. That state is anchored by the shell that owns it
and freed the moment the shell disconnects, so it does not accumulate.
Broker-wide: the task registry, global (`Setenv`) variables, SYS:, and the DOS
volume list. This second group is the state a stale broker gets wrong, and
`Setenv` is now the only part of it that cannot be rebuilt -- the escaped
filename spellings used to live here too, and are a pure function now.

**What one broker is.** Its socket is
`$XDG_RUNTIME_DIR/ace-broker-<sys>-<protocol>.sock`, and both halves of that
name are load-bearing:

* `<sys>` is a hash of the resolved SYS: root. SYS: is what makes an ACE
  system that system -- on a real Amiga "which machine" is answered by which
  volume you booted -- so two different roots are two different systems and
  get two different brokers. `XDG_RUNTIME_DIR` makes it per-user and per-login
  besides, since the system clears that directory when the user's last session
  ends.
* `<protocol>` is a hash of `src/broker_protocol.h`, computed by the Makefile.
  It is derived rather than hand-maintained because a version nobody remembers
  to bump is worse than none. Its presence in the *name* is what makes stale
  brokers harmless during development: a rebuilt command whose protocol
  changed does not find the previous build's broker at all, so it starts its
  own instead of talking to something that would misparse it.

Note what this does not separate. Once ACE is installed, a build tree and the
install both resolve the same `ACE_SYS_DIR`, so they are the same system and
share a broker -- which is correct when their protocol matches, and handled by
the name when it does not.

**When they do meet anyway.** A hand-set `ACE_BROKER_SOCKET` puts two builds
on one path regardless, and a compile outside `make` carries no version at
all. For those the version is also folded into the protocol magic, so a
mismatch is caught on the first field either side reads rather than after a
payload has been misparsed, and each side can name the other's version:

```text
ace: the broker on /run/user/1000/ace-broker-...sock speaks protocol
     0x27fbf499; this build speaks 0xdeadbeef.
ace: it is an older or newer ace-broker still running. Stop it with
     broker-stop and retry.
```

**Asking what is running.** `ace-brokerctl status` reports the client's own
protocol, SYS: and socket, then the broker's protocol, executable, pid,
uptime, socket, SYS:, and its live sessions and tasks. `ace-brokerctl socket`
and `ace-broker --print-socket` print where a connection would go without
starting anything.

Two consequences of having no idle timeout. Superseded brokers linger as
processes until logout clears `XDG_RUNTIME_DIR`; they are unreachable rather
than dangerous, but they do accumulate across a day of protocol edits, and
`broker-stop` only stops the one matching the current build. And `ace-broker`
takes any argument as a socket path, so an older one handed `--print-socket`
binds a socket by that name and serves it forever -- which is why the
start/stop scripts bound that call with a timeout and reject anything that is
not an absolute path.

## Known boundary

The AROS source tree remains an external source dependency. ACE currently
compiles selected AROS commands, the AROS shell, the AROS console handler,
AROS BOOPSI, and the whole pixel-facing half of AROS's console.device
(`stdconclass.c`, `consoleclass.c`, `support.c`) against the compatibility
headers in `compat/`.

**What "console.c itself is not compiled" actually means.** `console.c` is
the device's task/`BeginIO`/message-port machinery -- the generic AmigaOS
device-I/O protocol for a program that calls `OpenDevice()`/`DoIO()` and
waits. ACE's shell-in-a-window architecture never uses that protocol for
rendering: the shell child process's stdout is a plain Unix pipe read
directly by `amiga_console.c`, and the console handler's own self-echo
(`rom/filesys/console_handler/support.c`, already real, Phase 1) writes
through a synthetic `console.device` in `src/aros_exec_runtime.c` that exists
only to give `process_input()` a `struct ConUnit`-shaped context to edit
against -- a second, separate ConUnit from the one this phase added for
rendering. Both byte streams converge on one function call
(`ace_console_device_write()`), which is where the real `writeToConsole()`
now sits. Confirmed by tracing the actual data flow, not assumed; see the
git history for the trace. `console.c`'s task/BeginIO layer would only start
mattering if ACE needed a general `OpenDevice()`/`DoIO()`-driven program to
talk to a console window, which nothing in the current architecture does.

**Keyboard mapping is deliberately not AROS's.** `key_press()` in
`src/amiga_console.c` translates GDK keyvals to CSI byte sequences directly,
rather than going through AROS's real `rom/devs/console/cdinputhandler.c`
(Intuition-level input-event handling) and `rawkeyconvert.c` (a thin wrapper
over keymap.library's `MapRawKey()`, which needs a real default `KeyMap`
table -- no self-contained one turned up anywhere in `rom/keymap`; real
AmigaOS loads it from a `DEVS:Keymaps/` file ACE has no filesystem for).
This was assessed and then deliberately ruled out, not merely deferred:
keyboard layout is a physical-device concern, the same category as the
window surface itself, and GDK/Wayland already does it correctly for
whatever keyboard and layout the host has -- reimplementing an Amiga keymap
table on top would be strictly worse, not more authentic. `key_press()`'s
translation is the permanent design here, on the same footing as
`graphics.library` being authored rather than compiled: this is where the
seam sits, not a stopgap for AROS code to eventually take over. Editing
itself (cursor movement, backspace, history) runs in the child on real AROS
code, via `process_input()`; only the *encoding* of GDK key events into the
bytes it expects is ACE's, and stays that way. Raw-mode programs receive those
same bytes after `SetMode()` disables the child-side editor.

Font and palette selection is host-side: the ACE Shell GTK menu offers a
monospace typeface chooser and eight color slots, validated through the same
`ace_gfx_load_font()` path as startup. The bridge retains the raw console
stream, rebuilds the real console unit and RastPort for a typeface or palette
change, and replays the stream so the visible contents are repainted
immediately with the new cell metrics and eight pens.

The retained stream is bounded, at several screenfuls sized from the console's
own grid and dropped from the front at a line boundary. It exists only to
repaint after a typeface or palette change, so it needs to hold what is still
on screen, not the whole session; unbounded, it made those two operations cost
time proportional to how long the shell had been running, since the repaint had
to re-render and re-scroll past every line ever written.

A window resize usually does not go through that rebuild at all.
`ace_gfx_resize_rastport()` changes the surface's dimensions with the pixels
intact and the bridge then invokes AROS's real `Console_NewWindowSize()`
geometry path, so a live drag costs a geometry update rather than a re-render
of the stream -- which is what it used to cost, on every intermediate size GTK
delivered. Keeping the pixels is sound because a window resize moves no
character cell: the font is unchanged, so every cell keeps its pixel position
and only the number of rows and columns changes.

**Except when the character grid itself changes size.** The text on screen
was laid out against the old column count and the old bottom row, so it has
to be re-wrapped against the new ones. `ace_console_device_resize()` reads
`cu_XMax`/`cu_YMax` around the `Console_NewWindowSize()` call and repaints
the retained stream when either moved, clearing and homing first with a real
form feed because the replay has to start at the top left. Most steps of a
drag are smaller than one cell and change neither, so they stay free.

Shrinking would need the repaint in any case, for a second reason that is
real AROS code rather than ACE's. `console_newwindowsize()` clamps the cursor
into the new grid, and `stdcon_newwindowsize()` responds to a cursor that
moved by clearing the whole console and redrawing the cursor -- a
character-cell renderer with no retained character map has no way to tidy up
the cursor it left at the old position without wiping what it cannot redraw.
That is why shrinking made the text vanish once resize stopped replaying: the
old replay-everything path had been repainting what AROS had just erased,
without anyone noticing AROS was erasing it. Clamping only ever goes
downwards, so enlarging never hits that clear -- which is why the trigger
here is the grid and not the cursor. Testing the cursor made growing keep the
old narrow layout, with the text squashed into the top left of the wider
window.

A repaint replays a few screenfuls of the tail of the retained stream rather
than all of it (`replay_start()`). Anything older only scrolls off the top
again, and re-rendering it was most of what a repaint cost.

The bridge test covers both directions, and both checks are shaped around
things that hid the bug the first time. After a shrink it looks for ink in
the console's *upper half*: a whole-frame ink check passes on a console that
has been completely wiped, because the cursor block is itself non-background,
which is exactly why the original resize assertion did not catch the clear.
After a grow it writes lines long enough to wrap in a narrow console but not
a wide one, then looks for ink in the columns the console has just gained --
text squashed into the top left is precisely the absence of ink out there.
GTK
`size-allocate` coalesces those to about one frame, and `draw_console()` fills
whatever the window has gained with the console's background pen until the
console catches up. The drawing area is no longer fixed at the initial
`CONSOLE_WIDTH`/`CONSOLE_HEIGHT`. The selected font family, font size, and all
eight palette entries are written immediately to `$HOME/.config/ace.conf` and
loaded before the first RastPort is created. Also not implemented:
cursor blink (real console.device does not appear to
drive this from a timer either -- `stdcon_drawcursor()` only fires on
explicit `RenderCursor`/`UnRenderCursor` calls tied to command processing and
window-active state). DSR cursor-position-report replies are now returned:
The console input side now carries the replies that the imported AROS
`stdconclass.c` generates. That covers Vim's unchanged `__AROS__`
`CSI 0 q` window-size query (and the cursor-position report), without
putting a console task or message-port implementation into ACE. When Vim
enables raw event 12 with `CSI 12{`, GTK drawing-area resizes are delivered
back over the same stream in AROS's unchanged `CSI class;subclass;code;
qualifier;x;y;seconds;microseconds|` format. ACE also consumes a private
shell title message at the GUI boundary, so foreground commands can name
the ACE window without sending that private protocol into the AROS console
parser. Vim itself and the AROS sources remain untouched.

### Three answers ACE was giving that were not true

Running unmodified Vim through the live console turned up three places where
a seam answered a question confidently and wrongly. None of them looked like
a bug from the ACE side -- each seam did something reasonable -- and all
three showed up as the same complaint: Vim mostly works, but the screen
lags, and the window can be resized without Vim noticing.

**"Yes, there is a character waiting."** `WaitForChar()` counted the
synthetic argument line `ACE_COMMAND_ARGUMENTS` puts in front of a child's
cooked `Input()`. A raw reader never consumes that line -- deliberately, so
that a full-screen program does not read the shell's argument newline as its
first terminal byte -- so the count never went down and the answer was always
yes, without ever reaching the `select()` below it. A zero timeout is how a
program asks whether input is waiting *right now*: Vim's `char_avail()` asks
it before every screen update, and skips the update when the answer is yes,
because redrawing is pointless with keys still queued. So every repaint was
deferred until the *next* keypress, and the `Read()` that followed the false
yes blocked until one arrived. Pressing `i` appeared to do nothing; the
`-- INSERT --` for it arrived when the following key was pressed. The
argument line is now counted only in cooked mode.

**"Here is the console status you asked for."** ACE has no per-cell
character map -- `charmapconclass` is not in this profile -- so a repaint
re-renders the retained output stream. That stream still contains any status
query the program made, and re-rendering re-fired `con_inject()`, sending a
reply the program never asked for. Harmless anywhere except where it
actually happens: a grid change is what triggers a repaint, and a grid change
is also what makes Vim ask for the new console bounds. Vim's unchanged
`mch_get_shellsize()` writes `CSI 0 q` and then reads whatever arrives next,
so it read the stale replay reply, or the size report queued behind it,
`sscanf()` failed, and Vim concluded it was not on a console at all --
`term_console = FALSE`, 80x24, and no further resize handling for the rest of
the session. One resize was enough to disable resizing permanently. Replays
are now silent, which is the honest position: the question was asked once and
answered once.

**"The window changed size" -- repeatedly.** The same read makes the resize
report itself dangerous in quantity. A drag delivers a resize every frame,
and every report provokes a bounds query whose answer is read as the next
thing on the stream -- so a second report sent before the program asks
arrives in the answer's place, and a size report is not a valid answer. The
report is now sent once per character grid rather than once per pixel step,
which is also the only granularity a program laid out in cells can act on.

Two smaller ones from the same session: the console sent no Escape and no
Ctrl chord except `Ctrl-C`/`Ctrl-D`, because `gdk_keyval_to_unicode()` maps
Escape to nothing and the remaining Ctrl keys were swallowed; and closing the
window signalled only the shell, so a full-screen program the shell was
waiting on was left reading a console that could never produce another byte,
spinning at full CPU forever. The shell and everything it runs are now one
process group, and the console hangs up on the group.

### Which layer owns an assign, and what that costs ACE

The standard assigns are the clearest case of AmigaOS drawing a line that ACE
has to draw somewhere too. `rom/dos/cliinit.c` makes `SYS:` and six drawers
under it in C, before any shell exists, and `AddBootAssign()` falls back to
`SYS:` for a drawer that is missing so `LIBS:foo` always fails as a missing
file rather than a missing device. Everything else -- `T:`, `ENV:`, `CLIPS:`,
`FONTS:` ADD -- is `workbench/s/Startup-Sequence`, an editable script of
ordinary `Assign` commands run by the first CLI. The dividing line is
bootstrapping: the script layer is everything a shell can already do.

ACE keeps that line, with the broker as the boot layer, and moves exactly two
things across it. `T:` and `ENV:` are boot assigns here because AROS names
them `RAM:T` and `RAM:ENV` and ACE has no dependable `RAM:` to name -- tmpfs
mounts become `RAM:`, `RAM1:` ... in host mount order, so which one is the RAM
disk is a fact about the machine, not about ACE. They point into the host's
per-user runtime directory instead. And the Startup-Sequence's
`Copy ENVARC: ENV: ALL` is done by the broker, because ACE has no `Copy`.

Three commands were added to make a real Startup-Sequence possible, and the
interesting thing is which of them could be taken unmodified.

**`If`, `Else`, `EndIf` are AROS's own.** They looked like the hard case --
they change what the shell reads next -- but they do it by *consuming* the
script rather than by redirecting it: `SelectInput(cli_CurrentInput)`, then
`ReadItem()` and `FGetC()` until the matching `Else` or `EndIf`. A consumer
works across ACE's process boundary, because `fork()` shares the file
description and therefore the offset. So `RunCommand()` passes the script
descriptor to the child, `Cli()` presents it as `cli_CurrentInput`, and lines
the child eats are lines the shell will not see. The stream has to be
unbuffered on both sides or a child's read-ahead takes the shell's next
command into a buffer that dies with the child.

Two ACE bugs surfaced under that. `UnGetC(handle, -1)` -- AmigaDOS for "put
back the character just read", which `readitem.c` uses on the delimiter that
ended an item -- was casting `-1` to the byte `0xff` and pushing that back
instead, so the newline stayed consumed and a junk byte took its place. `If`'s
skip loop reads to the end of the line after finding its `EndIf`, so it ran on
and swallowed the line after the block: the symptom was a conditional that
worked and a command after it that silently vanished. And `IsInteractive()`
answered from the handle alone, which is right until a shell is started to run
one script and stop.

**`Execute` could not be.** AROS's works by assigning the opened script to
`cli_CurrentInput`, or by splicing the script and the unread remainder into a
temporary file and pointing `cli_CurrentInput` at that. Both are a command
redirecting the shell that started it, which is precisely what a separate
process cannot do -- and running it unmodified would be worse than doing
nothing, because copying the remainder consumes the shell's script to EOF and
the shell returns to an empty stream. ACE's writes the script into the
caller's own input file, ahead of the part not yet read: the same splice, made
in the file rather than in the CLI. With no script to splice into it runs a
nested shell in the same broker session, so what the script changes is still
changed afterwards.

The general shape: a command that *reads* shared state can be taken
unmodified, because ACE can hand it the same descriptor. A command that
*writes* the shell's own state cannot, because the write dies with the
process. `Path` is the next one on that side of the line -- it fills
`cli_CommandDir` -- which is why ACE has no `Path` yet and `C:` is doing the
whole job.

### Two ways ACE could take unmodified AROS code somewhere it cannot go

Both of these are the same shape: AROS source that is correct on the system
it was written for, reached by ACE with an input a real Amiga could not have
produced. Neither is fixable by changing AROS, and neither should be.

**A console smaller than one character cell hangs.** `consoleclass.c`
computes `cu_XMax`/`cu_YMax` as `(pixels / cell) - 1`, so a console narrower
or shorter than a single cell gets `-1` -- a grid with no columns or no rows
-- and the class chain then spins forever placing a character into it.
Confirmed by attaching to a hung process: the stack sits in AROS's own
`dispatch_consoleclass()`. `clamp_to_cell()` in `console_device_bridge.c`
guards every path that sizes a console. `replace_render_state()` had always
clamped; the point-resize path added for live resizing had not, which is
where this came in.

**A shell whose `Cli()` returns NULL crashes.** Real AmigaDOS gives every
shell process a CLI, so `Shell.c` and `cliPrompt.c` dereference `Cli()`
without checking -- correct for that system. On ACE the CLI is broker-backed,
and the broker is a separate process with a finite session table, so it can
fail where a real Amiga's could not; ACE's `Cli()` returned NULL and unmodified
`Shell.c` segfaulted at line 716, with five more unchecked calls inside its
command loop. `Cli()` now never returns NULL: it keeps the last state it read
successfully, or falls back to the same defaults the broker gives a new
session, and reports once on stderr. A full session table is now a
diagnosable degradation ("broker CLI state unavailable ... continuing with
defaults", then real errors from the commands) instead of a window that
vanishes.

### One broker per user

The broker used to default to a single machine-wide `/tmp/ace-broker.sock`,
which was wrong in both directions at once. Two users on one machine collided
on the same path, and since the socket is created 0600 the second one simply
could not use it. Meanwhile anything that pointed `ACE_BROKER_SOCKET` at a
private path -- every isolated test run -- got a whole additional broker
process, and they piled up: a day of testing left drifts of them behind.

The default is now `$XDG_RUNTIME_DIR/ace-broker.sock`, the per-user directory
made for this, which the system clears when the user's last session ends so a
socket cannot outlive its login. Where there is no runtime directory the uid
goes in the filename instead. `amiga_broker_socket_path()` in
`broker_protocol.h` is the single definition, shared by client and server so
they cannot disagree. `ACE_BROKER_SOCKET` still overrides, for a deliberately
isolated broker; that just is not what happens by accident any more.

**A second broker no longer displaces the first.** `main()` used to
`unlink()` the socket path unconditionally before binding, so starting
another broker silently stole the path from the live one and stranded it --
still running, still holding every session, on a socket nothing could reach.
A broker now holds an exclusive `flock` on `<socket>.lock` for its whole life
and exits if it is already held, so "one per socket" is enforced by the
kernel rather than by convention; the socket is only unlinked once the lock
is ours, which means any socket still on disk belongs to a broker that is
gone. The broker writes its pid into that lock file, which is how
`broker-stop` finds it without guessing from command lines, and `broker-start`
uses `flock -n` to detect a running broker and leave it alone rather than
restarting it and discarding its sessions.

**Sessions are reclaimed rather than exhausted.** Nothing tells the broker
that the shell behind a session has exited, so slots cannot be freed when
their owner goes away, and they used to simply run out: past 64 distinct
sessions every new one got `ENOSPC` and, before the `Cli()` fix above, every
ACE window opened from then on died on the spot. A broker meant to live as
long as the login has to cope with that, so a full table now gives up its
coldest slot instead of refusing. Every request stamps its session with an
ordinal and a live session is stamped constantly -- the shell reads its CLI
state to draw each prompt -- so the slot longest without a request is the best
available guess at one whose shell is gone. It is a guess: losing it costs a
session its current directory, assigns and variables, which is recoverable and
logged. Real ownership tracking would be better and still needs designing.

### The greyed-out menu items were labwc's, not ACE's

Worth recording, because it cost a lot of time and the symptom points the
wrong way. On a host whose labwc predates the fix below, ACE's exported menu
appears with **Typeface and Palette greyed and Quit active**. That looks
exactly like the provider race fixed in "Fix ACE appmenu provider lifecycle",
and it is tempting to go back into ACE's D-Bus export. Don't; check the
compositor's version first.

ACE's export is verifiable from the shell, and was correct throughout:

```sh
busctl --user list | grep ace-console          # find the connection, e.g. :1.858
P=/org/appmenu/gtk/window/menus/menubar/ace_shell
gdbus call --session --dest :1.858 --object-path $P \
    --method org.gtk.Menus.Start "[uint32 0, uint32 1]"
gdbus call --session --dest :1.858 --object-path $P \
    --method org.gtk.Actions.DescribeAll
```

`DescribeAll` returns `{'quit': (true…), 'typeface': (true…), 'palette':
(true…)}` -- all three present, all three enabled -- and the model carries the
right labels and `app.` action names. Nothing on ACE's side is disabled.

The cause is in labwc's appmenu consumer, fixed by `4e782e8` ("Fix appmenu
lifecycle and action readiness") and `90348bc` ("Wait for every remote appmenu
action") in the onscreen-windows tree. Its own comment names the mechanism,
including why Quit is the item that keeps working:

> GDBusActionGroup handles DescribeAll asynchronously and emits one
> action-added signal per dictionary entry. Merely checking that the action
> list is non-empty races with that sequence: if "quit" is the first entry, a
> snapshot taken from its signal marks every later action disabled.

`quit` is the first entry in ACE's dictionary, so a compositor without those
commits snapshots on it and disables `typeface` and `palette`. The fix belongs
in labwc; the only thing ACE could do about it is rename its actions so that
the alphabetically-or-insertion-first one is not the one that matters, which
would be superstition rather than engineering.

### The window's own two seams

`read_console()` drains the shell's socket in one pass per main-loop turn and
repaints once, rather than allocating a buffer and posting a `g_idle_add()`
callback for each 4KB the socket happened to deliver. The drain is capped at
`OUTPUT_DRAIN_MAX` so a program that prints without pause cannot hold the main
loop -- and with it the keyboard and the window -- for as long as it keeps
printing.

`ace_appmenu_wayland.c` puts ACE's Wayland objects on their own
`wl_event_queue`. The default queue belongs to GDK, and the previous code
called `wl_display_roundtrip()` on it from inside a GTK timeout that fires
four times a second: that both blocks and dispatches GDK's own event handlers
re-entrantly from inside a GTK callback, which is a plausible source of the
compositor-level instability this window had. A private queue keeps ACE's
round trip to ACE's own objects, and the periodic check is now a non-blocking
`wl_display_dispatch_queue_pending()` on that queue. The address is still
re-sent for a settle window after the surface appears -- a compositor may not
have built its view when ACE first offers one -- and then left alone, since a
compositor restart replaces GDK's whole display (caught by `ensure_manager()`)
and a manager coming or going is reported on the registry listener.

The next work is porting more AROS commands while keeping the AROS source
behavior intact. Every command in the tree now parses its arguments with
AROS's own `ReadArgs()`, so a new port needs no argument work at all: `Dir`
proves the real pattern engine and directory enumeration run on the broker's
host-path model, and `Delete`/`Protect` prove the same for removal and for
protection bits in both directions. What a port costs now is whatever DOS
calls it makes that ACE has not implemented yet -- `Filenote` needs
`SetComment()` and `Copy` needs that plus `Write()` and `SetFileDate()`. See
TODO.md.
