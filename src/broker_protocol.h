#ifndef AMIGA_SHELL_BROKER_PROTOCOL_H
#define AMIGA_SHELL_BROKER_PROTOCOL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * The protocol version is a hash of this file, computed by the Makefile and
 * passed in as a define.  It is derived rather than hand-maintained because
 * a version nobody remembers to bump is worse than none: it says "compatible"
 * while the layout underneath has changed.  Any edit to this header produces
 * a new version, which is conservative in the right direction.
 *
 * Zero means the build did not supply one -- an ad-hoc compile rather than a
 * make.  Such a build still talks to itself, and reports "unversioned".
 */
#ifndef AMIGA_BROKER_PROTOCOL_VERSION
#define AMIGA_BROKER_PROTOCOL_VERSION 0u
#endif

/*
 * How everything finds the broker.
 *
 * The socket lives in XDG_RUNTIME_DIR, which is per-user, private, and
 * cleared when the user's last session ends, so a socket cannot outlive the
 * login it belongs to.  Where there is no such directory the uid goes in the
 * filename instead, which still separates users even though nothing will
 * clean up after them.
 *
 * The name also carries a hash of the SYS: root -- see
 * amiga_broker_system_root() for why that, and not the user, is the thing a
 * broker's identity actually follows.
 *
 * ACE_BROKER_SOCKET still overrides, for a deliberately isolated broker.
 * That is a thing worth being able to do; it just should not be what happens
 * by accident.
 */
/* Implemented in src/broker_identity.c -- see there for why the rule lives
   in one place and what the broker's identity actually follows. */
const char *amiga_broker_system_root(void);
const char *amiga_broker_socket_path(void);

/*
 * The magic carries the protocol version, so a mismatch is caught on the
 * very first field either side reads rather than after a payload has been
 * misparsed.  A peer's version can be recovered from the magic it sent
 * (magic ^ BASE), which lets the diagnostic name both builds instead of
 * saying only that something is wrong.
 */
#define AMIGA_BROKER_MAGIC_BASE 0x414D4742u /* AMGB */
#define AMIGA_BROKER_MAGIC \
    (AMIGA_BROKER_MAGIC_BASE ^ (uint32_t)AMIGA_BROKER_PROTOCOL_VERSION)

static inline uint32_t amiga_broker_version_from_magic(uint32_t magic)
{
    return magic ^ AMIGA_BROKER_MAGIC_BASE;
}

