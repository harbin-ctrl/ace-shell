# ACE security FMM/CRM service redesign handoff

This document is a handoff for a future AI or engineer continuing ACE work.
It records the architectural decision reached in discussion with the user and
the implementation method expected for the next phase. It deliberately
supersedes the older visible root/user and mountview/deviceview model, but it
does not erase the useful implementation work already present in the tree.

Read this document together with the existing `HANDOFF.md`, `README.md`, the
current `Makefile`, and the actual source tree. The source tree is authoritative
for what exists today; this document is authoritative for the intended security
direction.

## Core product idea

ACE is an Amiga-like user interface for a Linux machine. The person using ACE
should not have to understand Linux ownership, root as a login identity, or
mountpoint topology in order to use normal AmigaDOS commands. ACE should feel
like an Amiga environment, while Linux remains visible through the explicit
`LNX` escape hatch.

The shell and broker are user-facing programs and must run as the logged-in
user. They must use that user's desktop session, D-Bus session bus, Wayland or
X11 connection, `HOME`, terminal settings, fonts, colors, and configuration.

ACE must not solve privilege by running the shell, console, or broker as UID 0.
`sudo ace-shell`, `ace-shell --root` in the old sense, and a root-owned GUI are
the wrong architecture. A root process has a different Unix identity and
therefore different desktop/session state; copying the user's environment into
it does not make it the user's D-Bus peer or configuration owner.

The user may still ask ACE to perform operations that require administrator
power. That power is delegated to a privileged FMM/CRM service and is requested only
when needed.

## The agreed process model

The model is a root supervisor with one worker per kind of privilege, all
reached through the user's broker:

```text
  ace-shell / ace-console       ordinary user, talks only to the broker
           |
           v
      ace-broker                ordinary user; owns env, cwd, assigns,
           |                    aliases, path translation, sessions
           |
           +---- authenticated once ----> root supervisor          root
           |                              creates the private mount namespace,
           |                              forks and reaps, routes by class,
           |                              performs no privileged operation
           |                                     |
           |          private channel, volume ---+--- forks, in that namespace
           |          records only         |     |
           |                               v     v
           |                        volume worker    CRM        root
           |                        the namespace's  one typed file
           |                        mounts and the   operation at a time,
           |                        device view      returns descriptors
           |                                               ^
           +---- second channel ---------------------------+
```

The supervisor is the only root peer the broker ever authenticates, and it is
deliberately the least capable process in the picture: it translates no
pathname, mounts nothing, opens nothing, and holds no state beyond child pids,
their channels, and the view root the volume worker reported. Volume-class
records are forwarded down the private channel and access-class work happens
on a channel the broker holds directly, so neither worker has a descriptor
reaching the other.

The workers are separate processes rather than modules in one, and the reason
is stronger than tidiness. Each closes every channel that is not its own on
the way in: the volume worker never holds the broker's, and the CRM holds
neither the broker's route to the volume side nor the supervisor's. "The CRM
must not be able to issue volume operations" therefore stops being a check
that can be got wrong and becomes a connection that does not exist.

The namespace is created in the supervisor, once, before either worker is
forked. That ordering is what makes the split work: `fork()` shares a mount
namespace and `unshare()` does not, so two workers that each unshared for
themselves would hold two private views of the disks that looked identical and
were not -- and the CRM's would be the one without the device the volume
worker had mounted. Creating a container is not mount policy; every decision
about what may be mounted, and where, stays in the volume worker, which is the
only process here that calls `mount()` at all. Where the namespace cannot be
had, the session continues without a device view rather than failing to start,
and the failure is remembered so that no worker quietly makes one of its own
later.

One authentication covers all of it. The user authorised ACE, not a particular
number of helper processes, and asking a second time for one decision is the
prompt fatigue this design already refused once. The supervisor forks each
worker and passes the broker's channels back over the existing descriptor
mechanism.

### Why the private namespace survives an unprivileged broker

An earlier version of this document assumed the broker could keep a private
mount namespace while becoming an ordinary user process. It cannot, and the
reason is structural rather than a matter of permissions:

```text
root-owned mount namespace, joined from uid 1000:
    setns: Operation not permitted (errno 1)
joined from root:
    setns: joined
```

`setns(fd, CLONE_NEWNS)` requires `CAP_SYS_ADMIN` in the user namespace that
owns the target mount namespace. A namespace created by the root FMM/CRM service is
owned by the initial user namespace, where a user process has no such
capability and no way to acquire one. Today this is invisible only because
`--root` re-executes the shell, the broker, and every command as root, so
everything is already inside.

