# ACE — AROS Command Environment

ACE is a Linux-hosted command environment built from a focused subset of AROS
shell and DOS behavior.
It intentionally does not build Workbench or the AROS kernel.  Original AROS
command sources are compiled against the compatibility headers in
`compat/include` and the broker-backed DOS implementation in `src`.

### ET — Edified Tine

ACE's Amiga ED-compatible editor is its maintained Tine fork, named ET —
Edified Tine. Its submotto is *Back in Tine.*

## Build and run

```sh
make
./build/ace-broker
```

The broker takes an optional socket path. With none, the name distinguishes
the originating user, authorization policy, view, SYS: root, and protocol
version. A normal session uses the host's existing mount tree. Start ACE with
`--root` when protected filesystem operations may request the separate root
mediator; the shell, console, and broker remain the logged-in user. Sudo is
tried noninteractively first, so a configured `sudo -n` path stays quiet;
otherwise polkit asks for authorization only when a protected operation first
needs it. Device view currently supports ext2, ext3, ext4, and vfat block
devices. Btrfs, procfs, sysfs, and other synthetic filesystems are not part of
this model yet.

In device view the broker creates a private mount namespace, makes the host
mount tree private, and non-recursively bind-mounts each already-mounted
supported filesystem at an ACE-managed root. A filesystem that is not mounted
is mounted there directly. ACE clients join that namespace before they touch
broker-resolved paths. A normal broker stop detaches the private roots; after
an abrupt exit Linux destroys the mounts when the last ACE process in that
namespace exits. The mountpoint directories themselves are harmless and are
reused on the next start.

Linux absolute symbolic links keep Linux meaning. If a link on `sda2:` names
`/boot/efi/file` and `/boot/efi` is `sda1:`, `ReadLink()` reports
`sda1:file`. Assigns likewise retain canonical DOS device paths, never Linux
mountpoint paths; use `ace-brokerctl name` first when starting from a Linux
path.

Install the built commands and console runtime:

```sh
make install
```

That is the whole install, and it needs no privileges. All installed ACE
programs share one directory -- `~/.local/lib/ace` -- because each finds its
companions -- the shell, the console, the broker, the AROS commands -- beside
its own executable. The desktop launcher is written with that directory's
absolute path in it, so the icon starts the build that was installed rather
than whatever PATH happens to find first.

`~/.local/bin` gets symlinks for the five entry points a person types at a
Linux prompt -- `ace-shell`, `ace-brokerctl`, `acepaste`, `broker-start` and
`broker-stop` -- and nothing else. `Copy`, `List`, `Type`, `Set`, `Run` and
`say` are commands inside ACE, reached by name through `C:`; on `PATH` they
only collided with host tools. An install over an older one removes the copies
that older one left in `~/.local/bin`, naming each as it goes.

### Say