enum amiga_broker_operation {
    AMIGA_BROKER_RESOLVE = 1,
    AMIGA_BROKER_GETCWD  = 2,
    AMIGA_BROKER_SETCWD  = 3,
    AMIGA_BROKER_ASSIGN  = 4,
    AMIGA_BROKER_GETVAR  = 5,
    AMIGA_BROKER_SETVAR  = 6,
    AMIGA_BROKER_DELVAR  = 7,
    AMIGA_BROKER_GETRESULT = 8,
    AMIGA_BROKER_SETRESULT = 9,
    AMIGA_BROKER_LISTVARS = 10,
    AMIGA_BROKER_GETCLI = 11,
    AMIGA_BROKER_SETFAILLEVEL = 12,
    AMIGA_BROKER_SETPROMPT = 13,
    AMIGA_BROKER_CLONESESSION = 14,
    AMIGA_BROKER_LISTDOS = 15,
    /*
     * Claim ownership of this connection's session.
     *
     * Every other operation is a transient use: a command process asks a
     * question and exits, and the broker cannot tell whether the shell that
     * owns the session is still alive. ATTACH is the shell saying "this
     * session is mine, and it lives exactly as long as this connection
     * does". The broker frees the session when the last attached connection
     * closes, which the kernel reports whether the shell exits, is killed,
     * or crashes.
     */
    AMIGA_BROKER_ATTACH = 16,
    /* Translate an absolute host path back into an AmigaDOS volume name. */
    AMIGA_BROKER_NAMEFROMHOST = 17,
    /* Return the current session's assignments for the DOS compatibility
     * layer that implements the unmodified AROS Assign command. */
    AMIGA_BROKER_LISTASSIGNS = 18,
    /* Resolve a relative DOS path beneath a specific host-side assignment
     * target. The AROS DOS dispatcher chooses the target; the broker only
     * performs component mapping and containment checks here. */
    AMIGA_BROKER_RESOLVE_BENEATH = 19,
    /* Change the filesystem label behind a DOS volume alias. */
    AMIGA_BROKER_RELABEL = 20,
    /* The shell's PATH list, kept in the broker because commands are
     * separate processes from the shell that owns the CLI. */
    AMIGA_BROKER_LISTPATH = 21,
    AMIGA_BROKER_PATH = 22,
    /* A dedicated, broker-to-task control connection.  It is deliberately
       separate from the request/reply connection: signals may arrive while
       a task is doing DOS I/O. */
    AMIGA_BROKER_TASK_ATTACH = 23,
    AMIGA_BROKER_TASK_FIND = 24,
    AMIGA_BROKER_TASK_SIGNAL = 25,
    AMIGA_BROKER_TASK_SET_FOREGROUND = 26,
    AMIGA_BROKER_TASK_BREAK_FOREGROUND = 27,
    AMIGA_BROKER_TASK_LIST = 28,
    /* Report what this broker is: its build, where it lives, which SYS: it
       serves, and what it is currently holding.  Answering "which broker am
       I talking to" should not require reading ps output. */
    AMIGA_BROKER_STATUS = 29,
    /* Public message ports.  A named port belongs to the process that
       registered it, and the point of registering is that another process can
       find it: that is what ADDRESS <port> means in ARexx, and it is the one
       thing a purely in-process port registry cannot do.
       PORT_ADD names a port and gets an id back; PORT_FIND turns a name into
       that id for a process that does not own it; PORT_REM gives it up. A
       port is also dropped when the connection that registered it closes, so
       a process that exits without tidying up does not leave a name that
       resolves to nothing. */
    AMIGA_BROKER_PORT_ADD = 30,
    AMIGA_BROKER_PORT_REM = 31,
    AMIGA_BROKER_PORT_FIND = 32,
    /* A dedicated, broker-to-process connection for message delivery, in the
       same spirit as TASK_ATTACH and for the same reason: a process waiting
       in WaitPort() is not in a position to answer a request, so the broker
       must be able to push to it.

       Deliberately a second connection rather than a second record type on
       the task channel.  A task signal is a fixed-size record; a delivered
       message is a serialised RexxMsg of up to MAX_PAYLOAD bytes, so the two
       cannot share a framing, and a long message must not be able to delay a
       Ctrl-C.

       One per process, not one per port.  A process that owns three ports
       receives all three on this channel, and a process that owns none at
       all still needs it -- a sender waiting for its reply is woken the same
       way.  Which is why the client attaches lazily, on first port use of
       any kind, rather than when a port is registered. */
    AMIGA_BROKER_PORT_ATTACH = 33,
    /* Send a message to a named port, and reply to one that was sent.

       PORT_PUT takes the port's *name*, not the id PORT_FIND would give you,
       so that finding the owner and handing it the message are one
       indivisible step. That is deliberate: on AmigaOS the equivalent
       sequence is wrapped in Forbid()/Permit() precisely so a port cannot
       disappear between the lookup and the send (amifuncs.c:535-545).
       Forbid() means nothing across Linux processes, so the atomicity has to
       come from the operation itself. Splitting this into PORT_FIND followed
       by a send would reintroduce exactly the race Forbid() existed to close.

       The broker answers PUT with a message id and pushes the message to the
       owner's channel. PORT_REPLY names that id and routes the results back
       to whoever sent it -- which is why the sender needs a channel of its
       own even when it owns no port at all.

       Neither carries the sender's identity: the broker takes it from the
       connection with SO_PEERCRED rather than believing what it is told,
       which is also how Exec knew which task had called it. */
    AMIGA_BROKER_PORT_PUT = 34,
    AMIGA_BROKER_PORT_REPLY = 35,
    /* Push-only: never sent as a request, only pushed down a *sender's*
       channel to say that the process holding its message is gone and no
       reply is coming.

       This exists because Linux can produce a situation AmigaOS could not.
       There, a receiver cannot die mid-delivery -- multitasking is frozen
       across the send -- and a task that dies holding a message has usually
       taken the machine with it. Here a receiver can simply be killed, and
       the sender would otherwise wait for a reply that can never arrive,
       forever and by design.

       Deliberately its own record type rather than a PORT_REPLY with a flag.
       "Nobody answered" is not "answered with nothing", and the broker should
       not have to pretend otherwise: it knows the difference and says so.
       Turning it into an ARexx failure result is the client's job, and has
       to happen there anyway -- the broker never looks inside a payload.

       Not a timeout, and there must never be one. A receiver that is alive
       and simply never replies hangs its sender on AmigaOS too:
       WaitPort() is Wait(1 << mp_SigBit) with no timeout and no
       SIGBREAKF_CTRL_C in the mask, so not even a break gets out of it. That
       is the semantics, not a defect, and ACE reproduces it. */
    AMIGA_BROKER_PORT_ABANDONED = 36,
    /* Request-side operation: push an opaque event to every attached port
       channel. It has no message correlation and is used for process-wide
       state notifications such as ARexx resource updates. */
    AMIGA_BROKER_PORT_BROADCAST = 37,
    /*
     * Where the FMM/CRM service put the device roots, or empty for an ordinary
     * session.
     *
     * A command needs this to recognise a path that only the CRM
     * can open.  Such a path fails locally with ENOENT -- it is not in this
     * process's mount namespace and never will be -- and ENOENT must never be
     * what triggers a privileged request, or every misspelling in every
     * script would become one.  So the seam asks once, caches it, and routes
     * by where the path is rather than by how the attempt failed.
     *
     * This is not the forbidden list of protected paths.  It is one value,
     * learned at runtime from the process that created it.
     */
    AMIGA_BROKER_VIEWROOT = 38,
    /*
     * Perform one privileged file operation, by proxy.
     *
     * The command asks the broker; the broker asks the FMM/CRM service's access
     * worker; the answer, and any descriptor it produced, comes back the same
     * way.  Commands never hold a channel to a root process: one semantic
     * authority and one privilege ingress, so "may this happen" has exactly
     * one place to be answered and exactly one place to be got wrong.
     *
     * The broker also decides which path domain the request is in, because
     * the broker is where path translation lives.  A path beneath the device
     * view goes as a view-relative one, so it is resolved with RESOLVE_BENEATH
     * and cannot leave its volume; anything else goes as a host path.  The
     * command does not need to know the difference and is not asked.
     */
    AMIGA_BROKER_PRIVOP = 39,