The two-process split dissolves this rather than working around it: nobody
unprivileged ever needs to enter the namespace, because nobody unprivileged
ever opens a path inside it. The CRM opens the object and passes
back a descriptor, and a descriptor does not belong to a namespace. Bulk I/O
then happens in the user's own process on that descriptor, so a large Copy is
one request rather than one per byte.

This works only because nothing in ACE reaches the filesystem behind the
seam, which is true and worth keeping true: ACE commands and LhA are compiled
with `-Dopen=ace_amiga_posix_open` and its relatives, Vim with
`-Dopen=ace_vim_open`, and the DOS layer goes through `native_dos.c`. A future
component that opened a raw host path directly would silently lose the device
view, and that is the property to protect.

### Consequences that follow from the split

One escalation trigger, and it is not the path. An earlier version of this
document had two: `EACCES`/`EPERM` for a protected object, and a prefix match
against the view root for a device-view path, on the grounds that such a path
fails locally with `ENOENT` and `ENOENT` must never escalate. The second one
was a mistake, and `src/ace_crm_retry.c` no longer has it. Deciding that an
operation needs privilege by comparing its pathname to a stored string is
guessing, it skipped the unprivileged attempt entirely, and the special case
existed only to work around the narrowness of the first trigger.

The seam now attempts every operation as the user and retries *any* failure
through the CRM in a `--root` session. The device-view case then needs no
special handling at all: such a path fails locally like anything else and the
retry serves it. What must survive is the answer -- when the privileged
attempt also fails, `report_failure()` keeps the caller's own errno if the
reply described the channel rather than the object, so a mistyped name still
says "not found" rather than `EIO`.

The one place a view-root prefix comparison remains is
`privop_domain_path()` in the broker, and it is not a privilege decision: by
then a failure has already summoned the CRM, and the comparison only chooses
which root the CRM resolves beneath -- `RESOLVE_BENEATH` for a volume,
`RESOLVE_IN_ROOT` for the host. That is containment.

`LNX` children lose the device view. They are Linux processes outside the ACE
seam, so they will not see paths under the view root. Today they inherit it
because everything is root and inside the namespace. This is consistent with
`LNX` being an explicit escape hatch rather than an implicit privilege path,
but it is a behaviour change and is recorded here as one.

### User shell and console

`ace-shell`, `ace-console`, and all normal ACE commands remain user processes.
The console exports menus and uses the user's session bus. It never contacts a
root D-Bus session and never reads root's configuration.

The shell talks to the broker. It does not need a privileged socket of its own.
This keeps one semantic authority and one privilege ingress.

### User broker

The broker always runs as the logged-in user. It owns only user-local and
Amiga-semantic state, including:

* sessions and connected clients;
* current directories and current devices;
* assigns and assign lists;
* environment variables and temporary files;
* AmigaDOS path parsing and translation;
* command/task/port state that is intentionally broker-backed;
* pending operation IDs and cancellation state.

The broker must not become a root process merely because a client asks for
privileged assistance. It sends a typed request to the FMM/CRM service and returns the
FMM/CRM service's result to the DOS compatibility layer.

The broker may retain descriptors, request IDs, and reconnect metadata. “No
state in the FMM/CRM service” does not mean no state anywhere; Amiga session state
belongs in the broker, while mount/resource state belongs in the volume worker.

### FMM/CRM service supervisor

The FMM/CRM service is a privileged implementation service, not a shell and not a
general root command runner. It has no `HOME`, current directory, user config,
GUI, D-Bus menu, or arbitrary command-string interface.

It exposes a versioned, typed RPC protocol with bounded messages. It must never
accept a request equivalent to “execute this shell command as root.” In
particular, it must not provide a generic root `execve()` operation to ACE
scripts, ARexx, or malformed commands.

The FMM/CRM service is launched only for a session that has opted into privileged
requests, and lazily: `--root` says a session may ask for privilege, not that
it has, and nothing root-side exists until the first operation that needs it.
Its supervisor keeps no filesystem or shell semantics of its own -- see the
process model above.

### Volume worker

The volume worker owns the privileged device/view responsibilities:

* discover the mounted filesystems, block-backed or not;
* build and maintain the ACE private mount namespace;
* create ACE-managed bindroots;
* mount supported filesystems on demand;
* unmount/detach them on request and orderly shutdown;
* clean up automatically when the namespace's last holder disappears;
* return stable logical device identifiers and carefully controlled root
  handles.

Its interface must not contain arbitrary file-write, delete, or command-exec
operations. A malformed file operation must not be able to ask it to mount an
arbitrary host path.

The volume worker necessarily holds kernel/resource state: a mount namespace,
mounts, bindroots, open descriptors, and worker lifetime. That is acceptable.
It must not hold Amiga current directories, assigns, environment variables, or
other semantic session state.

### CRMs

An CRM handles a narrowly scoped privileged DOS operation. It may be
one-shot or short-lived. Examples:

* open one exact protected source file for reading;
* open/create one exact protected destination file for writing;
* enumerate one exact protected directory;
* delete one exact path;
* rename one exact pair of paths;
* create one exact directory;
* perform one explicitly supported metadata operation.

For file I/O, prefer opening the object in the CRM and passing the
resulting descriptor back over the broker/FMM/CRM service channel. The ACE command can
then copy bytes as the ordinary user without receiving general root power.
For unlink, rename, mkdir, and similar operations that cannot be represented by
an fd, the CRM performs exactly that typed operation and returns a
precise status.

The CRM cannot issue volume-worker operations, and this is now
structural rather than enforced. It is a separate process, forked by the
supervisor into the namespace the supervisor made before either worker
existed, and it closes both the supervisor's channel and the private channel
to the volume worker on the way in. There is no capability bit meaning
"may not mount" -- there is nothing to mount with and nobody to ask. The class
check in the worker remains as a stated answer beside the structural one, so
that a reader of either reaches the same conclusion.

## Meaning of `--root`

`--root` is no longer a request to run ACE as UID 0. It means:

> This ACE session is allowed to request privileged assistance from the
> FMM/CRM service when ordinary user access is insufficient.

Without `--root`, ACE performs ordinary user operations and reports the
permission failure immediately. With `--root`, the shared DOS layer may retry a
denied operation through the access FMM/CRM service. Authorization is lazy in the
strong sense: starting ACE with `--root` does not make the shell root, does not
prompt, and does not start a privileged process at all. Nothing exists on the
root side of the boundary until something the user tried has been refused.

Example:

```text
Copy SYS:foo TO /root/foo
```

The command remains a user process. It copies ordinary files normally. When
opening the protected destination fails with `EACCES` or `EPERM`, the DOS
compatibility layer reports a structured `NEEDS_PRIVILEGE` condition to the
broker. The FMM/CRM service requests administrator authorization for that operation,
opens the exact object as needed, and the command continues.

The user-facing explanation should be “ACE requests administrator access for
this operation,” not “ACE has switched to root mode.” There is no root/user
title-bar mode.

The old `--user`, `--deviceview`, and `--mountview` switches should not remain
as the normal product model. They may temporarily survive as compatibility or
developer diagnostics while the migration is staged, but they must not define
the final user experience. `--root` is an authorization policy, not a view
selector.

Running the shell or broker while already UID 0 should fail clearly by default:

```text
ACE must be started as a normal user; privileged operations are provided by
the ACE FMM/CRM service.
```

Do not try to repair `sudo ace-shell` by copying `HOME`, D-Bus, or Wayland
environment variables into the root process. Unix session-bus credentials and
configuration ownership are identity-bound.

## Automatic granular elevation

The intended behavior is automatic at the shared API layer, not in every
command implementation.

Most AROS/Amiga commands use common DOS APIs such as:

* `Open`, `Read`, `Write`, and `Close`;
* `Lock`, `UnLock`, `Examine`, and `ExNext`;
* `DeleteFile`, `Rename`, and `CreateDir`;
* protection/metadata functions;
* the ACE POSIX wrapper used by embedded Vim and related components.

The common layer attempts the operation as the user first, and retries a
failure -- any failure -- through the FMM/CRM service in a `--root` session.
An earlier draft narrowed this to `EACCES`/`EPERM`, which forced a second,
path-based trigger to exist for objects that fail with `ENOENT` because they
are somewhere this process cannot see. Retrying everything removes the need
for that trigger, and so removes the guessing.

What must not happen is the failure being *replaced*: an ordinary `ENOENT`
that the privileged attempt also could not fix still reaches the user as
"not found", never as a fact about the channel that carried it.