`say` is an ACE command backed by a broker-owned Piper voice server. A normal
per-user `make install` installs Piper and the initial voice models alongside
ACE (a staged or system-wide install only installs the command files, because
it cannot choose a user's home directory):

```sh
say WARM            # preload the default voice
say hello there
say MALE "build finished"
```

The first `say` for a voice asks `ace-broker` to start it; later commands from
any shell using that broker reuse the warm model. The broker reaps voices
after two idle hours and terminates every voice server when it exits. Set
`ACE_SAY_IDLE_SECONDS` to a positive number to change that timeout (primarily
useful for testing). `ace-brokerctl say status` shows the broker-owned voices.
No systemd user units are installed or used.

A system-wide install is an ordinary `PREFIX` override rather than a target of
its own:

```sh
sudo make PREFIX=/usr/local POLKIT_ACTIONDIR=/usr/share/polkit-1/actions \
  AROS_ROOT="$HOME/aros" install
```

`AROS_ROOT` has to be passed explicitly there because `sudo` resets `$HOME`.
Prefer one install per machine: two sets of ACE binaries on one `PATH` drift
apart silently, since the older set keeps working perfectly well and only the
newer one stops being reached. `make install` warns when it has just installed
a copy that `PATH` will not select.

### The optional programs: Vim, Regina and LhA

Three programs ACE can run but does not own -- Vim, Regina and LhA -- are
built separately from the ordinary set, because two of them are large and one
arrives over the network. `make install` installs all three along with
everything else, so a full install is still one command; expect it to build
Vim, which is the slow part by a wide margin.

Each also has a build, install and clean target of its own, for working on
one without paying for the others:

```sh
make vim         make install-vim         make clean-vim
make regina      make install-regina      make clean-regina
make lha         make install-lha         make clean-lha
```

Vim and Regina build from source vendored under `third_party/`, so a plain
checkout builds them with nothing else to fetch. LhA is fetched instead --
its upstream ships release tarballs rather than a tree ACE tracks -- so
`make lha` downloads and checksums one into `build/`.

`make install-regina` puts the interpreter in `SYS:C` under both `rexx` and
`RX`, the name an Amiga user reaches for. `RX` is an alias for now: on AmigaOS
it does not run the script itself but sends it to the `REXX` port, which is a
difference that only shows once ports matter.

Each install target builds first if it needs to, so `make install-regina` on
its own is enough, and `make install` runs all three after installing ACE
itself. Each installs into the same directory as the rest of ACE
and symlinks the command into `SYS:C`. That shared directory is a
requirement, not tidiness: `build/rexx` finds `ace-user-shell` beside its own
executable, and a `rexx` installed anywhere else leaves `ADDRESS COMMAND`
silently doing nothing while reporting success.

To build against a different source tree instead of the vendored one:

```sh
make vim VIM_SRC=/path/to/vim
make regina REGINA_SRC=/path/to/aros-contrib/regina
```

Each clean target removes only what its own build produced, so cleaning one
does not cost a rebuild of the other two or of ACE itself; `make clean` remains
the blunt instrument that empties `build/` entirely. None of them touch
`third_party/`, which is source rather than output. `make clean-lha` also
discards the downloaded tarball, so `make clean-lha lha` re-fetches and
re-checksums it -- which is how to check that the download still works, not
just the compile.

`third_party/PROVENANCE.md` records where each tree came from, at which
commit, and under which licence. Both build entirely out of tree, so
`git status` under `third_party/` should never report a change from building.

Use `./build/ace-shell` when testing an uninstalled checkout; use `ace-shell`
only after `make install` and `make install-vim` have installed the matching
set of companions.

For testing, the project also provides quiet lifecycle commands:

```sh
source ./broker-start
source ./broker-stop
```

When sourced, `broker-start` also prepends `build/` to `PATH`, and
`broker-stop` removes that exact entry. This is necessary because an executed
child script cannot modify its parent shell's environment. Both commands also
work as ordinary executables for broker lifecycle control, but only the
source form changes `PATH`.

Both commands accept the optional `--root` authorization switch and honor
`ACE_BROKER_SOCKET`; an optional
`ACE_BROKER_PIDFILE` selects the PID-file location. If the socket
variable is unset, they use the same default as the DOS client.

In another terminal:

```sh
export ACE_SESSION=my-shell
./build/CD .
target=$(./build/ace-brokerctl name /tmp)
./build/ace-brokerctl assign WORK: "$target"
./build/ace-brokerctl doslist
./build/MakeDir WORK:ace-test-one WORK:ace-test-two ALL
./build/Echo hello from AROS TO WORK:ace-shell-test
./build/PathPart FILE Work:dir/file.txt
./build/PathPart DIR Work:dir/file.txt
./build/PathPart ADD Work: dir2 file.txt
./build/Fault 205 212
printf 'y\n' | ./build/Ask Continue?
```

`Peek` shows the path translation without opening or changing anything. With
no switch it resolves an AmigaDOS name to its full Linux path; `HOST` (or
`LINUX`) asks what AmigaDOS name ACE would assign to a Linux path:

```sh
./build/Peek SYS:C/Dir
./build/Peek SYS:C/#?
./build/Peek /home/erik/a:file HOST
```

The native commands also support AmigaDOS template mode. A standalone `?`
prints the command's argument template and reads the actual arguments from
the next input line:

```sh
printf 'FILE Work:dir/file.txt\n' | ./build/PathPart ?
# DIR/K,FILE/K,ADD/K/M: file.txt
```

Entering another `?` redisplays the template, as in AmigaDOS.

`MakeDir` is the first filesystem-mutating command ported from the original
AROS source. It supports multiple directory names and the `ALL` switch for
creating intermediate directories. Its DOS argument layer now supports
multi-valued `/M` arguments and explicit `FreeArgs()` cleanup.

Every ACE command now parses its arguments with AROS's own `ReadArgs()`.
Commands declare their arguments either by calling it directly or with the
`AROS_SHn` macros, and both routes are AROS's: the macros are expanded by
AROS's own `compiler/include/aros/shcommands.h`, which is where the
`ReadArgs()`/`FreeArgs()` pair around a command's body lives. ACE supplies only
the host process entry point that header assumes, in
`compat/include/ace_shcommand_host.h`.

AmigaDOS leaves a command's argument line in its input stream for `ReadArgs()`
to read, and ACE does the same: the shell puts the line it parsed there, and a
command started straight from a Linux shell gets its `argv` put back together
into one, quoted the way `readitem.c` will take it apart again.

`Delete` and `Protect` are the first destructive commands, both built from
their original AROS sources. `Delete` uses the real pattern matcher, deletes
directory trees with `ALL`, and refuses an object whose delete bit is
withdrawn unless `FORCE` is given; `Protect` is what withdraws it:

```sh
./build/Delete WORK:build/#?.o
./build/Delete WORK:scratch ALL
./build/Protect WORK:keep.txt d SUB
./build/Delete WORK:keep.txt          # refused
./build/Delete WORK:keep.txt FORCE    # clears protection, then deletes
```

`Filenote` attaches an AmigaDOS file comment, which is the one piece of Amiga
file metadata with no Unix field to hold it. ACE keeps it in the `user.comment`
extended attribute, so it lives on the inode and survives a rename or a move
within a filesystem, and `Examine()`/`ExNext()` read it back into
`fib_Comment`:

```sh
./build/Filenote WORK:notes.txt "second draft"
./build/Filenote WORK:notes.txt ""       # an empty comment clears it
```

A comment longer than the 79 characters a `FileInfoBlock` can carry is
refused rather than truncated, as on AmigaDOS. A filesystem with no extended
attributes at all -- VFAT has none, and ACE mounts VFAT -- reports
`ERROR_ACTION_NOT_KNOWN`, AmigaDOS's own answer for a handler that does not
implement an action.

`List` reads the same comment through the normal `FileInfoBlock` path. Use
`LFORMAT "%C"` when the comment is the field you want to display:

```sh
./build/List WORK:notes.txt LFORMAT "%C"
```

AmigaDOS's delete bit has no separate Unix permission -- on Unix it is the
containing directory that governs removal -- so it shares the owner write bit
with the write bit, which is the pairing ACE's `Examine()` already used in the
read direction. `Protect`'s other flags follow the same rule: the bits ACE can
express are the ones it maps, and the archive/pure/script trio is left
alone rather than guessed at.

`Dir` is now built from ACE's forked `src/dir.c` together with the real AROS
DOS pattern-matching and `ExAll()` sources. It exercises the
host filesystem seam through `Lock()`, `Examine()`, `ExNext()`, `DupLock()`,
and `CurrentDir()`, including recursive `ALL` listings and the `DIRS` and
`FILES` filters:

```sh
./build/Dir .
./build/Dir . ALL
./build/Dir src/#?.c FILES
```

AmigaDOS `#?` is the wildcard spelling used by the real AROS matcher. The
host seam deliberately leaves the optional `*`-as-wildcard root flag disabled,
so a bare `*` remains a literal pattern character, matching the configured
AmigaDOS behavior.

The broker owns per-session current directories and Assigns.  Its protocol is
deliberately small and binary; the DOS shim now has host filesystem
lock/enumeration and volume-label seams. `Relabel` uses `e2label` for ext2--4
and `fatlabel` for VFAT, while a tmpfs-backed synthetic `RAM:` volume gets a
live ACE-only label for the lifetime of the broker. Other filesystem types
return the AmigaDOS "action not known" error.

`Linux` is the explicit Linux escape hatch. It executes the named Linux program
directly with `execv()` and an explicit PATH search, passing the remaining
arguments unchanged; it never invokes a shell. In a live interactive ACE
console with unredirected input and output, the official `Linux` command starts
the Linux target on a private PTY, so interactive programs work as terminals:

    Linux /usr/bin/uname -a
    Linux printf hello
    Linux bash
    Linux bash --noprofile --norc -i

That PTY uses `TERM=xterm-256color`, receives the current ACE console size and
resize notifications, and translates ACE navigation keys to the corresponding
xterm input sequences. A bare `Linux bash` is therefore interactive: Bash sees a
controlling terminal, and Ctrl-C, Ctrl-D, Ctrl-Z, `jobs`, and `fg` have their
normal terminal meanings. Ctrl-D at an empty Linux prompt exits that target;
ordinary ACE script Ctrl-D remains the ACE shell's own script boundary. When
any Linux target exits, including one that reports a nonzero status, control
returns to the same ACE shell. Only an explicit ACE `EndCLI` ends that shell.

The two terminals are translated into each other by `src/terminal_translate.c`,
which is the only part of ACE that knows both vocabularies. xterm introduces
sequences with `ESC [`, names 256 colors and writes UTF-8; console.device
introduces sequences with the C1 byte `0x9B`, names one of eight pens and
reads Latin-1. Colors are mapped onto those pens -- ANSI, 256-color and
truecolor alike, with the bright colors carried by the console's bold flag --
and text is folded to Latin-1, with line drawing and the geometric shapes
`htop` and friends use given ASCII stand-ins. Commands the console lacks are
rebuilt from ones it has: erase-character becomes blanks and a cursor walk,
column and row addressing become a margin return and a bare row, and counted
insert/delete character become repeats of the console's single-cell forms.
Anything with no faithful equivalent -- scrolling regions, cursor save and
restore, window manipulation, mouse reporting -- is consumed here, because
console.device prints the sequences it does not recognize.

The PTY target remains an ordinary Linux process running as the logged-in
Linux user. It sees the host Linux filesystem and user permissions, not ACE's
AmigaDOS Assign/device view, and the PTY does not grant FMM/CRM privilege.
`Linux sudo bash` is still an explicit host-side privilege request subject to
the host's normal policy.

Any AmigaDOS redirection (`<`, `>`, or `>>`), a piped/scripted shell, separate
console endpoints, or a nonexportable `CON:` handle stays on Linux's direct
descriptor path. That preserves byte-exact noninteractive behavior; official
Linux invocations still give their Linux target `TERM=xterm-256color`, while a
standalone `./build/Linux` preserves the caller's TERM. There is no automatic
host-command fallback. An unrecognized command is an AmigaDOS command failure.
`Linux` is the deliberate mechanism for running a Linux command.

The broker now performs a read-only discovery pass over host block devices
when it starts. Filesystem-bearing devices are registered in the initial ACE
DOS device list with their kernel name, filesystem type, filesystem UUID,
label when valid, and `/dev` path:

```sh
./build/ace-brokerctl doslist
```

The reverse translation can be inspected directly with `name`:

```sh
./build/ace-brokerctl name /home/erik/repo/ace
```

The kernel name, UUID, and valid filesystem label are treated as
case-insensitive DOS aliases for the same filesystem volume. Labels may
contain spaces when quoted, but may not contain the DOS separator or host path
separators. Unformatted partitions are retained as device entries so they can
be named and inspected even though they cannot be mounted by the DOS volume
resolver. Swap, encrypted containers, RAID members, and other explicitly
non-filesystem media are not registered as file volumes.

### Linux names that AmigaDOS cannot spell

AmigaDOS takes very nearly every name Linux can produce. Spaces, `+`, `#`,
`*`, quotes and non-ASCII bytes are ordinary filename characters; the pattern
metacharacters among them need quoting when they appear in a pattern, which is
equally true on a real Amiga and is not a reason to rename anyone's files. So
`my file.txt`, `report(final)#1.txt` and `Grüße.txt` are left exactly as they
are and open under their own names.

Four things genuinely cannot be spelled, and only these reach the mapper:

* `:`, which separates a volume from the path after it;
* `/`, which separates components -- a Linux filename can never contain one,
  but the rule is stated for completeness;
* `^`, but only when what follows it really does spell an encoded payload.
  `a^b.txt` and `caret^symbol` are left alone, because nothing after the caret
  decodes; only a caret that would be mistaken for a marker has to be escaped;
* names too long for a `FileInfoBlock` component, and names that differ from
  another name in the same directory only by case, which a case-insensitive
  AmigaDOS filesystem regards as one file.

Those get a spelling of the form `header^base32`:

```text
Hi:There.txt  ->  Hi^HJKGQZLSMUXHI6DUAA
a^b.txt       ->  a^LZRC45DYOQAA
notes.txt     ->  ^NZXXIZLTFZ2HQ5AA      (next to Notes.txt)
report.txt    ->  repor^OQXHI6DUAA       (next to reporT.txt)
```

The escape is a pure function of the host name. Nothing is stored, so a name
means the same thing in every broker, on every machine, and after every
restart; there is no dictionary to persist, migrate, or garbage-collect.

Everything before the `^` is literal, and the split falls as late as it can:
at the first byte that cannot be spelled, or for a case collision at the first
byte where the colliding names differ. Of a set of colliding names the
lexically first keeps its plain spelling and the rest are escaped.

base32 rather than base64 because the filesystem is case-insensitive, so an
alphabet distinguishing `A` from `a` would let two encodings name one file.

Deciding whether a caret opens an escape takes more than "the rest is base32".
`x^AAAA` is valid base32 and decodes to two zero bytes, which would read as a
host name containing an embedded NUL -- something no filesystem can hold. So
the decoded bytes must spell one of the two forms ACE actually emits, told
apart by their last byte:

* `0x00` -- literal: the rest of the host name, and no other NUL before it.
  Decoding reconstructs the name without touching the disk.
* `0x80` / `0x81` -- compressed: see below. The engine's own decode must
  succeed, not merely look plausible.

Anything else is not a payload ACE could have produced, so the caret is just
a caret. Both directions ask this same question, so whatever `map_component`
declines to escape, `unmap_component` declines to decode, and every name
survives the round trip.

### When the literal escape still doesn't fit

106 base32 characters carry 530 bits, and a name may be up to `NAME_MAX`
bytes -- there is no encoding that maps every name into that space, because
there are more possible names than payloads. Rather than refuse outright,
the tail `component_split_point()` already chose to keep -- not the whole
name, which was tried and measured worse, since header bytes are already
free (one raw character each) and compression's per-string overhead usually
costs more than a few already-short bytes can save -- gets one more chance:
compressed with whichever of two engines does better.

* **PACK39** -- direct radix-39 bit-packing of `[a-z0-9_.-]`. No header, no
  per-block overhead, which is what lets it beat general compression on
  short strings. Build-system-generated names are almost always made of
  exactly this alphabet.
* **DEFLATE** -- raw DEFLATE (zlib, no wrapper) for anything outside it:
  mixed case, accented bytes, punctuation the direct pack does not cover.

Two engines, chosen by one bit. An earlier version of this idea carried
four engines and reserved a sixteen-symbol header alphabet to tell them
apart; measured against the real corpus below, the two here alone already
reach the same result the four did, so the other two -- and the header
space spent naming four -- were paying for nothing.

The compressed bytes are not spelled in base32. Base32 is what the literal
form has always used, and stays that way -- changing it would touch every
`:` and every case collision ACE has ever escaped, not just this rare tail
-- but its 5-bits-per-character cost was the actual ceiling on this tier: 106
base32 characters buy only about 65 compressed bytes, and names that
compressed to more than that were refused for want of encoding, not for
want of compression. So the compressed tail gets its own alphabet, DENSE128,
at 7 bits/character -- 128 symbols, chosen the same way base32's were: no
two of them differing only by AmigaDOS's case fold. Lowercase ASCII is
excluded entirely, and so is every Latin-1 byte that has one; what is left
is ASCII punctuation and uppercase letters, Latin-1 symbols, and Latin-1
uppercase-or-caseless letters.

Telling DENSE128 apart from base32 does not lean on probability the way
telling literal apart from compressed within base32 does. `^0` is reserved,
structurally: RFC 4648 excludes `0`/`1`/`8`/`9` from base32's own alphabet
to keep them visually distinct from `O`/`I`/`B`/`S`, so a base32 literal can
never begin with `0`. `^0<DENSE128 payload>` is therefore unambiguous by
construction, the same way `^` itself is reserved to open an escape at all.

A name that compression still cannot fit is **refused**, not given a
synthetic spelling that only half-works. A high-entropy hash-style name (39
hex digits pack into roughly 0.66 bytes per character regardless of how
"random" they look -- PACK39 is a positional radix conversion, not an
entropy coder) can still land past the roughly 91-byte compressed budget 105
DENSE128 characters buy.

So the mapper fails predictably instead of on a subset nobody can
characterise. An unspellable name reports `ERROR_INVALID_COMPONENT_NAME`, and
because a directory listing has to name every entry, one such file makes its
directory unlistable rather than silently omitting it.

Measured against the full Debian 12 (bookworm) package archive's file index
-- `Contents-amd64.gz`, 913,356 unique basenames, fetched and tested
independently -- 258 basenames exceed `AMIGA_COMPONENT_LIMIT` outright. Every
one of ACE's own commands re-derives its own compressed form and round-trips
correctly, with zero of the 258 producing the wrong name. **159 of the 258
are rescued by compression alone** -- the comment field is not used for this,
and deliberately so: it carries the risk that plain `Copy` and LhA do not
preserve it, only `Copy CLONE` does, and nothing stops a later `Filenote`
from overwriting a slot the encoder was relying on. The remaining 99 are
genuinely too high-entropy for either engine at this budget -- mostly hash
digests -- and are refused the same way an unrescuable name always was. On
this host's own live filesystem -- a full Debian install plus 28GB of
working data, 460,946 directory entries -- no name exceeded 107 bytes and
the longest was 90, so this tier exists for corpora shaped like a package
archive, not for what ordinarily ends up on a running system.

The first filesystem handler supports VFAT and ext2, ext3, and ext4. The
broker also enumerates the live Linux mount table. Block-backed mounts are
attached to their existing DOS volume entries, while filesystems without a
block device receive synthetic names (for example `RAM:` for tmpfs). On the
first use of an unmounted supported alias, the broker finds an existing mount
or asks Linux to mount it on demand. The host mountpoint is an
implementation detail: sda2:etc/hosts means etc/hosts below the root of the
filesystem on sda2, even if Linux happens to mount that filesystem at /. When
an AROS lock is converted back into a name, ACE chooses the longest matching
mountpoint and strips it, so nested mounts remain independent. ACE releases
mounts it created when the broker stops.

The first local Exec compatibility layer is now in `src/exec_compat.[ch]`.
It provides host-backed memory, registered tasks, Amiga signal-bit masks,
message ports, and case-insensitive library/device registries. Signals are
implemented with mutexes and condition variables rather than Unix signal
numbers. The focused test is run with:

```sh
make test-exec-compat
```

This layer is intentionally local to the runtime; the broker remains the
shared AmigaDOS/session authority. The AROS Shell will use this layer for its
Exec calls and the broker-backed DOS shim for DOS state.

`PathPart` is currently filesystem-independent.  It uses the original AROS
command source and native AmigaDOS-style `FilePart`, `PathPart`, and `AddPart`
implementations, so the paths it prints do not need to exist on Linux.

`Fault` and `Ask` are also built from their original AROS command sources.
`Fault` uses Amiga `/N/M` numeric-list argument handling, and `Ask` uses the
native console stream plus the `utility.library` compatibility surface.

The broker's first CLI-state draft provides session-local and broker-global
variables through `GetVar`, `SetVar`, and `DeleteVar`, plus a per-session
return-code/result2 record. `ace-brokerctl` can exercise these directly:

```sh
./build/ace-brokerctl setvar LOCAL value
./build/ace-brokerctl setgvar GLOBAL value
./build/ace-brokerctl getvar LOCAL
./build/ace-brokerctl setresult 10 205
./build/ace-brokerctl result
```

The original AROS `Set`, `Unset`, `Alias`, and `Unalias` commands are now
also built. Their local-variable lists are backed by the broker, and aliases
are kept separate from ordinary variables, as they are in AmigaDOS:

```sh
Set SCORE 5
Set
Alias HI Echo []
Alias
Get SCORE
Unalias HI
Unset SCORE
```

Alias expansion by the future shell command dispatcher is not implemented
yet; these commands currently provide the AmigaDOS alias state and its
inspection/update behavior.

The first CLI-lifecycle commands are also available. `FailAt` and `Prompt`
update broker-owned CLI state, while `Why` reads the previous command's
secondary result:

```sh
FailAt
FailAt 5
Prompt 'AMIGA> '
Get MISSING
Why
./build/ace-brokerctl cli
```

The diagnostic `cli` output is four lines: return code, `Result2`, fail
level, and prompt. A real interactive AmigaDOS-compatible command dispatcher
will consume this state in the next shell layer.

The first classic-console shell slice is now available. It uses a GTK drawing
surface as the Linux console, a full-duplex Unix stream for the
child CLI, and a cloned broker session. The surface starts with the classic
eight-pen palette and renders a first classic terminal subset: Amiga/ANSI CSI
cursor movement, colors, text attributes, erasing, tabs, scrolling, and local
line editing.

The live window forwards keyboard bytes directly to the child. The child-side
DOS seam feeds those bytes into AROS's real console-handler editing path from
`rom/filesys/console_handler/support.c`, and writes the handler's `do_write()`
echo back through the same output stream. That placement is essential: when a
program such as Vim takes raw mode, it receives the keystrokes itself instead
of waiting for the shell's editor to produce a complete line. Piped/scripted
sessions retain their direct stream behavior; only the GUI-launched session
enables cooked editing.

`src/console_device.[ch]` remains the smaller standalone console-device test
seam. The real-handler bridge is in `src/aros_console_editor.[ch]`; it is the
staging point for the remaining DOS packet and task integration. AROS
Workbench integration, clipboard, and packet/task ABI remain deliberately outside
this profile.

```sh
source ./broker-start
export ACE_SESSION=main-shell
./build/ace-shell
```

With a Vim build in the checkout, type `vim` in the ACE Shell window. With an
installed Vim, `make install-vim` places the executable and its `runtime/`
directory beside the installed ACE shell.

Running `ace-shell` opens a separate console window and returns to
the launching terminal. The window runs the original AROS `Shell.c` through
the installed `ace-user-shell` binary.

The real shell starts the sibling `ace-broker` only when the configured
`ACE_BROKER_SOCKET` is unreachable, waits for it to accept connections, and
reuses an existing broker. The broker exits half an hour after its last shell
detaches -- long enough that the next window reuses it, short enough that an
idle one does not outlast the day -- and the startup lock prevents concurrent
shells from starting duplicates. Set `ACE_BROKER_BINARY` only when the broker
is not beside `ace-user-shell`.

Inside that shell, command parsing, prompting, redirection, aliases, and
command errors are handled by the original AROS Shell code. At its command
loading boundary, ACE implements AROS `LoadSeg()`/`RunCommand()` with direct
`fork()`/`exec()` for ACE/AROS commands; it does not search the host `PATH`.
There is no automatic host-command fallback. `EndCLI` is compiled from the original
AROS command source and terminates the current real AROS shell, including a shell
running in an ACE window. `NewCLI` opens a separate window and starts
another real AROS shell with a cloned initial session:

```text
AMIGA> SET FOO parent
AMIGA> NEWCLI
```

`build/EndCLI` is compiled from the original AROS `EndCLI.c`; its CLI state change
is carried across the host process boundary by the ACE DOS bridge. `build/NewCLI`
is compiled from the original AROS `NewCLI.c` (which includes
`NewShell.c`).  Its unchanged `Open("CON:...")` and `SystemTagList()` calls
are currently backed by the host compatibility layer; the compatibility
layer launches the ACE console and clones the broker session.

The ACE Shell window has a GTK menu with typeface and eight-pen palette
dialogs. The console retains a bounded tail of its output stream, so applying
a new typeface or palette rebuilds the AROS console and repaints it
immediately. The mouse wheel opens modal scrollback: the live console keeps
processing output in the background while a separate historical render stays
at the selected line offset. A top-of-console overlay reports the distance
back, and any key returns to the live view; output by itself does not. The
drawing-area size allocation follows window resizes and
updates the real AROS window geometry. A resize smaller than one character
cell keeps the pixels already on screen rather than re-rendering anything,
and the console's background pen fills whatever the window has gained until
the console catches up. A resize that does change the character grid repaints
the retained stream, re-wrapping the text to the new width and putting the
last line back on the last row, in both directions. When the
installed appmenu GTK module and compositor support it, ACE exports the menu
model and actions over D-Bus and advertises that address to the compositor's
Wayland appmenu interface for the desktop/right-click menu; the local menu bar
is hidden while that advertisement is live and returns if it is lost.
Otherwise the same menu remains below the title bar.
AROS Workbench and clipboard extensions remain outside this profile.

The shell also supplies the terminal contracts used by unchanged Amiga
programs: console status replies are returned to the program's input stream,
and a program that enables AROS raw resize event 12 receives the original
console-device event format whenever the ACE window is resized -- once per
character grid, since that is the granularity a console program is laid out
in and a second report would be read in place of the answer to the bounds
query the first one provokes. Redrawing the console after a resize replays
the retained output stream, and a replay is silent: a status query in that
stream was answered when the program made it, and answering it again would
put a reply into the program's input that it never asked for. Foreground
commands update the ACE title bar through an ACE-private shell boundary; the
private bytes are removed before the remaining output reaches the AROS
console parser.

The keyboard carries what a full-screen program needs: Escape, every Ctrl
chord, the arrows and their shifted forms, the 101-key block, and F1-F10, all
as the console.device sequences an Amiga program's own key table is written
against. Closing the window hangs up on the shell and everything it is
running, so a program the shell is waiting on is not left reading a console
that no longer exists. While in scrollback, Ctrl-C copies the retained output
to the Linux clipboard, or copies a mouse-selected region; the selection is
disabled in the live console. Ctrl-V pastes the Linux clipboard into the shell
from either mode, and copy operations briefly report their character count at
the bottom of the console. F12 always copies the full retained output and
returns to the live console when scrollback was active.

## Edit, the line editor

`Edit` is the one command here with no AROS source behind it. AmigaDOS shipped
two editors -- ED, the full-screen one, and EDIT, the line editor -- and only
the second is a command in the sense the rest of `SYS:C` is: it edits a file
from a script of one-line commands, in a forward pass, so a file larger than
memory can still be edited. AROS never had it, so `src/edit.c` is written to
the AmigaDOS manual's description rather than ported from anything.

It is written to `dos.library` and `exec.library` alone -- no stdio, no
`malloc`, no host calls -- so the same file compiles for AmigaOS and for AROS
as readily as for ACE, where it enters through the same seam as any other
command that keeps its own `main()` and calls `ReadArgs()` itself.

```sh
./build/Edit WORK:notes.txt WITH WORK:notes.ed
printf 'M2\nE/old/new/\nM*\nW\n' | ./build/Edit WORK:notes.txt
```

The source file passes through a queue of previous lines on its way to the
destination, and `PREVIOUS` and `WIDTH` size that queue exactly as the manual
says: `PREVIOUS` lines of `WIDTH` characters is the memory it uses, and the
number of lines `P` can move back over. `WIDTH` does not limit a line, though
-- the original shows and writes a line longer than it whole, and so does
this. Trailing blanks are dropped and a last line with no line feed does not
acquire one, so an edit that changes nothing writes the file back byte for
byte.

With no `TO` file the editing goes to a work file in `T:`, named the way the
original names it -- `T:E<nn>-WK<n>`, with the process number in it and the
`WK` number distinguishing the second one a `REWIND` needs while the first is
still being read. `W` moves it into place and keeps the source as
`T:EDIT-BACKUP`, so a session that fails partway through has not touched the
original. Because `T:` is rarely on the same device as the file being edited
and AmigaDOS cannot rename across devices, a move that fails is retried as a
copy. `STOP` throws the edit away, writing the lines it had already passed to
the work file and leaving it in `T:` -- the original does not clean up after itself, and this is
a work-alike, so the two `WK` names are reused and overwritten by the next
edit in the same process rather than probed for a free one. `REWIND`
closes the destination, reopens it as the source, and starts a second pass,
which is what turns inserted lines into numbered original ones.

The screen behaviour is the original's rather than anything invented here,
because it was checked against AmigaDOS's own EDIT running under emulation.
A line verifies as two lines -- its number, then its text:

```
3.
three cat cat
```

The number is `+++` for a line that has none of its own because it was
inserted or split, and the terminator is `*` rather than `.` on the extra line
past the end of the file, which is numbered as though it were the next one.
Verification is deferred to the end of a whole command line and happens only
if nothing has shown the line already, so `M2;M3` prints one line, `3(N)`
prints only the line it arrives at, and `2(N;?)` prints two lines rather than
four. `n(...)` is a repeat group, with `;` between the commands inside it.
Errors name their place before they name themselves, with a `>` under the
character of the command line the editor had reached.

Two places where the manual contradicts itself are settled, and the real
program was the arbiter for both: the first string of `A`, `B`, `E` and their
global forms is always the one searched for -- chaining them turns `one cat`
into `one YdogX` on the Amiga and here alike -- and a global change acts on
every occurrence in a line where the single-line commands act on the first.
The string qualifiers the manual mentions but never defines are not
implemented, and are reported as unknown commands rather than ignored. Two
commands turned out to be spelled differently than the manual says: a global
is suspended with `DG`, not `SG`, and the manual's `D.*` and `I.*` are `D*`
and `I*`. The insert terminator cannot be changed at all.

`make test-edit` runs the editor against a broker of its own. Many of its
cases are transcripts taken from the original under emulation, replayed and
compared byte for byte.

The later ones were not typed by hand. `tools/amiga-edit-chassis` is an
AmigaDOS script that polls a drawer the emulator shares with the host, runs
whatever it finds staged there, and leaves the answer behind;
`tools/amiga-edit-drive.sh` stages one EDIT run from this side and waits for
the answer, and `tools/amiga-dir-drive.sh` does the same for `Dir`. One run
takes a second or two, which turned checking a clone against the real program
from a typing exercise into something that could be done a hundred times.
Most of what `Edit` gets right, it gets right because that loop found it
wrong first -- and the same loop, pointed at `Dir`, found a real defect in
`Dir` too: see `src/dir.c` and `make test-dir-sort`.

## The standard assigns, and who makes them

An Amiga makes its standard assigns in two layers, and ACE keeps the split.

The first layer is not a script and cannot be, because a shell cannot find a
script, or the `Assign` that would make an assign, until it exists.
`internalBootCliHandler()` in AROS's `rom/dos/cliinit.c` locks the boot volume
as `SYS:` and then makes `C:`, `LIBS:`, `DEVS:`, `L:`, `S:` and `FONTS:` as
drawers under it, each falling back to `SYS:` itself when the drawer is not
there. ACE's broker does the same job in the same order, and adds `ENVARC:`,
`ENV:` and `T:`. `SYS:` is the install's own `share/ace`, laid out the Amiga
way: `SYS:C` is a drawer of links to the commands, which live in `bin` where a
Linux user's PATH can also reach them, so both views are true and neither is a
copy. `ENV:` and `T:` are in the host's per-user runtime directory rather than
in `RAM:`, because ACE names tmpfs mounts `RAM:`, `RAM1:` ... in host mount
order and no script could portably name the one it meant.

The second layer is `S:Startup-Sequence`, an ordinary script of ordinary
commands, and it is meant to be edited. A starting shell runs
`S:Startup-Sequence`, then `S:Shell-Startup`, then `S:ACE-Startup`, skipping
whichever are absent; the supplied Startup-Sequence ends by running
`S:User-Startup` if it exists, which is the file an install does not
overwrite. The shell reads them as `cli_CurrentInput`, which is the same
mechanism `rom/dos/boot.c` uses to hand the boot script to the first CLI: at
the end of the script the Shell closes it, falls back to the keyboard and
carries on.

`C:` is not a search path. AROS's `loadCommand()` looks in the current
directory, then the list the `Path` command sets, and only then in the `C:`
multiassign, which is why `C:` finds everything without being consulted first.
ACE runs that search unmodified. `Path` stores its directory locks in the
broker-backed shell session, so a command running in a separate process can
change the list and the next shell lookup sees it:

```text
Path SYS:Tools ADD
Path SYS:Tools HEAD
Path SYS:Tools REMOVE
Path SHOW
```

A failed `Open()` of a data file still reports a missing file rather than
quietly finding a command with that name.

`Which` uses the same current-directory, `Path`, and `C:` search order and
prints the location of a matching executable. It returns a warning without
printing an error when no match exists, as AmigaDOS does.

`If`, `Else` and `EndIf` are AROS's own, unmodified. They skip a block by
reading the script and consuming the lines themselves, and an ACE command is a
separate process holding the same descriptor as the shell, so consuming those
lines moves the shell's position exactly as it does on the machine they were
written for. `Execute` is ACE's, because AROS's works by redirecting the
shell's input from inside the command, which no separate process can do:
running inside a script it writes the new script into the caller's own input
ahead of the unread part, and typed at a prompt it runs the script in a nested
shell in the same session, so directory changes and variables still persist.

`Skip`, `Lab` and `EndSkip` provide the older AmigaDOS script-jump form. `Skip`
searches the shared script input for a matching `Lab`, or for the next
`EndSkip` when no label is given. `Quit` is the script-only command from
AmigaDOS: it stops the current script and returns zero or the optional `RC`
value. It is deliberately not an interactive synonym for `EndCLI` or
`EndShell`.

Global variables are files, as they are on an Amiga: one per variable in
`ENV:`, and in `ENVARC:` too when `Setenv` is given `SAVE`. `Type ENV:Editor`
prints one, `Dir ENV:` lists them, and the broker copies `ENVARC:` into `ENV:`
when it starts, which is that system's boot-time `Copy`. Local variables stay
in the broker: on AmigaOS they live on the shell process's own list, which
every command shares by being that process, and ACE's commands are not.

## BOOPSI

The real AROS BOOPSI implementation is now built from unmodified AROS source:
`rom/intuition/{rootclass,makeclass,freeclass,addclass,removeclass,findclass,
newobjecta,disposeobject,setattrsa,getattr,nextobject}.c` and the amiga.lib
method dispatchers in `compiler/alib/{domethod,dosupermethod,coercemethod,
alib_util}.c`. That is roughly 1700 lines of AROS against 510 lines of ACE
seam, and unlike the console handler it needs no patch at all — the AROS
working tree is untouched by this build.

BOOPSI is Commodore's, introduced with AmigaOS 2.0 and documented in the ROM
Kernel Reference Manual: Libraries. AROS additionally uses it as the internal
architecture of `console.device`, so the console classes ACE is working toward
require it. It is also what `gadgetclass`, `imageclass`, `windowclasses` and
`screenclass` are built from, should ACE ever grow a real Intuition layer.

ACE supplies only what a real AROS build would generate or configure:

* `compat/aros-real/include/aros/libcall.h` turns an AROS library entry point
  into a plain C function. On AmigaOS the arguments arrive in named registers
  and the library base arrives with the call; on the host they are ordinary
  parameters and the base is a file-scope object.
* `compat/aros-real/include/aros/asmcall.h` does the same for hook and user
  functions, and supplies the `AROS_UFC*` forms that `CALLHOOKPKT()` in AROS's
  own `utility/hooks.h` is built from. That macro is where every BOOPSI method
  dispatch crosses the calling-convention boundary.
* `compat/aros-real/include/aros/atomic.h` maps the class reference counts onto
  the compiler's atomic builtins.
* `compat/aros-real/include/ace_boopsi_intern.h` is force-included ahead of the
  AROS sources and claims the include guard of
  `rom/intuition/intuition_intern.h`, which is the private header of the entire
  Intuition library. BOOPSI needs three fields out of its 1500 lines: the class
  list, that list's lock, and the rootclass.
* `src/aros_boopsi_runtime.c` is the Exec seam — memory, memory pools,
  recursive semaphores and list handling — plus the one-time rootclass
  bootstrap that `rom/intuition/intuition_init.c` performs on a real AROS
  build.

Whether the varargs method calls read their arguments straight off the stack or
marshal them through `GetMsgFromStack()` is left to `AROS_SLOWSTACKMETHODS` in
AROS's own `arch/<cpu>/include/aros/cpu.h`. AROS sets it for exactly those
architectures whose ABI does not pass varargs contiguously, so aarch64 and
x86_64 hosts get the marshalling path and i386 does not.

The focused test builds a two-level class hierarchy through the real
`MakeClass()` and checks AROS's dispatch, its instance-data layout, its
reference counting and its teardown refusals:

```sh
make test-boopsi
```

## graphics.library

`stdconclass.c`, `consoleclass.c`, and `support.c` from `rom/devs/console` --
the real AROS BOOPSI classes that touch pixels for `console.device`, plus the
real ANSI/CSI escape-sequence parser (`writeToConsole()`) -- are compiled
unmodified against `src/aros_graphics_runtime.c`. Unlike every other seam in
ACE, this one is authored rather than compiled from AROS source:
graphics.library is the real hardware boundary, the point where AmigaOS
drawing calls become pixels on a display, and the alternative to writing it
is a HIDD driver stack ACE does not want.

This is now what actually renders the live `ace-console` window.
`src/console_device_bridge.c` builds one real `ConUnit` per window and
`src/amiga_console.c`'s output path calls `writeToConsole()` on it directly --
the same call `console.c`'s real `beginio()`/`CMD_WRITE` would make, taken
without console.c's task/message-port machinery because ACE's architecture
never routes rendering through `DoIO()` (see HANDOFF.md for the full trace).
`console_device_bridge.c` exists as a separate translation unit because
AROS's real headers and GTK/glib's cannot coexist in one file (both define
`struct timeval`, and `console_gcc.h`'s `MAX`/`MIN` collide with glib's);
`amiga_console.c` only ever sees its opaque `struct ace_console_device`.

What AROS's console classes actually require of a `RastPort`/`BitMap`/
`TextFont` is narrow, confirmed by reading every call site rather than
guessing: ten drawing calls (`Move`, `Text`, `SetAPen`, `SetBPen`, `SetDrMd`,
`SetABPenDrMd`, `RectFill`, `ScrollRaster`, `SetSoftStyle`, plus the
`AllocRaster`/`FreeRaster`/`InitTmpRas` scratch-buffer trio for the
character-cell cursor), and from a font, exactly three scalars --
`tf_XSize`, `tf_YSize`, `tf_Baseline` -- to lay out a fixed character-cell
grid ("For now one should use only non-proportional fonts", in
`consoleclass.c`'s own words). Nothing in this codepath ever reads a font's
glyph bitmap data.

That gap is where ACE improves on real Amiga hardware rather than emulating
it: glyphs are rendered from a host TrueType font via cairo/fontconfig
instead of a bitmap font ACE would have to draw and ship, and
`SetSoftStyle()`'s bold/italic requests select the font's own real bold/
italic faces rather than being synthesized by smearing or shearing a single
face the way real Amiga hardware did. `ace_gfx_font_family_complete()`
enforces that a chosen family actually has all four faces (regular, bold,
italic, bold-italic) before it can be used. Underline is drawn as a rule
below the baseline either way, since it is a soft-style flag on real
hardware too, never a font glyph.

The focused test builds a real `ConUnit` through `NewObjectA()` on the real
class chain and drives it two ways: AROS's own `Console_DoCommand()` macro
directly, and `writeToConsole()` with a raw CSI byte sequence -- the latter
is what actually exercises the escape-sequence *parser*, since
`Console_DoCommand()` alone dispatches an already-parsed command. Both read
the resulting pixels back to confirm:

```sh
make test-graphics test-console-device-bridge
```

The bridge test also writes retained output, changes all eight palette slots,
resizes the backing surface both smaller and larger, and changes the typeface,
checking that the repainted surface keeps both its content and its new
background. It scrolls far enough to exhaust the surface's scroll headroom
several times over and then scrolls that content back off the top, which
catches a scroll that fails to move what is on screen or fails to clear what
it uncovers, and it checks that the console reports the region it drew into.

This needs `cairo` and `fontconfig` development files, and at least one
complete monospace family on the host; `Liberation Mono`, `DejaVu Sans Mono`,
and the `monospace` fontconfig alias are tried in that order, both for the
test and for the live `ace-console` window. The ACE Shell menu offers a
monospace typeface chooser and eight color slots, validated through the same
font-loading path as startup. The selected font family, font size, and eight
palette entries are saved immediately to `$HOME/.config/ace.conf` and loaded
on the next start. See HANDOFF.md for what is still not real (keyboard input,
cursor blink).

Global variables currently last only for the lifetime of the broker. The
`SAVE`/`ENVARC:` persistence behavior is still reserved for a later draft.
Every command built through the native AROS shell-command wrapper now
publishes its return code and `IoErr()` as the session's result record.