    /*
     * The session's count of operations that needed the CRM to complete, and
     * whether the shell is reporting it.
     *
     * Counted here rather than in the commands because the commands are
     * separate processes: each one lives for a single operation and takes its
     * knowledge with it when it exits, while the broker is the single point
     * every privileged request already passes through.  The flags say which
     * of the four things is wanted; see AMIGA_BROKER_TALLY_*.
     */
    AMIGA_BROKER_TALLY = 40,

    /* Shared Piper voices. The broker, rather than a shell or systemd user
       unit, owns their processes so every ACE shell reuses a warm model. */
    AMIGA_BROKER_SAY_WARM = 41,
    AMIGA_BROKER_SAY_STATUS = 42
};

#define AMIGA_BROKER_ASSIGN_REMOVE       0x0001u
#define AMIGA_BROKER_ASSIGN_ADD          0x0002u
#define AMIGA_BROKER_ASSIGN_PREPEND      0x0004u
#define AMIGA_BROKER_ASSIGN_PATH         0x0008u
#define AMIGA_BROKER_ASSIGN_DEFER        0x0010u
#define AMIGA_BROKER_ASSIGN_REMOVE_ITEM  0x0020u

#define AMIGA_BROKER_PATH_ADD       0x0001u
#define AMIGA_BROKER_PATH_PREPEND   0x0002u
#define AMIGA_BROKER_PATH_REMOVE    0x0004u
#define AMIGA_BROKER_PATH_RESET     0x0008u

#define AMIGA_BROKER_VAR_LOCAL  0x0001u
#define AMIGA_BROKER_VAR_GLOBAL 0x0002u
#define AMIGA_BROKER_VAR_SAVE   0x0004u
#define AMIGA_BROKER_VAR_ALIAS  0x0008u
#define AMIGA_BROKER_VAR_ANY    0x0010u
#define AMIGA_BROKER_VAR_VARIABLE 0x0020u
/* Internal callers may already hold an absolute Linux path (for example a
 * native lock passed to CurrentDir). It must not receive Amiga '/' semantics. */
/*
 * What an AMIGA_BROKER_TALLY request is asking for, in its flags.
 *
 * REPORT is the one the shell sends before each prompt: it answers with the
 * count and clears it, so the number always covers the ground between one
 * prompt and the next.  It answers zero while the tally is off, so a session
 * that turns it on does not immediately receive a backlog of operations
 * nobody was counting.
 */
#define AMIGA_BROKER_TALLY_REPORT 0u
#define AMIGA_BROKER_TALLY_ON     1u
#define AMIGA_BROKER_TALLY_OFF    2u
#define AMIGA_BROKER_TALLY_STATE  3u