Neither the AmigaDOS path nor the host path takes any part in that decision.
The set of objects that need privilege is whatever the kernel refuses, which is
always right and never drifts; a list of protected places would be a second,
worse answer to a question the kernel is already answering. Two consequences
are worth stating because they are the ways this has actually gone wrong:

* **Naming is not access.** Translating a path must not require the broker to
  be able to stat it. A directory the user cannot enter is exactly where an
  authorised session is supposed to be able to work, and a resolver that
  refused such a name would end the operation before it ever reached the seam
  that exists to retry it. See `within_base_filesystem()` in `src/broker.c`.
* **The device view is not a route for ordinary files.** Its paths live in a
  namespace no user process is in, so anything resolved there can only be
  opened by the access worker, as root. The broker therefore resolves through
  the host's own mount whenever the host can name the object, and reaches for
  the view only when it cannot -- a device the host has not mounted, or a
  directory covered by something mounted over it. Routing a whole mounted
  device through the view would make every file on it a privileged operation:
  every read done as root, every new file root-owned, and nothing about it
  visible to the user. See `ace_dos_devices_root()` in `src/dos_devices.c`.

`tests/escalation_contract_test.sh` holds all of this in place from outside, by
counting privileged processes: an authorised session that has been refused
nothing must be running none.

This lets commands remain unchanged:

```text
command -> DOS compatibility API -> user syscall
                                  -> if denied: broker -> FMM/CRM service
```

The implementation work is concentrated in the shared DOS/native seam and the
broker protocol. It must not require rewriting every command.

Current ACE source is not perfectly centralized. The migration must audit and
route the direct host calls in `src/native_dos.c`, `src/ace_amiga_posix.c`,
broker-side file operations, and any other shared wrappers. `LNX` is explicitly
outside this model: it is an experimental Linux escape hatch, and a Linux
program launched by `LNX` remains an ordinary unprivileged Linux user process,
with the host Linux filesystem view rather than ACE's device view. Its
interactive PTY path is implemented, so a user who wants `sudo bash` may use
`LNX sudo bash`; any elevation remains subject to the host's normal policy.

Unmodified third-party code that bypasses ACE's DOS/POSIX seam is also outside
automatic per-object elevation. That is desirable; do not add a dangerous
global syscall interception layer just to cover it.

## Authorization and powers

The FMM/CRM service should use polkit or an equivalent action-based authorization
mechanism for production. `sudo`/`pkexec` may be useful for development or
bootstrap experiments, but they are poor foundations for a descriptor-sensitive
long-lived GUI protocol because they rewrite environments and may not preserve
the required file descriptors.

Use distinct authorization concepts even if `--root` enables both:

* manage/discover/mount ACE device filesystems;
* read protected files;
* modify protected files;
* alter protected metadata, if supported.

### Authorization lifetime: decided, session-scoped

The explicit product decision this section previously asked for has been made:
`--root` is the permission, one authentication turns it into a running
FMM/CRM service, and that FMM/CRM service serves the session until the session ends, the
user gives the privilege up through `DROP_PRIVILEGE`, or an optional timeout
expires. The timeout is off by default and exists because it was asked for,
not because ACE imposes it.

Per-object authorization was considered and rejected. A `Copy` of two hundred
files would raise two hundred prompts, and a person shown the same dialog two
hundred times is not making two hundred decisions: they are making one and
then reflexing one hundred and ninety-nine times. Prompt fatigue converts a
security control into a formality, and a formality the user has been trained
to dismiss unread is worse than none, because it also launders the result as
consent. It is equally wrong for the machine being emulated. An Amiga has no
privilege model because the person at the keyboard owns the computer, and an
interface that repeatedly asks that person for permission to use their own
disk feels like a guest account, not an Amiga.

The consequence is recorded in `src/ace_privilege_protocol.h` and must not be
forgotten: with session-scoped authorization the FMM/CRM service's opcode surface is
the entire security boundary, not one layer of several. Nothing downstream
asks again. That makes the narrow typed interface more load-bearing, not less.
A generic operation added to the FMM/CRM service would not be a weakness in depth --
in a `--root` session it would be a root shell reachable from any ARexx
script.

### File ownership: decided, and emergent rather than configured

Files created through the FMM/CRM service are owned by root, as `sudo cp` would leave
them. ACE does not invent an ownership rule, and AmigaDOS has no owner model
to borrow one from.

This is safe only because authorization is not execution as root. The shell,
the commands, and the broker remain the user's own processes; the shared seam
attempts every operation as the user first and reaches the FMM/CRM service only
once that attempt has failed. So the session-long authorization does not make
everything the user touches root-owned -- which is precisely what the current
`ace_mode_elevate_if_needed()` re-exec does today, including in the user's own
home. The two decisions depend on each other and must not be separated.

The set of places where root-owned files can therefore appear is exactly the
set of places the user could not have written anyway: `/root`, `/etc`, `/usr`.
There, root-owned is the correct outcome and a user-owned file would be the
anomaly -- and a genuine escalation, since a user-owned file in a system tree
outlives the session as permanent write access obtained from a temporary
lease.

**Do not ever add a list of protected paths.** The boundary is defined by the
kernel's answer to the attempt, not by anything ACE maintains. A configured
list would be redundant when correct and wrong when it drifted, and it would
elevate in places that did not need it. The rule is emergent by design: try as
the user, and let the failure decide.

Commands are talky, so a file that does land root-owned says so when it is
created rather than leaving the user to discover it later when they cannot
edit it.

### Ownerless filesystems: the Amiga model made exact

The one case where root ownership would intrude on the Amiga world proper is
removable media: a stick or card mounted root-owned turns `Copy` into `DH1:`
into root-owned files on the user's own floppy.

FAT, exFAT, and NTFS do not carry ownership either. The kernel invents it at
mount time from `uid=`/`gid=`. The volume worker therefore mounts those
filesystems with `uid=` set to the ACE user, which makes the Amiga model
exactly true rather than approximately true: an ownerless filesystem presented
to a single user who owns all of it is what a floppy was. No FMM/CRM service
involvement and no ownership concept anywhere in sight.

Filesystems that genuinely carry ownership, and the system directories, keep
Linux semantics and ACE reports them honestly. The illusion is perfect where
the Amiga actually lived and honest where it did not.

## IPC and authentication requirements

Use an inherited Unix `SOCK_SEQPACKET` socketpair or an equally private,
descriptor-based channel whenever possible. Do not use a predictable global
socket as the trust boundary.

The FMM/CRM service handshake should include:

* protocol magic and version;
* per-launch random instance identifier;
* expected broker PID and process-start identity where available;
* peer credentials (`SO_PEERCRED`/`SCM_CREDENTIALS`);
* bounded maximum message and descriptor counts;
* explicit feature negotiation.

Every request needs a monotonically tracked request ID, an opcode, flags, and a
bounded payload length. Replies must return the request ID, a success/failure
status, a stable ACE error number, and a Linux errno only as supplementary
diagnostic information.

Do not trust a filesystem path merely because it came from the broker. Validate
logical device IDs and relative paths. Reject `..`, unexpected absolute paths,
magic links, and symlink escapes according to the chosen DOS semantics. Use
`openat2()`-style beneath/no-magic-link constraints where available, and use
dirfds/open handles rather than repeatedly resolving untrusted strings.

The FMM/CRM service must never parse or invoke a shell command. It must never accept an
arbitrary executable path from an ACE script.

## Signals, cancellation, and lifetime

The root workers must not be placed in the terminal's foreground process group.
Signals should be translated into protocol messages:

```text
terminal SIGINT/SIGBREAK
    -> ACE shell
    -> broker BREAK/CANCEL(request_id)
    -> FMM/CRM service operation worker
```

The broker must not rely on `kill()` to control a root FMM/CRM service. The FMM/CRM service
should watch its private channel for EOF/HUP and, where available, monitor a
broker `pidfd`. An orderly shutdown is an authenticated `SHUTDOWN` request.

If the broker dies, the FMM/CRM service must clean up and exit. If the volume worker
dies, its mount namespace must eventually disappear; the broker reports the
device-view service as unavailable and can request a clean restart. If an
CRM dies, only that operation fails.

Cancellation must be operation-specific. Atomic operations such as rename and
unlink either happen or do not happen. Multi-file copy needs defined behavior
for a partial destination and cancellation.

## Migration plan

The plan below replaces an earlier nine-phase sequence that contained an
ordering error. That sequence placed "make the broker permanently user-owned"
(its Phase 2) before "implement the volume worker" (its Phase 4). The broker's
root-ness is load-bearing for exactly one thing: it creates the private mount
namespace at `broker.c:3928`, mounts the device view into it through
`ace_dos_devices_prepare_device_view()`, and clients `setns()` into *it* from
`broker_client.c`. The broker cannot stop being root while it still owns the
namespace, so de-rooting must follow the volume worker, not precede it. Doing
it the other way round means writing a compatibility path that is discarded
unread.