#define AMIGA_BROKER_PATH_HOST  0x80000000u

#define AMIGA_BROKER_MODE_ROOT       0x0001u
#define AMIGA_BROKER_MODE_DEVICEVIEW 0x0002u

#define AMIGA_BROKER_MAX_PAYLOAD 16384u

/* Which of a message's streams travel with it, and in which order the
   descriptors are attached: stdin first when present, then stdout.

   rm_Stdin and rm_Stdout are the *sender's* own streams, and they matter:
   RexxMast adopts them as the script's input and output when they are not
   BNULL (RexxMast.c:257-260), which is how a script sent to another process
   writes on the console of the process that sent it. On AmigaOS this is free,
   because a BPTR FileHandle is valid in any task. Here it is not, so the
   descriptors themselves are passed with SCM_RIGHTS and the receiver gets a
   real handle onto the sender's streams. */
#define AMIGA_BROKER_PORT_HAS_STDIN   0x0001u
#define AMIGA_BROKER_PORT_HAS_STDOUT  0x0002u
#define AMIGA_BROKER_PORT_MAX_FDS     2u

/*
 * Request flags.
 *
 * AMIGA_BROKER_FLAG_FOR_OPEN says what the resolved name is about to be used
 * for, and it exists because the answer differs.  A character device, a
 * socket or a FIFO is a real entry with a real name: it can be examined,
 * renamed and deleted like anything else, and Lock() and DeleteFile() must go
 * on working.  What cannot be done is reading it as a file, and finding that
 * out by trying is what has to be avoided -- opening a FIFO with no writer
 * blocks uninterruptibly, and opening a character device succeeds and never
 * ends.  So the caller says it means to open, and the broker answers before
 * anything is opened.
 */
#define AMIGA_BROKER_FLAG_FOR_OPEN 0x00000001u

struct amiga_broker_request {
    uint32_t magic;
    uint32_t operation;
    uint32_t session_length;
    uint32_t path_length;
    uint32_t value_length;
    uint32_t flags;
    uint32_t mode;
    uint32_t owner_uid;
    /* For AMIGA_BROKER_PRIVOP: which FMM/CRM service operation is being asked for.
       Zero for everything else.  Carried as its own field rather than packed
       into flags, so that a request naming no operation names none rather
       than accidentally naming the first one. */
    uint32_t privop;
    /* The file mode for a privileged create or protection change.
       Deliberately not `mode`, which every request already uses to carry the
       session's privilege and view so the broker can refuse a client that
       thinks it is in a different session.  Overloading it here made every
       privileged request look like a mode mismatch and be refused with
       EXDEV -- a failure that pointed at the wrong thing entirely. */
    uint32_t privop_mode;
    /* For a privileged date change: the modification time, in seconds since
       the epoch.  Sixty-four bits and signed, because a `mode` is the wrong
       shape for a date twice over -- it is too narrow to survive 2038, and
       AmigaDOS dates can sit either side of the epoch. */
    int64_t privop_time;
};

/* The response carries a descriptor as ancillary data.  Stated in a flag
   rather than left for the receiver to notice, because "no descriptor" and "a
   descriptor I failed to collect" must not look alike. */
#define AMIGA_BROKER_RESPONSE_HAS_FD 0x0001u

struct amiga_broker_response {
    uint32_t magic;
    int32_t status;
    uint32_t payload_length;
    uint32_t flags;
};

/* Records sent only from broker to a TASK_ATTACH connection, after its
   ordinary successful attach response. */
struct amiga_broker_task_signal {
    uint32_t magic;
    uint32_t operation;
    uint64_t task_id;
    uint32_t signals;
};

/* Records sent only from broker to a PORT_ATTACH connection, after its
   ordinary successful attach response.

   Unlike a task signal this is a header, not a whole record: payload_length
   bytes of serialised message follow it immediately.  message_id is the
   broker's correlation id for one send, and it is what a reply is routed
   back by -- the sender kept its own struct RexxMsg and needs to know which
   one this answers.  payload_length is bounded by AMIGA_BROKER_MAX_PAYLOAD;
   anything larger is a protocol error, not a large message. */
struct amiga_broker_port_record {
    uint32_t magic;
    uint32_t operation;
    uint64_t message_id;
    uint64_t port_id;
    uint32_t payload_length;
    /* AMIGA_BROKER_PORT_HAS_*: which descriptors accompany this record as
       ancillary data. Named rather than counted, because either stream can be
       absent on its own. */
    uint32_t flags;
};

#endif