Two other merges follow from reading the code rather than the design: the
FMM/CRM service channel and the de-rooting are one change once the volume worker
exists, and the shared-wrapper work and the access opcodes are a single design
because the wrappers and the operations that serve them have to be shaped
together.

Five chunks, in dependency order.

### Chunk A: freeze the contract

Write the protocol and the error semantics before implementing any privilege.

`src/ace_privilege_protocol.h` exists and is the authority: opcodes carrying
their capability class in the high byte so the check is a mask rather than a
table that can drift from the enum beside it; the handshake, nonce, and peer
validation; bounded payloads and descriptor counts; a stable ACE status space
with the host `errno` alongside as diagnostic detail only; and no operation
that executes anything.

Decisions recorded above: session-scoped authorization, root-owned creates in
root-owned places with no configured path list, ownerless filesystems mounted
as the user, `DROP_PRIVILEGE` for deliberate reduction.

Provisional and reversible: the FMM/CRM service is launched with `pkexec` and
connects *back* to a nonce-named socket under `XDG_RUNTIME_DIR`, because
`pkexec` rewrites the environment and will not carry an inherited socketpair
across the exec. Polkit action names can replace the authorization step later
without changing the protocol.

No behaviour change in this chunk.

### Chunk B: the FMM/CRM service process and its channel

A privileged `ace-fmm` and the broker-side client. Launch, handshake,
`SO_PEERCRED` and nonce validation in both directions, request ids,
`PING`/`CAPS`/`CANCEL`/`DROP_PRIVILEGE`/`SHUTDOWN`. No privileged operation is
served yet, which is the point: the channel and its lifetime get tested before
anything dangerous rides on them.

Lifetime: the FMM/CRM service exits on channel EOF, because a broker that crashed
cannot send `SHUTDOWN`. The broker never uses `kill()` on the FMM/CRM service -- a
user process holding a signal lever on a privileged one is the arrangement
this design exists to avoid.

### Chunk C: the two privileged processes, then de-root the broker

C1 and C2 are done: the supervisor makes the namespace and then owns two
kinds of child inside it -- a volume worker holding the mounts and the device
view, and an CRM per request whose channel it hands back to the broker. The
supervisor itself serves nothing: volume records are relayed to the volume
worker over a private channel and the reply relayed back unchanged.

C3 is done. `prepare_device_view()` asks the FMM/CRM service instead of mounting;
`broker.c` no longer unshares anything; `broker_client.c`'s namespace joining
and its `unshare(CLONE_FS)` workaround are deleted rather than moved, since no
user process enters a namespace any more; and `--root` no longer re-executes
anything, so `ace_mode_elevate_if_needed()` is gone. Starting any ACE program
as root now fails with an explanation.

The FMM/CRM service launcher prefers a noninteractive `sudo` probe over `pkexec`,
following what `ace_modes.c` already did and for the same reason its comment
gives. That is also what makes the elevated path testable: an elevated channel
that only runs when somebody types a password is one that never runs in a test
suite.

`make test-device-view` was named here as the regression gate, and it was the
wrong choice: it skips on any machine without a nested mount to reveal, which
includes the machine this was developed on, so it gated nothing.
`make test-FMM/CRM service-broker` covers what actually changed -- the broker runs as
the user, a root FMM/CRM service holds the privilege, the two are in different mount
namespaces, the session answers, the FMM/CRM service dies when the broker does, and a
root broker is refused. Prefer it, and treat a skipping test as untested
rather than passing.

### Chunk D: escalation for protected paths, and the DOS seam

Mostly done. `src/ace_crm_retry.[ch]` is the single place in ACE that decides an
operation needs privilege: it does the ordinary thing first as the ordinary
user, and a failure -- whatever it was -- becomes a request. Nothing in that
file looks at the path. `native_dos.c`
and `ace_amiga_posix.c` both route through it, which covers ACE's own commands
and the unmodified third-party code compiled with the `-Dopen=` redefinitions.

Commands never hold a channel to a root process. They ask the broker
(`AMIGA_BROKER_PRIVOP`), the broker asks the CRM, and the reply --
including any descriptor -- comes back the same way. One semantic authority,
one privilege ingress. The broker also decides which resolution domain a path
is in, because the broker is where path translation lives.

`make test-privileged-file` proves the whole path end to end: an unprivileged
`Type` reads a root-only file, and `Delete` removes one from sticky `/tmp`
where the user could not. It also checks the premise -- that the same user
genuinely cannot read the file -- because without that the rest proves
nothing.

**What is not done, and is the next piece of work.** Path *resolution* still
happens in the broker as the ordinary user, so an object inside a directory
the user cannot traverse cannot be named at all:

```text
resolve RAM4:protected-dir        -> /tmp/protected-dir     (works)
resolve RAM4:protected-dir/file   -> fails                  (broker cannot stat it)
```

Escalation therefore reaches protected objects in traversable directories --
which is the common case, `/etc/shadow` among them -- but not objects inside a
`0700` directory owned by root. The fix is for the broker's resolution to use
its own CRM when a component is refused, exactly as the seam does
for operations. It was left out rather than half-built: the resolution code is
substantial and threading escalation through it deserves its own pass.

Two smaller gaps: `ace_crm_retry_last_was_privileged()` exists so that commands
can be talky about a file that has just been created root-owned, and no
command consults it yet; and `utime`, `symlink` and `readlink` have no FMM/CRM service
operations, so they do not escalate.



Shared wrappers for `open`, `stat`, directory enumeration, `unlink`, `rename`,
`mkdir`, and protection changes. Attempt as the user; fall back to the
FMM/CRM service when that attempt fails, whatever it failed with. The path
takes no part in the decision.

Prefer opening in the worker and passing the descriptor back over the existing
`SCM_RIGHTS` machinery the broker protocol already has, so the privileged part
is the single `open()` that needed it and what returns is a capability to one
object rather than power over others.

Every path is resolved with `openat2()` under `RESOLVE_BENEATH` and
`RESOLVE_NO_MAGICLINKS` against a dirfd the FMM/CRM service already holds. `..`,
unexpected absolute paths, and symlinks leaving the tree are refused rather
than normalised. `RENAME` carries both paths in one request, because a rename
whose halves were resolved separately is two operations with a window between
them.

Adapt `src/native_dos.c`, `src/ace_amiga_posix.c`, and the broker path seam.
Do not edit commands one at a time.

### Chunk E: policy, hardening, install, documentation (complete)

`--root` is now authorization only. `--user`, `--deviceview`, and
`--mountview` are rejected by the product CLI; the broker chooses its internal
view from the authorization policy. The FMM/CRM service launch tries silent
`sudo -n` first and falls back to the ACE-specific `org.ace.Ace.fmm`
polkit action, with generic `pkexec` retained for installs whose user-local
policy directory is not searched by polkit.

The full test target set covers ordinary and denied operations, FMM/CRM service and
broker death, stale channels, malformed packets, path traversal, symlink
escapes, cross-class requests, cancellation, and the protected-file end to
end path. `make install` installs the action, desktop launcher, matching
binaries, and optional companions. The remaining FMM/CRM service gaps are the path
resolution, talky reporting, and unsupported DOS-call items recorded below.

Run the full suite, `make install`, update `README.md`, `HANDOFF.md`, and
`TODO.md`. Do not leave a root GUI or root broker running as a test artifact.

## Existing tree and work discipline

This repository already contains substantial work for the superseded visible
root/device-view model, including `--root`, `--user`, `--deviceview`,
`--mountview`, private bindroots, broker identity fields, and root namespace
joining. Treat those changes as implementation material to refactor, not as the
final architecture.

The following tactical fixes were made during earlier work and may be useful
while migrating:

* multithreaded ACE clients needed `unshare(CLONE_FS)` before joining a mount
  namespace, because Linux otherwise rejected `setns(..., CLONE_NEWNS)` with
  `EINVAL`;
* `Assign` had a tab-parser bug that collapsed empty label fields and exposed
  `/dev/sda2` as an Amiga alias; preserving empty fields fixed it;
* installation dependencies were adjusted so `ace-modes.o` is linked into
  Tine/Vim-related targets.

Do not discard unrelated user work. The working tree has also contained Peek
work (`src/peek.c`, `tests/peek_test.sh`) and other pre-existing changes. Always
inspect `git status` and the relevant diff before editing. Use `apply_patch` for
source/document edits. Avoid destructive resets and broad deletion.

When validating changes:

1. build with the repository's normal Makefile and warning flags;
2. run focused tests for the touched seam;
3. run mode/device/filesystem translation tests during migration;
4. run the full suite before installation when practical;
5. use `make install` only after the build is clean;
6. verify installed binaries are the build just tested;
7. clean only diagnostic processes and files created by the test.

Useful existing focused tests include `test-modes`, `test-device-view`,
`test-filesystem-translation`, `test-system-assigns`, and the broader command
and shell tests listed in the Makefile. New FMM/CRM service tests should use private
temporary broker/FMM/CRM service sockets and must always clean up their processes and
mount resources.

## Decisions not to regress

The following user decisions remain in force unless explicitly revisited:

* ACE presents logical Amiga device names, not Linux mountpoint paths;
* Linux absolute symlinks retain Linux meaning and translate to the target ACE
  device when they cross filesystems;
* assigns are created from logical device locations, never raw Linux paths;
* the device view covers every filesystem, not only the block-backed ones.
  This reverses an earlier scope decision, and the reason it was wrong is
  worth keeping: a view that held only vfat and ext gave `/run` no view of its
  own, so the directories systemd's per-unit credential mounts obscure on it
  had nowhere to be seen -- while enumeration, which reads the host directory,
  listed them anyway. ACE showed a name and then said it did not exist, and
  `Dir ALL`, `List ALL` and `Copy ALL` each walked into one. A session that
  asked to see the filesystems independently asked about all of them.
  A block device is still mounted from its device node, because the host may
  not have mounted it at all; everything else is bind-mounted, non-recursively,
  from wherever it already is, and named to the worker by device id so that
  the broker still names a filesystem rather than a place. A filesystem that
  cannot be shown on its own -- one whose only mountpoint is itself covered,
  as autofs under `/proc/sys/fs/binfmt_misc` is -- is absent from the view
  rather than fatal to it;
* btrfs is currently unsupported;
* procfs/sysfs do not yet have a final model;
* RAM is modeled as one RAM per tmpfs;
* commands are talky and report both success and failure with specific error
  numbers where possible;
* atomicity matters for filesystem operations;
* `LNX` is an explicit Linux escape hatch, not an implicit ACE privilege path;
* authorization is session-scoped: `--root` plus one authentication, until the
  session ends, `DROP_PRIVILEGE`, or an optional timeout that is off by
  default. ACE does not prompt per object, and must not be changed to;
* the shared seam always attempts an operation as the user first, and reaches
  the FMM/CRM service only when that attempt failed. Trying as the user first
  is what keeps session-scoped authorization from making everything the user
  touches root-owned, and `tests/navigation_matrix_test.sh` holds it in place
  by checking that what an authorised session creates in the user's own drawer
  is still owned by the user. Which failures are retried is a separate
  question from whether the attempt happens, and the answer to the second is
  always;
* there is no configured list of protected paths, and none may be added. The
  boundary is whatever the kernel refuses;
* files created through the FMM/CRM service are root-owned, and ACE says so when it
  creates them;
* ownerless filesystems (FAT, exFAT, NTFS) are mounted with `uid=` set to the
  ACE user, because a filesystem with no owners presented to a single user who
  owns all of it is exactly what an Amiga floppy was;
* the FMM/CRM service has no operation that executes anything, and gaining one would
  turn a `--root` session into a root shell reachable from any ARexx script;
* the volume worker and the CRM are separate processes, and the
  CRM's inability to mount anything is structural -- it holds no
  channel to the volume side. Do not merge them back into one process with a
  capability mask;
* no unprivileged ACE process ever enters a mount namespace, because it
  cannot: `setns(CLONE_NEWNS)` needs `CAP_SYS_ADMIN` in the owning user
  namespace. Descriptors cross that boundary instead, and they are enough;
* nothing in ACE may reach the filesystem outside the ACE seam. A component
  that opens a raw host path directly loses the device view silently, which is
  the worst way to lose it;
* `LNX` children do not see the device view, and that is correct: `LNX` is an
  escape hatch to Linux, and a Linux process gets Linux's view.

The central security/product decision is now:

> ACE's shell, console, and broker always run as the user. A user who starts
> ACE with `--root` permits the user-owned broker to request narrowly scoped
> privileged operations from a root FMM/CRM service. The FMM/CRM service has a volume-view
> personality and a separate protected-operation personality, but it is not a
> root shell and it does not own user state.
