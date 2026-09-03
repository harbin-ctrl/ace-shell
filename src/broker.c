#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
/* For struct ucred / SO_PEERCRED: the broker asks the kernel who is on the
   other end of a connection rather than believing a pid the caller sent. */
#define _GNU_SOURCE

#include "broker_dictionary.h"
#include "broker_protocol.h"
#include "clipboard_bridge.h"
#include "ace_fmm_client.h"
#include "dos_devices.h"
#include "ace_modes.h"

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define MAX_SESSIONS 64
#define MAX_ASSIGNS  64
#define MAX_ASSIGN_TARGETS 16
#define MAX_COMMAND_PATHS 32
#define MAX_VARS     128
#define MAX_NAME     64
#define MAX_VALUE    4096
#define MAX_LIST_RESULT AMIGA_BROKER_MAX_PAYLOAD
#define MAX_TASKS 256
/* Ports are claimed by name and are rarer than tasks: one per application
   that offers a command vocabulary, not one per running command. */
#define MAX_PORTS 128
/* Delivery channels are one per process, and a sender needs one as much as a
   port owner does, so this is sized against processes rather than ports. */
#define MAX_PORT_CHANNELS 256
#define MAX_BROADCAST_EVENTS 256
/* Messages sent but not yet replied to. A sender blocks in WaitPort() until
   its reply arrives, so this counts conversations in flight rather than
   messages ever sent. */
#define MAX_PORT_MESSAGES 256
#define MAX_SAY_VOICES 16
#define DEFAULT_FAIL_LEVEL 10
#define DEFAULT_PROMPT "%N.%S> "
#define AMIGA_COMPONENT_LIMIT 107
/* '~' is legal in a stored name, but AROS pattern syntax makes it a
 * negation operator. '^' remains an ordinary filename character there and
 * is still visually conspicuous on Linux. */
#define MAPPED_MARKER '^'
#define ASSIGN_DIRECTORY 1
#define ASSIGN_LATE 3
#define ASSIGN_NONBINDING 4

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

struct variable_entry {
    char name[MAX_NAME];
    char value[MAX_VALUE];
    uint32_t type;
};

struct assign_entry {
    char name[MAX_NAME];
    char root[PATH_MAX];
    char targets[MAX_ASSIGN_TARGETS][PATH_MAX];
    size_t target_count;
    int type;
    bool host_backed;
};

/* A Linux directory entry that cannot be carried safely through an AROS
 * pathname gets a short, visible spelling for the life of this broker. The
 * parent is part of the key: the same spelling may be used independently in
 * different directories. */

struct broker_session {
    bool in_use;
    /* Ordinal of the last request that touched this session, for reclaiming
     * the coldest one when every slot is taken. See get_session(). */
    uint64_t last_used;
    /* Connections that have claimed this session with ATTACH -- normally the
     * one shell the session belongs to, briefly two while a replacement
     * connection re-attaches. While this is non-zero the session has a live
     * owner: it is never reclaimed, and when it falls back to zero the
     * session is freed on the spot. See release_session(). */
    uint32_t anchors;
    char id[128];
    char cwd[PATH_MAX];
    char command_paths[MAX_COMMAND_PATHS][PATH_MAX];
    size_t command_path_count;
    struct assign_entry assigns[MAX_ASSIGNS];
    struct variable_entry local_vars[MAX_VARS];
    int32_t return_code;
    int32_t result2;
    int32_t fail_level;
    char prompt[MAX_VALUE];
    /* Operations this session needed the CRM to complete, since the last time
       the shell asked.  Counted here because the commands that perform them
       are separate processes: each one exits with what it knew.  See
       AMIGA_BROKER_TALLY. */
    uint32_t privileged_operations;
    int tally_enabled;
    pid_t foreground_pid;
    uint64_t foreground_task;
    uint32_t pending_foreground_signals;
};

static struct broker_session sessions[MAX_SESSIONS];

struct broker_task {
    uint64_t id;
    int fd;
    int session;
    pid_t pid;
    char name[MAX_NAME];
};

static struct broker_task tasks[MAX_TASKS];
static uint64_t next_task_id = 1;

/*
 * Public message ports.
 *
 * Deliberately a registry of names rather than of ports: the broker never
 * holds a struct MsgPort, which lives in the registering process's memory and
 * means nothing here. What it holds is the fact that a name is claimed, by
 * which process, over which connection -- enough for another process to ask
 * "who is REXX" and be told something it can then send to.
 *
 * Keyed the same way tasks are, and released the same way: when the
 * connection that registered a port goes away, so does the name. A process
 * that exits without calling DeletePort() must not leave a name behind that
 * resolves to a process which is no longer there, because the caller that
 * finds it has no way to tell.
 */
struct broker_port {
    uint64_t id;
    int fd;
    int session;
    pid_t pid;
    char name[MAX_NAME];
};

static struct broker_port ports[MAX_PORTS];
static uint64_t next_port_id = 1;

/*
 * Message delivery channels: where to push a message so that a process
 * blocked in WaitPort() actually receives it.
 *
 * Keyed by pid rather than by port, because that is the granularity the
 * client attaches at: one process, one channel, however many ports it owns
 * and whether or not it owns any. Routing a delivery is therefore two steps
 * -- port name to owning pid, pid to channel -- and the second step is what
 * makes a reply to a sender that owns no port possible at all.
 *
 * A channel is dropped when its connection closes, exactly as a port is. The
 * pid stays the key even so: a process that reconnects gets a new channel
 * with a new id, and nothing routes to the old one in between.
 */
struct broker_port_channel {
    uint64_t id;
    int fd;
    pid_t pid;
};

static struct broker_port_channel port_channels[MAX_PORT_CHANNELS];
static uint64_t next_port_channel_id = 1;

/* Resource registration is a process-wide ARexx state change. Existing
 * Regina processes receive it live; a Regina process started afterwards must
 * receive the accepted history when it attaches its delivery channel. The
 * broker does not inspect these bytes -- it retains opaque broadcast events
 * in order and replays them to the newcomer. RXADDLIB followed by RXREMLIB
 * therefore reconstructs the same final state as live delivery. */
struct broker_broadcast_event {
    void *payload;
    size_t length;
};

static struct broker_broadcast_event broadcast_events[MAX_BROADCAST_EVENTS];
static size_t broadcast_event_count;

/*
 * A message that has been delivered and not yet replied to.
 *
 * On AmigaOS this table does not exist: PutMsg() puts a pointer on a list and
 * ReplyMsg() puts the same pointer back, because both processes are looking
 * at the same memory. Here the two processes share nothing, so the broker
 * holds the correlation -- an id going out with the message and coming back
 * with the reply -- and the sender keeps its own struct RexxMsg to write the
 * results into when they arrive.
 *
 * The sender is recorded by connection rather than by pid so that a reply
 * goes to the exact process still waiting for it.
 */
struct broker_port_message {
    uint64_t id;
    uint64_t port_id;
    int sender_channel_fd;
    pid_t sender_pid;
    pid_t owner_pid;
};

static struct broker_port_message port_messages[MAX_PORT_MESSAGES];
static uint64_t next_port_message_id = 1;

/* Piper models are broker state. A `say` process is short-lived, while model
 * loading is expensive; keeping the child here shares one warm voice among
 * every ACE shell that uses this broker. */
struct broker_say_voice {
    char name[MAX_NAME];
    pid_t pid;
    time_t last_used;
};

static struct broker_say_voice say_voices[MAX_SAY_VOICES];
static struct variable_entry global_vars[MAX_VARS];
static int server_fd = -1;
static time_t broker_started;
/* Set from amiga_broker_socket_path(), or argv[1], as main() starts, before
 * anything (including the signal handlers) can read it. */
static const char *socket_path;

static unsigned long say_idle_seconds(void)
{
    const char *configured = getenv("ACE_SAY_IDLE_SECONDS");
    char *end;
    unsigned long seconds;

    if (!configured || !*configured)
        return 2ul * 60ul * 60ul;
    errno = 0;
    seconds = strtoul(configured, &end, 10);
    if (errno || *end || !seconds)
        return 2ul * 60ul * 60ul;
    return seconds;
}

static void reap_say_voices(void)
{
    time_t now = time(NULL);
    unsigned long idle = say_idle_seconds();

    for (size_t index = 0; index < MAX_SAY_VOICES; index++) {
        struct broker_say_voice *voice = &say_voices[index];
        int state;

        if (!voice->pid)
            continue;
        if (waitpid(voice->pid, &state, WNOHANG) == voice->pid) {
            fprintf(stderr, "ace-broker: say voice '%s' stopped\n", voice->name);
            memset(voice, 0, sizeof(*voice));
            continue;
        }
        if (now >= voice->last_used &&
            (unsigned long)(now - voice->last_used) >= idle) {
            fprintf(stderr, "ace-broker: stopping idle say voice '%s'\n",
                    voice->name);
            if (kill(-voice->pid, SIGTERM) != 0)
                (void)kill(voice->pid, SIGTERM);
            (void)waitpid(voice->pid, &state, 0);
            memset(voice, 0, sizeof(*voice));
        }
    }
}

static void stop_say_voices(void)
{
    for (size_t index = 0; index < MAX_SAY_VOICES; index++)
        if (say_voices[index].pid)
            if (kill(-say_voices[index].pid, SIGTERM) != 0)
                (void)kill(say_voices[index].pid, SIGTERM);
}

static int say_voice_server(char *result, size_t result_size)
{
    const char *configured = getenv("ACE_SAY_VOICE_SERVER");
    char executable[PATH_MAX];
    ssize_t length;
    char *slash;

    if (configured && *configured) {
        if (strlen(configured) >= result_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(result, configured);
        return 0;
    }
    length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0 || (size_t)length >= sizeof(executable) - 1)
        return -1;
    executable[length] = '\0';
    slash = strrchr(executable, '/');
    if (!slash || snprintf(result, result_size, "%.*s/say-voice-server",
                           (int)(slash - executable), executable) >=
                      (int)result_size) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

static bool valid_say_voice(const char *name)
{
    if (!name || !*name || strlen(name) >= MAX_NAME)
        return false;
    for (; *name; name++)
        if (!isalnum((unsigned char)*name) && *name != '-' && *name != '_')
            return false;
    return true;
}

static int warm_say_voice(const char *name)
{
    struct broker_say_voice *slot = NULL;
    char server[PATH_MAX];
    pid_t child;

    if (!valid_say_voice(name)) {
        errno = EINVAL;
        return -1;
    }
    reap_say_voices();
    for (size_t index = 0; index < MAX_SAY_VOICES; index++) {
        if (say_voices[index].pid && strcmp(say_voices[index].name, name) == 0) {
            say_voices[index].last_used = time(NULL);
            return 0;
        }
        if (!say_voices[index].pid && !slot)
            slot = &say_voices[index];
    }
    if (!slot) {
        errno = ENOSPC;
        return -1;
    }
    if (say_voice_server(server, sizeof(server)) != 0)
        return -1;
    child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        (void)setsid();
        execl(server, server, name, (char *)NULL);
        _exit(127);
    }
    strcpy(slot->name, name);
    slot->pid = child;
    slot->last_used = time(NULL);
    return 0;
}

static int say_poll_timeout(void)
{
    time_t now = time(NULL);
    time_t next = now + (time_t)say_idle_seconds();

    for (size_t index = 0; index < MAX_SAY_VOICES; index++)
        if (say_voices[index].pid &&
            say_voices[index].last_used + (time_t)say_idle_seconds() < next)
            next = say_voices[index].last_used + (time_t)say_idle_seconds();
    if (next <= now)
        return 0;
    if (next - now > INT_MAX / 1000)
        return INT_MAX;
    return (int)((next - now) * 1000);
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const char *bytes = buffer;
    while (length) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!written)
            return -1;
        bytes += written;
        length -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, void *buffer, size_t length)
{
    char *bytes = buffer;
    while (length) {
        ssize_t received = read(fd, bytes, length);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!received)
            return -1;
        bytes += received;
        length -= (size_t)received;
    }
    return 0;
}

/*
 * Reads a whole message, distinguishing a peer that closed cleanly between
 * requests from one that failed mid-message. With one request per connection
 * that distinction did not exist -- every end of input was the end of the
 * exchange -- but a held connection ends with exactly this, and a normal
 * disconnect must not be reported as an error.
 *
 * Returns 1 for a complete message, 0 for a clean close, -1 for a failure.
 */
static int read_message(int fd, void *buffer, size_t length)
{
    char *bytes = buffer;
    size_t received = 0;

    while (received < length) {
        ssize_t chunk = read(fd, bytes + received, length - received);

        if (chunk < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!chunk)
            return received ? -1 : 0;
        received += (size_t)chunk;
    }
    return 1;
}

/*
 * Reads a record and collects any descriptors that came with it.
 *
 * The broker never looks at what a passed descriptor refers to; it hands it
 * on to the process the message is for. Only the first read can carry
 * ancillary data, so it is the only one that needs recvmsg.
 */
static int read_message_with_fds(int fd, void *buffer, size_t length,
                                 int *fds, size_t max_fds, size_t *fd_count)
{
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int) * AMIGA_BROKER_PORT_MAX_FDS)];
    } control;
    struct msghdr message;
    struct iovec vector;
    struct cmsghdr *header;
    ssize_t received;

    *fd_count = 0;
    memset(&control, 0, sizeof(control));
    memset(&message, 0, sizeof(message));
    vector.iov_base = buffer;
    vector.iov_len = length;
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    do {
        received = recvmsg(fd, &message, 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0)
        return -1;
    if (!received)
        return 0;
    for (header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
        size_t count;

        if (header->cmsg_level != SOL_SOCKET ||
            header->cmsg_type != SCM_RIGHTS)
            continue;
        count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t index = 0; index < count; index++) {
            int passed;

            memcpy(&passed, CMSG_DATA(header) + index * sizeof(int),
                   sizeof(passed));
            if (*fd_count < max_fds)
                fds[(*fd_count)++] = passed;
            else
                close(passed);
        }
    }
    if ((size_t)received < length)
        return read_message(fd, (char *)buffer + received,
                            length - received) == 1 ? 1 : -1;
    return 1;
}

static uint64_t session_clock;

static struct broker_session *touch_session(struct broker_session *session)
{
    if (session)
        session->last_used = ++session_clock;
    return session;
}

/* Gives a session's slot back. Called when the last connection that claimed
   the session closes, whether its shell exited, was killed, or crashed. */
static void release_session(struct broker_session *session)
{
    memset(session, 0, sizeof(*session));
}

/*
 * Finds a session by name, creating it if it is new.
 *
 * A session that a shell has claimed with ATTACH has a lifetime the broker
 * knows exactly, and is freed the moment its last connection closes, so it
 * is never a candidate here.
 *
 * That leaves sessions nobody claimed -- created by a standalone command or
 * by brokerctl, which come and go as separate processes and expect their
 * session to outlive each of them. Nothing can say when those are finished
 * with, so they keep the older rule: a full table gives up its coldest
 * unclaimed slot rather than refusing, since the alternative was that the
 * sixty-fifth distinct session got ENOSPC forever after and every ACE window
 * opened from then on died on the spot. What has changed is that this can no
 * longer take a live shell's session away from underneath it.
 */
/*
 * SYS: -- the boot volume. On a real machine dos.library locks the volume it
 * was booted from; here the equivalent question is which installation of ACE
 * this broker belongs to. The compiled-in answer is the install prefix, the
 * environment can override it for a test run, and a broker that finds
 * neither falls back to its own directory, which in a build tree is where
 * the commands are.
 */
static char system_root[PATH_MAX];
/*
 * The session's fmm, or NULL for an ordinary unprivileged session.
 *
 * Deliberately not closed from the signal handler.  Closing it politely means
 * sending a message and waiting, which a handler must not do; letting the
 * process exit closes the descriptor, and the fmm treats that EOF as the
 * end of the session.  It is the same path a crashed broker takes, so it is
 * the path that has to work anyway.
 */
static struct ace_privilege_connection *fmm;
/* The broker owns the CRM channel as well as the FMM channel.  Keeping both
 * handles here makes their common --root-shell lifetime explicit. */
static struct ace_privilege_connection *crm;
static uint32_t root_shells;

/*
 * The privileged services belong to the set of live --root shells, not to
 * the broker process itself.  The transition from no such shells to one is
 * the one time an authentication request may be made; further shells reuse
 * the already-authorised pair.
 */
static int start_root_services(void)
{
    if (!ace_mode_is_root() || fmm)
        return 0;
    fmm = ace_fmm_start(ACE_PRIVILEGE_CAP_VOLUME |
                        ACE_PRIVILEGE_CAP_ACCESS);
    if (!fmm)
        return -1;
    if (ace_dos_devices_prepare_device_view(fmm) == 0)
        return 0;
    ace_privilege_connection_close(fmm);
    fmm = NULL;
    ace_dos_devices_shutdown();
    return -1;
}

/* Close the CRM first: its EOF is its unambiguous stop signal.  The FMM then
 * shuts down its private mount namespace. */
static void stop_root_services(void)
{
    if (crm) {
        ace_privilege_connection_close(crm);
        crm = NULL;
    }
    if (fmm) {
        ace_privilege_connection_close(fmm);
        fmm = NULL;
    }
    ace_dos_devices_shutdown();
}

static struct assign_entry *allocate_assign(struct broker_session *session,
                                            const char *name);

static bool is_directory(const char *path)
{
    struct stat information;

    return path && *path && stat(path, &information) == 0 &&
           S_ISDIR(information.st_mode);
}

static bool is_regular_file(const char *path)
{
    struct stat information;

    return path && *path && stat(path, &information) == 0 &&
           S_ISREG(information.st_mode);
}

/* mkdir -p for a path ACE owns. */
static bool make_directory_path(const char *path)
{
    char work[PATH_MAX];
    size_t length = strlen(path);

    if (length >= sizeof(work))
        return false;
    strcpy(work, path);
    for (char *cursor = work + 1; *cursor; cursor++) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(work, 0700) != 0 && errno != EEXIST)
            return false;
        *cursor = '/';
    }
    return (mkdir(work, 0700) == 0 || errno == EEXIST) && is_directory(work);
}

static void resolve_system_root(void)
{
    /* Shared with every client through broker_protocol.h: the socket this
       broker listens on is named after this root, so a client that resolved
       it differently would look for its broker somewhere else entirely. */
    const char *root = amiga_broker_system_root();

    if (strlen(root) < sizeof(system_root))
        strcpy(system_root, root);
    else
        strcpy(system_root, "/");
}

/*
 * The volatile half of the boot assigns. AROS puts ENV: and T: in RAM:,
 * which it can name because the RAM disk is a fixed part of the system. ACE
 * names its tmpfs mounts RAM:, RAM1: ... in host mount order, so which one
 * is "the" RAM disk is an accident of how this machine happens to be
 * mounted, and no script could portably name it. The host's own per-user
 * runtime directory is the same thing said properly: tmpfs, private, and
 * emptied between boots.
 */
static void volatile_root(char *result, size_t result_size)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");

    if (runtime && *runtime &&
        (size_t)snprintf(result, result_size, "%s/ace", runtime) < result_size)
        return;
    snprintf(result, result_size, "/tmp/ace-%lu", (unsigned long)getuid());
}

static void set_directory_assign(struct broker_session *session,
                                 const char *name, const char *path)
{
    struct assign_entry *assign;

    if (!is_directory(path))
        return;
    assign = allocate_assign(session, name);
    if (!assign || strlen(path) >= sizeof(assign->root))
        return;
    memset(assign->targets, 0, sizeof(assign->targets));
    strcpy(assign->targets[0], path);
    assign->target_count = 1;
    strcpy(assign->root, path);
    assign->type = ASSIGN_DIRECTORY;
    assign->host_backed = true;
}

/*
 * AddBootAssign() in rom/dos/cliinit.c: the drawer if it is there, and SYS:
 * itself if it is not. A system with nothing to put in L: still answers for
 * L:, which is why an unmodified program can open "LIBS:foo" on any Amiga
 * and get a missing file rather than a missing device.
 */
static void add_boot_assign(struct broker_session *session, const char *name,
                            const char *drawer)
{
    char path[PATH_MAX];

    if ((size_t)snprintf(path, sizeof(path), "%s/%s", system_root, drawer) <
        sizeof(path) && is_directory(path))
        set_directory_assign(session, name, path);
    else
        set_directory_assign(session, name, system_root);
}

/*
 * What dos.library establishes before the first shell runs. It has to happen
 * here rather than in a script, for the same reason it is C code in AROS:
 * the shell cannot find the script that would make these assigns, or the
 * Assign command that would make them, until they exist.
 */
static void seed_boot_assigns(struct broker_session *session)
{
    char volatile_path[PATH_MAX];
    char path[PATH_MAX];
    char clips_path[PATH_MAX];

    set_directory_assign(session, "SYS", system_root);
    add_boot_assign(session, "C", "C");
    add_boot_assign(session, "LIBS", "Libs");
    add_boot_assign(session, "DEVS", "Devs");
    add_boot_assign(session, "L", "L");
    add_boot_assign(session, "S", "S");
    add_boot_assign(session, "FONTS", "Fonts");

    /* ENVARC: is the saved half of the environment and lives with the
       installation; ENV: is the live half and does not survive a reboot. */
    if ((size_t)snprintf(path, sizeof(path), "%s/Prefs/Env-Archive",
                         system_root) < sizeof(path)) {
        (void)make_directory_path(path);
        set_directory_assign(session, "ENVARC", path);
    }
    volatile_root(volatile_path, sizeof(volatile_path));
    if ((size_t)snprintf(path, sizeof(path), "%s/env", volatile_path) <
        sizeof(path)) {
        (void)make_directory_path(path);
        set_directory_assign(session, "ENV", path);
    }
    if ((size_t)snprintf(path, sizeof(path), "%s/t", volatile_path) <
        sizeof(path)) {
        (void)make_directory_path(path);
        set_directory_assign(session, "T", path);
    }
    if (ace_clipboard_store_prepare() == 0 &&
        ace_clipboard_store_root(clips_path, sizeof(clips_path)) == 0)
        set_directory_assign(session, "CLIPS", clips_path);
}

/*
 * The Startup-Sequence's `Copy ENVARC: ENV: ALL`, done here because ACE has
 * no Copy command to do it with yet, and because a broker is the closest
 * thing ACE has to a boot: this runs once, when the broker starts, and the
 * live environment it produces is then shared by every session.
 */
static void restore_environment_archive(void)
{
    char archive[PATH_MAX];
    char live[PATH_MAX];
    char volatile_path[PATH_MAX];
    struct dirent *entry;
    DIR *stream;

    if ((size_t)snprintf(archive, sizeof(archive), "%s/Prefs/Env-Archive",
                         system_root) >= sizeof(archive))
        return;
    volatile_root(volatile_path, sizeof(volatile_path));
    if ((size_t)snprintf(live, sizeof(live), "%s/env", volatile_path) >=
        sizeof(live) || !make_directory_path(live))
        return;
    stream = opendir(archive);
    if (!stream)
        return;
    while ((entry = readdir(stream))) {
        char from[PATH_MAX];
        char to[PATH_MAX];
        char buffer[4096];
        ssize_t count;
        int source;
        int target;

        if (entry->d_name[0] == '.')
            continue;
        if ((size_t)snprintf(from, sizeof(from), "%s/%s", archive,
                             entry->d_name) >= sizeof(from) ||
            (size_t)snprintf(to, sizeof(to), "%s/%s", live, entry->d_name) >=
            sizeof(to))
            continue;
        if (!is_regular_file(from))
            continue;
        source = open(from, O_RDONLY);
        if (source < 0)
            continue;
        target = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (target < 0) {
            close(source);
            continue;
        }
        while ((count = read(source, buffer, sizeof(buffer))) > 0) {
            ssize_t written = 0;

            while (written < count) {
                ssize_t step = write(target, buffer + written,
                                     (size_t)(count - written));

                if (step <= 0)
                    break;
                written += step;
            }
            if (written < count)
                break;
        }
        close(source);
        close(target);
    }
    closedir(stream);
}

static struct broker_session *get_session(const char *id)
{
    struct broker_session *free_slot = NULL;
    struct broker_session *coldest = NULL;

    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].in_use && strcmp(sessions[i].id, id) == 0)
            return touch_session(&sessions[i]);
        if (!sessions[i].in_use) {
            if (!free_slot)
                free_slot = &sessions[i];
        } else if (sessions[i].anchors == 0 &&
                   (!coldest || sessions[i].last_used < coldest->last_used)) {
            coldest = &sessions[i];
        }
    }
    if (!free_slot) {
        free_slot = coldest;
        if (free_slot)
            fprintf(stderr, "ace-broker: session table full, reclaiming the "
                            "least recently used session '%s' for '%s'\n",
                    free_slot->id, id);
    }
    if (!free_slot || strlen(id) >= sizeof(free_slot->id))
        return NULL;

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->in_use = true;
    strcpy(free_slot->id, id);
    if (!getcwd(free_slot->cwd, sizeof(free_slot->cwd)))
        strcpy(free_slot->cwd, "/");
    free_slot->fail_level = DEFAULT_FAIL_LEVEL;
    strcpy(free_slot->prompt, DEFAULT_PROMPT);
    seed_boot_assigns(free_slot);
    return touch_session(free_slot);
}

static struct broker_session *find_session(const char *id)
{
    for (size_t i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].in_use && strcmp(sessions[i].id, id) == 0)
            return touch_session(&sessions[i]);
    return NULL;
}

/*
 * Amiga names for host names that have none.
 *
 * The escape is  <header>^<base32>  and it is a pure function of the host
 * name: nothing is stored, so a name means the same thing in every broker, on
 * every machine, and after every restart.  The previous scheme drew a random
 * suffix per broker process and remembered it in RAM, which meant a name died
 * with the broker that invented it.
 *
 * base32 rather than base64 because AmigaDOS filesystems are case-insensitive,
 * so an alphabet that distinguishes 'A' from 'a' would let two different
 * encodings name one file and silently overwrite each other.  RFC 4648's
 * A-Z2-7 has no such pair, and every character of it is already legal in an
 * ACE component.  It costs about 3% against base36 and is bit-packing rather
 * than bignum arithmetic.
 *
 * Which of the two forms an escape is saying is carried by the last byte of
 * the payload, which is the whole trick:
 *
 *   ends in NUL   the payload is the rest of the host name, literally.
 *                 Decoding reconstructs it; no lookup, no directory scan,
 *                 nothing to store or keep in step.
 *   ends nonzero  the host name was too long to fit, so the payload is a
 *                 hash of it.  A hash cannot be inverted, and is not: the
 *                 parent is scanned and each entry encoded forward until one
 *                 matches, the way a password is checked rather than
 *                 recovered.
 */
#define BASE32_ALPHABET "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"

static size_t base32_encoded_length(size_t bytes)
{
    return (bytes * 8 + 4) / 5;
}

static size_t base32_encode(const unsigned char *data, size_t length,
                            char *result, size_t result_size)
{
    uint32_t accumulator = 0;
    unsigned bits = 0;
    size_t used = 0;

    for (size_t index = 0; index < length; index++) {
        accumulator = (accumulator << 8) | data[index];
        bits += 8;
        while (bits >= 5) {
            if (used + 1 >= result_size)
                return 0;
            result[used++] = BASE32_ALPHABET[(accumulator >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits) {
        if (used + 1 >= result_size)
            return 0;
        /* Trailing bits are left-aligned and zero-filled, as RFC 4648 does
           before it pads; ACE omits the '=' padding because '=' is not a
           legal ACE component character and the length is recoverable. */
        result[used++] = BASE32_ALPHABET[(accumulator << (5 - bits)) & 31];
    }
    result[used] = '\0';
    return used;
}

static int base32_decode(const char *text, unsigned char *result,
                         size_t result_size, size_t *result_length)
{
    uint32_t accumulator = 0;
    unsigned bits = 0;
    size_t used = 0;

    for (const char *cursor = text; *cursor; cursor++) {
        unsigned char character = (unsigned char)*cursor;
        const char *position;

        if (character >= 'a' && character <= 'z')
            character = (unsigned char)(character - 'a' + 'A');
        position = strchr(BASE32_ALPHABET, character);
        if (!position || !character)
            return -1;
        accumulator = (accumulator << 5) | (uint32_t)(position - BASE32_ALPHABET);
        bits += 5;
        if (bits >= 8) {
            if (used >= result_size)
                return -1;
            result[used++] = (unsigned char)((accumulator >> (bits - 8)) & 0xff);
            bits -= 8;
        }
    }
    /* Whatever is left is the zero fill the encoder added; anything set in it
       means the text was not produced by base32_encode(). */
    if (bits >= 5 || (accumulator & ((1u << bits) - 1)))
        return -1;
    *result_length = used;
    return used ? 0 : -1;
}

/*
 * Compression, for the tail a literal escape still cannot fit.
 *
 * Two engines, chosen by one bit -- COMPRESS_PACK39 or COMPRESS_DEFLATE below
 * -- because two is what turned out to be worth having.  An earlier version
 * of this idea carried four (adding Brotli and Unishox2) and reserved a
 * sixteen-symbol Latin-1 header alphabet to name them; measured against the
 * real corpus this is tuned on, the two here alone already reach the same
 * result the four did, so the other two, and the header space it took to
 * tell four apart, were paying for nothing.
 *
 *   COMPRESS_PACK39   direct radix-39 bit-packing of the alphabet
 *                     [a-z0-9_.-] -- no header, no per-block overhead,
 *                     which is what lets it beat general compression on
 *                     short strings.  Build-system-generated names are
 *                     almost always made of exactly this alphabet.
 *   COMPRESS_DEFLATE  raw DEFLATE (zlib, no zlib/gzip wrapper) for
 *                     anything outside it -- mixed case, accented bytes,
 *                     punctuation the direct pack does not cover.
 *
 * Tried only after the plain literal escape has already failed, and only on
 * the tail component_split_point() chose to keep -- not the whole name.
 * Compressing the header too was tried and measured worse: those bytes are
 * already free (one raw character each), and compression's per-string
 * overhead usually costs more than a few already-short bytes can save.
 *
 * DEFLATE runs with a preset dictionary -- AMIGA_BROKER_DEFLATE_DICTIONARY
 * in broker_dictionary.h -- because most names this tier ever sees are too
 * short to build their own back-reference history: a single 90-byte name
 * carries little of its own repetition to exploit, but it very often
 * shares vocabulary with countless other names a compressor never gets to
 * see, one file at a time. Priming the window with that shared vocabulary
 * up front is what closes the gap.
 *
 * Measured against the full Debian 12 package archive's file index
 * (Contents-amd64.gz, 913,356 unique basenames, fetched and tested
 * independently of wherever this idea first came from): 258 basenames
 * exceed AMIGA_COMPONENT_LIMIT outright. Without the dictionary, 160 of
 * those round-trip through this tier; with it, 213 -- and every one of the
 * 258 was checked for regressions from adding the dictionary, since a
 * preset dictionary only ever gives the compressor more to reference, never
 * less, so no name can round-trip worse for having one. Zero did. See
 * tests/filesystem_translation_test.sh.
 */
enum {
    COMPRESS_PACK39 = 0,
    COMPRESS_DEFLATE = 1,
};

/* The trailer byte a compressed payload ends in: distinct from 0x00, the
   literal form's terminator, and checked as a second, independent proof of
   which form this is -- see compressed_payload_decode(). */
#define COMPRESSED_TRAILER(engine) (unsigned char)(0x80 | (engine))

/*
 * The literal escape stays on base32 (5 bits/character) -- it is what every
 * ':' and every case collision already uses, and changing it would touch
 * every escaped name ACE has ever produced, not just the rare over-length
 * one.  The compressed tail has no such history, and base32's cost is what
 * was actually limiting this tier: 106 base32 characters buy only 66
 * compressed bytes, and a name that compresses to, say, 88 bytes -- a real
 * measured case -- was refused for want of encoding, not compression.
 *
 * DENSE128 packs at 7 bits/character instead, 128 symbols chosen the same
 * way base32's alphabet was: never two of them differing only by
 * AmigaDOS's case fold.  Lowercase ASCII is excluded entirely, and so is
 * every Latin-1 codepoint that has one (0xE0-0xFE); ß (0xDF) has no single
 * Latin-1 uppercase codepoint and is caseless here.  What is left is ASCII
 * punctuation and uppercase letters, Latin-1 symbols (0xA1-0xBF), and
 * Latin-1 uppercase-or-caseless letters (0xC0-0xDF) -- 65 + 31 + 32 = 128,
 * with ':', '/', '^' and space left out as everywhere else in this file.
 *
 * A compressed payload cannot be confused with a literal one because it is
 * never handed to the same decoder: '0' cannot open a base32 literal (RFC
 * 4648 excludes 0/1/8/9 to keep them visually distinct from O/I/B/S), so
 * "^0" is reserved, structurally, to mean "what follows is DENSE128", the
 * same way "^" itself is reserved to open an escape at all.  This is a
 * guarantee, not a probability -- unlike telling the two payload forms
 * inside base32 apart by their last byte, which is exactly why that trick
 * is not reused here.
 */
static const unsigned char DENSE128_ALPHABET[128] = {
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x30, 0x31,
    0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
    0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52,
    0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
    0x5b, 0x5c, 0x5d, 0x5f, 0x60, 0x7b, 0x7c, 0x7d,
    0x7e, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
};

static int dense128_value(unsigned char character)
{
    for (int index = 0; index < 128; index++)
        if (DENSE128_ALPHABET[index] == character)
            return index;
    return -1;
}

static size_t dense128_encoded_length(size_t bytes)
{
    return (bytes * 8 + 6) / 7;
}

static size_t dense128_encode(const unsigned char *data, size_t length,
                              char *result, size_t result_size)
{
    uint32_t accumulator = 0;
    unsigned bits = 0;
    size_t used = 0;

    for (size_t index = 0; index < length; index++) {
        accumulator = (accumulator << 8) | data[index];
        bits += 8;
        while (bits >= 7) {
            if (used + 1 >= result_size)
                return 0;
            result[used++] = (char)DENSE128_ALPHABET[(accumulator >> (bits - 7)) & 0x7f];
            bits -= 7;
        }
    }
    if (bits) {
        if (used + 1 >= result_size)
            return 0;
        result[used++] = (char)DENSE128_ALPHABET[(accumulator << (7 - bits)) & 0x7f];
    }
    result[used] = '\0';
    return used;
}

static int dense128_decode(const char *text, unsigned char *result,
                           size_t result_size, size_t *result_length)
{
    uint32_t accumulator = 0;
    unsigned bits = 0;
    size_t used = 0;

    for (const unsigned char *cursor = (const unsigned char *)text; *cursor;
         cursor++) {
        int value = dense128_value(*cursor);

        if (value < 0)
            return -1;
        accumulator = (accumulator << 7) | (uint32_t)value;
        bits += 7;
        if (bits >= 8) {
            if (used >= result_size)
                return -1;
            result[used++] = (unsigned char)((accumulator >> (bits - 8)) & 0xff);
            bits -= 8;
        }
    }
    if (bits >= 7 || (accumulator & ((1u << bits) - 1)))
        return -1;
    *result_length = used;
    return used ? 0 : -1;
}

static const char PACK39_ALPHABET[] = "abcdefghijklmnopqrstuvwxyz0123456789_.-";

static int pack39_value(char character)
{
    const char *position = character ? strchr(PACK39_ALPHABET, character) : NULL;

    return position ? (int)(position - PACK39_ALPHABET) : -1;
}

static bool pack39_encodable(const char *text)
{
    if (!*text)
        return false;
    for (; *text; text++)
        if (pack39_value(*text) < 0)
            return false;
    return true;
}

/* [orig_len][big-endian base-256 digits of the base-39 value]. orig_len is
   what tells the decoder when to stop dividing; without it, trailing zero
   digits (e.g. "aa" and "a") would be indistinguishable. */
static bool pack39_encode(const char *text, unsigned char *result,
                          size_t result_size, size_t *result_length)
{
    unsigned char digits[NAME_MAX + 2];
    size_t digit_count = 0;
    size_t text_length = strlen(text);

    if (!text_length || text_length > NAME_MAX)
        return false;
    for (size_t index = 0; index < text_length; index++) {
        int value = pack39_value(text[index]);
        uint32_t carry;

        if (value < 0)
            return false;
        carry = (uint32_t)value;
        for (size_t digit = 0; digit < digit_count; digit++) {
            uint32_t product = (uint32_t)digits[digit] * 39 + carry;

            digits[digit] = (unsigned char)(product & 0xff);
            carry = product >> 8;
        }
        while (carry) {
            if (digit_count >= sizeof(digits))
                return false;
            digits[digit_count++] = (unsigned char)(carry & 0xff);
            carry >>= 8;
        }
    }
    if (1 + digit_count > result_size)
        return false;
    result[0] = (unsigned char)text_length;
    for (size_t index = 0; index < digit_count; index++)
        result[1 + index] = digits[digit_count - 1 - index];
    *result_length = 1 + digit_count;
    return true;
}

static bool pack39_decode(const unsigned char *data, size_t length,
                          char *text, size_t text_size)
{
    unsigned char digits[NAME_MAX + 2];
    size_t digit_count;
    size_t orig_length;
    char reversed[NAME_MAX + 1];
    size_t produced = 0;

    if (length < 1)
        return false;
    orig_length = data[0];
    if (!orig_length || orig_length > NAME_MAX || orig_length >= text_size)
        return false;
    digit_count = length - 1;
    if (digit_count > sizeof(digits))
        return false;
    memcpy(digits, data + 1, digit_count);

    for (size_t position = 0; position < orig_length; position++) {
        uint32_t remainder = 0;
        size_t new_count = 0;

        for (size_t digit = 0; digit < digit_count; digit++) {
            uint32_t current = (remainder << 8) | digits[digit];
            uint32_t quotient = current / 39;

            remainder = current % 39;
            if (quotient || new_count)
                digits[new_count++] = (unsigned char)quotient;
        }
        digit_count = new_count;
        if (produced >= sizeof(reversed))
            return false;
        reversed[produced++] = PACK39_ALPHABET[remainder];
    }
    /* Anything left over is magnitude orig_length digits did not account
       for -- not a value pack39_encode() could have produced. */
    if (digit_count != 0)
        return false;
    for (size_t index = 0; index < produced; index++)
        text[index] = reversed[produced - 1 - index];
    text[produced] = '\0';
    return true;
}

/*
 * Both directions set the dictionary immediately after init and before the
 * first deflate()/inflate() call, not in response to Z_NEED_DICT: that
 * signal is a zlib-wrapper feature (the FDICT bit and Adler-32 in a zlib
 * header), and this is raw deflate (negative windowBits, no header) so it
 * is never produced. zlib's own documentation says as much: for raw
 * inflate, the dictionary must be set right after inflateInit2().
 */
static bool deflate_compress(const unsigned char *data, size_t length,
                             unsigned char *result, size_t result_size,
                             size_t *result_length)
{
    z_stream stream;
    int status;

    memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 9,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    if (deflateSetDictionary(&stream, (const Bytef *)AMIGA_BROKER_DEFLATE_DICTIONARY,
                             AMIGA_BROKER_DEFLATE_DICTIONARY_LENGTH) != Z_OK) {
        deflateEnd(&stream);
        return false;
    }
    stream.next_in = (Bytef *)data;
    stream.avail_in = (uInt)length;
    stream.next_out = (Bytef *)result;
    stream.avail_out = (uInt)result_size;
    status = deflate(&stream, Z_FINISH);
    *result_length = stream.total_out;
    deflateEnd(&stream);
    return status == Z_STREAM_END;
}

static bool deflate_decompress(const unsigned char *data, size_t length,
                               char *text, size_t text_size)
{
    unsigned char buffer[NAME_MAX + 1];
    z_stream stream;
    int status;
    size_t produced;

    memset(&stream, 0, sizeof(stream));
    if (inflateInit2(&stream, -15) != Z_OK)
        return false;
    if (inflateSetDictionary(&stream, (const Bytef *)AMIGA_BROKER_DEFLATE_DICTIONARY,
                             AMIGA_BROKER_DEFLATE_DICTIONARY_LENGTH) != Z_OK) {
        inflateEnd(&stream);
        return false;
    }
    stream.next_in = (Bytef *)data;
    stream.avail_in = (uInt)length;
    stream.next_out = buffer;
    stream.avail_out = (uInt)sizeof(buffer);
    status = inflate(&stream, Z_FINISH);
    produced = stream.total_out;
    inflateEnd(&stream);
    if (status != Z_STREAM_END || produced >= text_size)
        return false;
    memcpy(text, buffer, produced);
    text[produced] = '\0';
    return true;
}

/*
 * Decodes a "^0..." compressed suffix (the caller has already consumed the
 * '0'), and is also the validity check for one: called with scratch output
 * and its result discarded, from escape_payload_valid(), the same way the
 * literal form's validity check never needs a second copy of its own rules.
 *
 * The DENSE128 payload's own last byte selects the engine -- still checked
 * against COMPRESSED_TRAILER's exact values rather than merely "nonzero",
 * because a structural guarantee costs nothing extra here and the marker
 * already carries the main one.
 *
 * Both engines are proven correct here by encoding the decoded candidate
 * back and comparing, not merely trusted to have decoded cleanly. PACK39
 * has no failure mode of its own -- any byte string decodes to some text --
 * so this was already the only real guarantee available. DEFLATE used to
 * be trusted on inflate() reaching Z_STREAM_END alone, on the reasoning
 * that its bitstream format is self-checking; that stopped being enough
 * once a preset dictionary was added, because inflate() succeeding no
 * longer proves the *dictionary* used to decode this payload was the one
 * used to encode it. Raw deflate carries no checksum of its own to catch
 * that mismatch, and AMIGA_BROKER_DEFLATE_DICTIONARY is frozen precisely so
 * this check never needs to fire in practice -- but a name that somehow
 * did encode against a different dictionary must fail predictably here,
 * not decode to something plausible-looking and wrong.
 */
static bool compressed_payload_decode(const char *suffix, char *text,
                                      size_t text_size)
{
    unsigned char payload[NAME_MAX + 2];
    unsigned char verify[NAME_MAX + 2];
    size_t length = 0;
    size_t verify_length;
    unsigned char trailer;

    if (dense128_decode(suffix, payload, sizeof(payload), &length) != 0 ||
        length < 1)
        return false;
    trailer = payload[length - 1];
    if (trailer == COMPRESSED_TRAILER(COMPRESS_PACK39)) {
        if (!pack39_decode(payload, length - 1, text, text_size))
            return false;
        return pack39_encodable(text) &&
               pack39_encode(text, verify, sizeof(verify), &verify_length) &&
               verify_length == length - 1 &&
               memcmp(verify, payload, verify_length) == 0;
    }
    if (trailer == COMPRESSED_TRAILER(COMPRESS_DEFLATE)) {
        if (!deflate_decompress(payload, length - 1, text, text_size))
            return false;
        return deflate_compress((const unsigned char *)text, strlen(text),
                                verify, sizeof(verify), &verify_length) &&
               verify_length == length - 1 &&
               memcmp(verify, payload, verify_length) == 0;
    }
    return false;
}

/*
 * Does the text after a '^' actually spell a payload this file could have
 * produced?
 *
 * This is what keeps '^' an ordinary character almost everywhere.  A name
 * like "a^b.txt" is left alone, because "b.txt" is not base32 and so the
 * caret cannot be the start of an escape; only a caret followed by something
 * that really does decode has to be escaped to keep the marker unambiguous.
 *
 * Two forms, told apart before either decoder even runs, by the first
 * character:
 *
 *   '0'    compressed: what follows is DENSE128, ending in a trailer byte
 *          that names the engine.  '0' cannot open a valid base32 stream --
 *          RFC 4648 excludes 0/1/8/9 from that alphabet -- so this is a
 *          structural fact, not a coincidence to be probabilistically ruled
 *          out; see the note above DENSE128_ALPHABET.
 *   other  literal: base32, as it always was.  "Decodes as base32" is not
 *          by itself enough -- the decoded bytes must end in their own NUL
 *          and contain no other, or they are not that name's own
 *          terminator.
 *
 * Both directions ask this same question, which is the point: whatever
 * map_component() declines to escape, unmap_component() must decline to
 * decode, or a name would not survive the round trip.
 */
static bool escape_payload_valid(const char *suffix)
{
    unsigned char payload[NAME_MAX + 2];
    char scratch[NAME_MAX + 2];
    size_t length = 0;

    if (suffix[0] == '0')
        return compressed_payload_decode(suffix + 1, scratch, sizeof(scratch));
    if (base32_decode(suffix, payload, sizeof(payload), &length) != 0)
        return false;
    if (payload[length - 1] != '\0')
        return false;
    for (size_t index = 0; index + 1 < length; index++)
        if (!payload[index])
            return false;
    return true;
}

/* The caret that opens an escape, or NULL.  Not simply the first caret: in
   "a^b^<payload>" the first one is an ordinary character and the second is
   the marker. */
static const char *escape_marker(const char *name)
{
    for (const char *cursor = name; *cursor; cursor++)
        if (*cursor == MAPPED_MARKER && escape_payload_valid(cursor + 1))
            return cursor;
    return NULL;
}

/* Where a name stops being spellable, or SIZE_MAX if it never does. */
static size_t first_unspellable_index(const char *name)
{
    for (size_t index = 0; name[index]; index++) {
        if (name[index] == ':' || name[index] == '/')
            return index;
        if (name[index] == MAPPED_MARKER &&
            escape_payload_valid(name + index + 1))
            return index;
    }
    return SIZE_MAX;
}

/*
 * AmigaDOS takes very nearly every name Linux can produce.  Spaces, '+',
 * '#', '*', quotes, accented and non-ASCII bytes are all ordinary filename
 * characters; the pattern metacharacters among them need quoting when they
 * appear in a pattern, which is equally true on a real Amiga and is not a
 * reason to rename anybody's files.  So the mapper stays out of the way
 * unless the name genuinely cannot be spelled:
 *
 *   ':'   separates a volume from the path that follows it.
 *   '/'   separates path components.  A Linux filename can never contain
 *         one, but the check costs nothing and says what the rule is.
 *   '^'   introduces one of these escapes -- but only when what follows it
 *         really does spell a payload.  A caret in "a^b.txt" is just a
 *         caret; see escape_payload_valid().
 *   too long for a FileInfoBlock component.
 *
 * A case collision is the fifth trigger and cannot be seen from the name
 * alone -- it depends on what else is in the directory -- so
 * host_case_variant_needs_mapping() decides that one separately.
 */
static bool component_needs_mapping(const char *name)
{
    return strlen(name) > AMIGA_COMPONENT_LIMIT ||
           first_unspellable_index(name) != SIZE_MAX;
}





/* Linux permits several spellings which AmigaDOS would regard as the same
 * component.  Keep the lexical first one as the ordinary Amiga spelling and
 * make every other one go through the visible ^ escape mapping. */
static bool host_case_variant_needs_mapping(const char *parent,
                                            const char *host_name)
{
    DIR *stream;
    struct dirent *entry;
    char first[NAME_MAX + 1];
    bool collision = false;
    bool found = false;

    stream = opendir(parent);
    if (!stream)
        return false;
    while ((entry = readdir(stream)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcasecmp(entry->d_name, host_name) != 0)
            continue;
        if (!found || strcmp(entry->d_name, first) < 0) {
            strcpy(first, entry->d_name);
            found = true;
        }
        if (strcmp(entry->d_name, host_name) != 0)
            collision = true;
    }
    closedir(stream);
    return collision && strcmp(host_name, first) != 0;
}

/*
 * AmigaDOS treats these as ordinary component names, while Linux reserves
 * them for its own traversal. Colon cannot occur in an AmigaDOS component,
 * but is an ordinary Linux filename byte, so the two spellings are a stable,
 * collision-free bridge. This is deliberately before the broker-lifetime
 * escape mapping: ':' and '::' have these meanings on every ACE volume, not
 * a synthetic spelling that varies with the broker session.
 */
static const char *amiga_component_for_host(const char *host_name)
{
    if (strcmp(host_name, ":") == 0)
        return ".";
    if (strcmp(host_name, "::") == 0)
        return "..";
    return NULL;
}

static const char *host_component_for_amiga(const char *amiga_name)
{
    if (strcmp(amiga_name, ".") == 0)
        return ":";
    if (strcmp(amiga_name, "..") == 0)
        return "::";
    return NULL;
}

/*
 * Where the literal part of a name has to stop.
 *
 * Everything before the split is kept as-is and everything from it on is
 * encoded, so the split wants to be as late as possible: a byte costs one
 * character in the header and 1.6 inside the payload.  The first character
 * that has no Amiga spelling is therefore the natural place.
 *
 * A case collision has no such character -- "notes.txt" is perfectly legal in
 * itself, and only impossible next to "Notes.txt" -- so the split is the
 * first byte where the colliding siblings actually differ.  That matters for
 * more than tidiness: with three variants of one name, splitting any later
 * would give two of them identical payloads and headers differing only in
 * case, which on a case-insensitive filesystem is one file, not two.
 */
static size_t component_split_point(const char *parent, const char *host_name)
{
    size_t length = strlen(host_name);
    size_t split = length;
    DIR *stream;
    struct dirent *entry;

    if (first_unspellable_index(host_name) < split)
        split = first_unspellable_index(host_name);

    stream = opendir(parent);
    if (!stream)
        return split;
    while ((entry = readdir(stream)) != NULL) {
        size_t index = 0;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, host_name) == 0 ||
            strcasecmp(entry->d_name, host_name) != 0)
            continue;
        while (entry->d_name[index] && entry->d_name[index] == host_name[index])
            index++;
        if (index < split)
            split = index;
    }
    closedir(stream);
    return split;
}

static int map_component(const char *parent, const char *host_name,
                         char *result, size_t result_size)
{
    unsigned char payload[NAME_MAX + 2];
    char encoded[AMIGA_COMPONENT_LIMIT + 1];
    char candidate[AMIGA_COMPONENT_LIMIT + 1];
    const char *fixed_name;
    size_t host_length = strlen(host_name);
    size_t split;
    size_t tail_length;
    size_t encoded_length;

    fixed_name = amiga_component_for_host(host_name);
    if (fixed_name) {
        if (strlen(fixed_name) >= result_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(result, fixed_name);
        return 0;
    }
    if (!component_needs_mapping(host_name) &&
        !host_case_variant_needs_mapping(parent, host_name)) {
        if (host_length >= result_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(result, host_name);
        return 0;
    }

    split = component_split_point(parent, host_name);
    if (split > host_length)
        split = host_length;
    tail_length = host_length - split;

    /* Literal form: the rest of the host name, and its own NUL to say so. */
    if (tail_length + 1 <= sizeof(payload)) {
        memcpy(payload, host_name + split, tail_length);
        payload[tail_length] = '\0';
        encoded_length = base32_encoded_length(tail_length + 1);
        if (split + 1 + encoded_length <= AMIGA_COMPONENT_LIMIT &&
            base32_encode(payload, tail_length + 1, encoded, sizeof(encoded))) {
            if ((size_t)snprintf(candidate, sizeof(candidate), "%.*s%c%s",
                                 (int)split, host_name, MAPPED_MARKER,
                                 encoded) < sizeof(candidate)) {
                if (strlen(candidate) >= result_size) {
                    errno = ENAMETOOLONG;
                    return -1;
                }
                strcpy(result, candidate);
                return 0;
            }
        }
    }

    /*
     * The literal escape does not fit.  Before giving up, try compressing
     * just the tail component_split_point() chose -- not the whole name,
     * which was tried and measured worse; see the comment on
     * compressed_payload_decode() and its neighbours above.
     *
     * component_split_point() can return the whole name as header with
     * nothing left to reduce -- no unspellable byte, no case collision,
     * simply too long -- which leaves no tail to compress at all.  There is
     * no character- or case-driven reason to keep any of it as header in
     * that case, so this tier starts over with the whole name as its tail.
     */
    {
        char tail_text[NAME_MAX + 2];
        unsigned char compressed[NAME_MAX + 2];
        unsigned char best[NAME_MAX + 2];
        size_t compress_split = split;
        size_t compress_tail_length = tail_length;
        size_t best_length = 0;
        bool have_best = false;

        if (!compress_tail_length) {
            compress_split = 0;
            compress_tail_length = host_length;
        }
        if (compress_tail_length > NAME_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(tail_text, host_name + compress_split, compress_tail_length);
        tail_text[compress_tail_length] = '\0';

        if (pack39_encodable(tail_text)) {
            size_t candidate_length;

            if (pack39_encode(tail_text, compressed, sizeof(compressed) - 1,
                              &candidate_length)) {
                have_best = true;
                best_length = candidate_length;
                memcpy(best, compressed, candidate_length);
                best[best_length++] = COMPRESSED_TRAILER(COMPRESS_PACK39);
            }
        }
        {
            size_t candidate_length;

            if (deflate_compress((const unsigned char *)tail_text,
                                 compress_tail_length, compressed,
                                 sizeof(compressed) - 1, &candidate_length) &&
                (!have_best || candidate_length + 1 < best_length)) {
                have_best = true;
                best_length = candidate_length;
                memcpy(best, compressed, candidate_length);
                best[best_length++] = COMPRESSED_TRAILER(COMPRESS_DEFLATE);
            }
        }

        if (have_best) {
            /* +2, not +1: the marker and the '0' that says DENSE128 comes
               next -- see the note above DENSE128_ALPHABET. */
            encoded_length = dense128_encoded_length(best_length);
            if (compress_split + 2 + encoded_length <= AMIGA_COMPONENT_LIMIT &&
                dense128_encode(best, best_length, encoded, sizeof(encoded))) {
                if ((size_t)snprintf(candidate, sizeof(candidate), "%.*s%c0%s",
                                     (int)compress_split, host_name,
                                     MAPPED_MARKER, encoded) <
                    sizeof(candidate)) {
                    if (strlen(candidate) >= result_size) {
                        errno = ENAMETOOLONG;
                        return -1;
                    }
                    strcpy(result, candidate);
                    return 0;
                }
            }
        }
    }

    /*
     * It does not fit, compressed or not, and nothing can make it fit: even
     * a name that compresses cannot be guaranteed into 106 base32
     * characters, because there are more possible names than payloads.  A
     * scheme that fails on some names has to; one that fails predictably is
     * worth more than one that fails on a subset nobody can characterise.
     *
     * Reached by 45 of the Debian corpus's 258 over-length basenames -- the
     * rest are caught by the tier above.  These 45 are dominated by hash
     * digests: high-entropy by construction, so neither PACK39 (a radix
     * conversion, not an entropy coder) nor a dictionary (which can only
     * help where content recurs) has anything to work with.
     */
    errno = ENAMETOOLONG;
    return -1;
}

/*
 * An Amiga filesystem matches a name without regard to case: `dir`, `Dir` and
 * `DIR` name the same thing, which is why an Amiga user types commands in
 * whatever case they please. Linux does not, so a name that does not exist
 * is looked for among the entries that are there.
 *
 * A name being created -- which exists nowhere yet -- keeps the spelling it
 * was given. Where a directory holds several entries differing only in case,
 * something an Amiga filesystem could not have contained in the first place,
 * the lexical first is its ordinary spelling.  The other spellings are
 * available through the explicit ^ mappings made by map_component().
 */
static void match_existing_case(const char *parent, char *name,
                                size_t name_size)
{
    char best[NAME_MAX + 1];
    struct dirent *entry;
    DIR *stream;
    int found = 0;

    stream = opendir(parent);
    if (!stream)
        return;
    while ((entry = readdir(stream))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (strcasecmp(entry->d_name, name) != 0)
            continue;
        if (strlen(entry->d_name) >= sizeof(best))
            continue;
        if (!found || strcmp(entry->d_name, best) < 0) {
            strcpy(best, entry->d_name);
            found = 1;
        }
    }
    closedir(stream);
    if (found && strlen(best) < name_size)
        strcpy(name, best);
}

static int unmap_component(const char *parent, const char *amiga_name,
                           char *result, size_t result_size)
{
    unsigned char payload[NAME_MAX + 2];
    size_t payload_length = 0;
    const char *fixed_name = host_component_for_amiga(amiga_name);
    const char *marker;
    size_t header_length;

    if (fixed_name) {
        if (strlen(fixed_name) >= result_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(result, fixed_name);
        return 0;
    }

    /* Not merely the first caret -- the first one that opens an escape.  A
       caret the mapper left alone, because what followed it was not a
       payload, is an ordinary character and belongs to the header. */
    marker = escape_marker(amiga_name);
    if (!marker) {
        if (strlen(amiga_name) >= result_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(result, amiga_name);
        match_existing_case(parent, result, result_size);
        return 0;
    }
    header_length = (size_t)(marker - amiga_name);

    if (marker[1] == '0') {
        /* Compressed: the host name is the header and the decompressed
           tail.  escape_marker() already proved this decodes, by calling
           the same function; doing it again here is the one place that
           proof gets spent, not duplicated for its own sake. */
        char tail_text[NAME_MAX + 2];

        if (compressed_payload_decode(marker + 2, tail_text,
                                      sizeof(tail_text))) {
            size_t tail_length = strlen(tail_text);

            if (header_length + tail_length >= result_size) {
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(result, amiga_name, header_length);
            memcpy(result + header_length, tail_text, tail_length);
            result[header_length + tail_length] = '\0';
            return 0;
        }
    } else if (base32_decode(marker + 1, payload, sizeof(payload) - 1,
                             &payload_length) == 0 &&
               payload[payload_length - 1] == '\0') {
        /* Literal: the host name is the header and the decoded remainder. */
        if (header_length + payload_length - 1 >= result_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(result, amiga_name, header_length);
        memcpy(result + header_length, payload, payload_length - 1);
        result[header_length + payload_length - 1] = '\0';
        return 0;
    }

    /*
     * Not an escape this side can read.  Hand the name back as given so the
     * caller reports a missing file rather than a broken translation; there
     * is no second form to go looking for, because a name that does not fit
     * is refused at the point it is encoded rather than given a synthetic
     * spelling that only resolves next to its own file.
     */
    if (strlen(amiga_name) >= result_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(result, amiga_name);
    return 0;
}

static int append_path_text(char *result, size_t result_size,
                            size_t *used, const char *text)
{
    int written = snprintf(result + *used, result_size - *used, "%s%s",
                           *used && result[*used - 1] != ':' ? "/" : "",
                           text);

    if (written < 0 || (size_t)written >= result_size - *used) {
        errno = ENAMETOOLONG;
        return -1;
    }
    *used += (size_t)written;
    return 0;
}

static int normalize_path(const char *base, const char *path,
                          char *result, size_t result_size);

/* Convert the device layer's ordinary volume spelling into the AROS-facing
 * spelling, mapping each unsafe host component against its host parent. The
 * host and Amiga suffixes normally have the same component structure; the
 * host parent is what makes each mapping directory-specific. */
static int name_from_host_with_mappings(const char *path, char *result,
                                        size_t result_size)
{
    char raw[PATH_MAX];
    char canonical[PATH_MAX];
    char volume_root[PATH_MAX];
    char host_parent[PATH_MAX];
    const char *raw_cursor;
    const char *host_cursor;
    char *colon;
    size_t used;

    if (ace_dos_devices_name_from_path(path, raw, sizeof(raw)) != 0)
        return -1;
    if (normalize_path("/", path, canonical, sizeof(canonical)) != 0 ||
        ace_dos_devices_volume_root_for_path(canonical, volume_root,
                                             sizeof(volume_root)) != 0)
        return -1;
    colon = strchr(raw, ':');
    if (!colon) {
        errno = EINVAL;
        return -1;
    }
    used = (size_t)(colon - raw) + 1;
    if (used >= result_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(result, raw, used);
    result[used] = '\0';

    if (strlen(volume_root) >= sizeof(host_parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(host_parent, volume_root);
    host_cursor = canonical + strlen(volume_root);
    while (*host_cursor == '/')
        host_cursor++;
    raw_cursor = colon + 1;
    while (*raw_cursor == '/')
        raw_cursor++;

    while (*raw_cursor) {
        char raw_component[PATH_MAX];
        char host_component[NAME_MAX + 1];
        char mapped_component[AMIGA_COMPONENT_LIMIT + 1];
        const char *raw_slash = strchr(raw_cursor, '/');
        const char *host_slash = strchr(host_cursor, '/');
        size_t raw_length = raw_slash ? (size_t)(raw_slash - raw_cursor) :
                                        strlen(raw_cursor);
        size_t host_length = host_slash ? (size_t)(host_slash - host_cursor) :
                                          strlen(host_cursor);

        if (!raw_length || raw_length >= sizeof(raw_component)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(raw_component, raw_cursor, raw_length);
        raw_component[raw_length] = '\0';
        if (!host_length || host_length >= sizeof(host_component)) {
            /* This can only happen for a filesystem root alias whose
             * spelling contains a synthetic component not present in the
             * host mount path. Preserve it literally. */
            strcpy(host_component, raw_component);
        } else {
            memcpy(host_component, host_cursor, host_length);
            host_component[host_length] = '\0';
        }
        if (map_component(host_parent, host_component, mapped_component,
                          sizeof(mapped_component)) != 0)
            return -1;
        if (append_path_text(result, result_size, &used, mapped_component) != 0)
            return -1;
        if (strlen(host_parent) + strlen(host_component) + 2 >=
            sizeof(host_parent)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (strcmp(host_parent, "/") != 0)
            strcat(host_parent, "/");
        strcat(host_parent, host_component);
        raw_cursor = raw_slash ? raw_slash + 1 : raw_cursor + raw_length;
        host_cursor = host_slash ? host_slash + 1 : host_cursor + host_length;
        while (*raw_cursor == '/')
            raw_cursor++;
        while (*host_cursor == '/')
            host_cursor++;
    }
    return 0;
}

static int normalize_path(const char *base, const char *path,
                          char *result, size_t result_size)
{
    char combined[PATH_MAX * 2];
    char *parts[PATH_MAX / 2];
    size_t part_count = 0;
    char *cursor;
    int written;

    if (path[0] == '/')
        written = snprintf(combined, sizeof(combined), "%s", path);
    else
        written = snprintf(combined, sizeof(combined), "%s/%s", base, path);
    if (written < 0 || (size_t)written >= sizeof(combined)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    cursor = combined;
    while (*cursor) {
        char *slash = strchr(cursor, '/');
        if (slash)
            *slash = '\0';
        if (*cursor && strcmp(cursor, ".") != 0) {
            if (strcmp(cursor, "..") == 0) {
                if (part_count)
                    part_count--;
            } else if (part_count < sizeof(parts) / sizeof(parts[0])) {
                parts[part_count++] = cursor;
            } else {
                errno = ENAMETOOLONG;
                return -1;
            }
        }
        if (!slash)
            break;
        cursor = slash + 1;
    }

    if (result_size < 2) {
        errno = ENAMETOOLONG;
        return -1;
    }
    result[0] = '/';
    result[1] = '\0';
    for (size_t i = 0; i < part_count; i++) {
        size_t current = strlen(result);
        written = snprintf(result + current, result_size - current,
                           "%s%s", current > 1 ? "/" : "", parts[i]);
        if (written < 0 || (size_t)written >= result_size - current) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }
    return 0;
}

static void pop_host_component(char *path)
{
    char *slash;

    if (strcmp(path, "/") == 0)
        return;
    slash = strrchr(path, '/');
    if (!slash || slash == path)
        strcpy(path, "/");
    else
        *slash = '\0';
}

/* Like normalize_path(), but resolves broker-created component spellings as
 * the path is walked. The parent host path is therefore available for every
 * reverse-map lookup; a component token is never interpreted globally. */
static int normalize_mapped_path(const char *base, const char *path,
                                 char *result, size_t result_size)
{
    char combined[PATH_MAX * 2];
    char *cursor;
    int written;

    written = snprintf(combined, sizeof(combined), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(combined)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (path[0] == '/')
        strcpy(result, "/");
    else {
        if (strlen(base) >= result_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        /* normalize_amiga_path() resolves a path one component at a time and
           passes the buffer it is building as both the base and the result,
           so this is regularly a copy onto itself -- which strcpy(), whose
           parameters are restrict-qualified, does not allow.  Harmless with
           this libc and reported by valgrind on every component. */
        if (result != base)
            strcpy(result, base);
    }

    cursor = combined;
    while (*cursor) {
        char *slash = strchr(cursor, '/');
        char decoded[PATH_MAX];

        if (slash)
            *slash = '\0';
        if (*cursor) {
            if (unmap_component(result, cursor, decoded,
                                sizeof(decoded)) != 0)
                return -1;
            size_t current = strlen(result);

            written = snprintf(result + current, result_size - current,
                               "%s%s", current > 1 ? "/" : "", decoded);
            if (written < 0 || (size_t)written >= result_size - current) {
                errno = ENAMETOOLONG;
                return -1;
            }
        }
        if (!slash)
            break;
        cursor = slash + 1;
    }
    return 0;
}

/*
 * An Amiga volume is one filesystem. Linux will happily mount a second
 * filesystem partway down a first one, but on this side that mount is a
 * separate volume with a name of its own, so resolution must stop at the
 * boundary rather than walk through it: "sda2:proc" is procfs reached
 * through the wrong volume, and PROC: is how procfs is addressed. Nothing
 * becomes unreachable, because every mount the broker knows about is
 * published under its own name in the DOS device list.
 *
 * The comparison is against the nearest ancestor that exists, because the
 * caller may be naming an object it is about to create -- MakeDir() and
 * Open() for write both resolve a path before there is anything there.
 *
 * The mount point's own directory is refused along with everything under
 * it. That directory really does belong to this volume, and its contents as
 * this volume sees them are whatever the mount obscures, but an unprivileged
 * process cannot read underneath a mount: the kernel locks that view
 * deliberately. Refusing is the honest answer until ACE can see through it.
 */
static int within_base_filesystem(const char *base, const char *path)
{
    struct stat base_info;
    struct stat probe_info;
    char probe[PATH_MAX];

    if (stat(base, &base_info) != 0)
        return -1;
    if (strlen(path) >= sizeof(probe)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(probe, path);
    while (stat(probe, &probe_info) != 0) {
        char *slash;

        /*
         * A refusal is not a crossing.
         *
         * This walk exists to catch a path that has silently left its volume,
         * and it answers that by comparing devices.  When the broker is not
         * allowed to look, it has not found a crossing -- it has found out
         * that it cannot tell, and refusing the name on that basis would end
         * the operation here, before the seam that exists to retry it with
         * privilege.  A directory the user cannot enter is exactly where an
         * authorised session is supposed to be able to work.
         *
         * Nothing is granted by continuing.  The name is still only a name:
         * the unprivileged attempt that follows is bounded by the same
         * permissions that refused this stat, and the privileged one resolves
         * under the worker's own beneath-this-root constraint.
         */
        if (errno != ENOENT && errno != ENOTDIR && errno != EACCES &&
            errno != EPERM)
            return -1;
        slash = strrchr(probe, '/');
        if (!slash || slash == probe)
            return 0;
        *slash = '\0';
        /* Trimmed back to the volume root: same filesystem by definition. */
        if (strcmp(probe, base) == 0)
            return 0;
    }
    if (probe_info.st_dev != base_info.st_dev) {
        /* Device view deliberately has no nested mounts beneath a bindroot.
         * A different device here can therefore only have been reached by
         * following a soft link, whose absolute target keeps ordinary Linux
         * meaning and is represented in ACE by that target device's name. */
        if (ace_dos_devices_is_full_root(base))
            return 0;
        /* From this volume's point of view the object is simply not here,
           which is what ERROR_OBJECT_NOT_FOUND says. Reporting a crossing
           would describe a Linux arrangement AmigaDOS has no word for. */
        errno = ENOENT;
        return -1;
    }
    return 0;
}

static int normalize_mapped_path_beneath(const char *base, const char *path,
                                         char *result, size_t result_size)
{
    size_t base_length;

    if (normalize_mapped_path(base, path, result, result_size) != 0)
        return -1;
    if (strcmp(base, "/") != 0) {
        base_length = strlen(base);
        if (strcmp(result, base) != 0 &&
            (strncmp(result, base, base_length) != 0 ||
             result[base_length] != '/')) {
            errno = EACCES;
            return -1;
        }
    }
    return within_base_filesystem(base, result);
}

static struct assign_entry *find_assign(struct broker_session *session,
                                         const char *name)
{
    for (size_t i = 0; i < MAX_ASSIGNS; i++)
        if (session->assigns[i].name[0] &&
            strcasecmp(session->assigns[i].name, name) == 0)
            return &session->assigns[i];
    return NULL;
}

static struct assign_entry *allocate_assign(struct broker_session *session,
                                             const char *name)
{
    struct assign_entry *entry = find_assign(session, name);

    if (entry)
        return entry;
    for (size_t i = 0; i < MAX_ASSIGNS; i++) {
        if (!session->assigns[i].name[0]) {
            if (strlen(name) >= sizeof(session->assigns[i].name))
                return NULL;
            memset(&session->assigns[i], 0, sizeof(session->assigns[i]));
            strcpy(session->assigns[i].name, name);
            return &session->assigns[i];
        }
    }
    return NULL;
}

static struct variable_entry *find_variable(struct variable_entry *variables,
                                            const char *name, uint32_t type)
{
    for (size_t i = 0; i < MAX_VARS; i++)
        if (variables[i].name[0] && strcasecmp(variables[i].name, name) == 0)
            if (type == 0 || variables[i].type == type)
                return &variables[i];
    return NULL;
}

static struct variable_entry *allocate_variable(struct variable_entry *variables,
                                                const char *name, uint32_t type)
{
    struct variable_entry *entry = find_variable(variables, name, type);

    if (entry)
        return entry;
    for (size_t i = 0; i < MAX_VARS; i++) {
        if (!variables[i].name[0]) {
            if (strlen(name) >= sizeof(variables[i].name))
                return NULL;
            strcpy(variables[i].name, name);
            variables[i].type = type;
            return &variables[i];
        }
    }
    return NULL;
}

static int variable_scope(uint32_t flags, bool *local, bool *global)
{
    *local = (flags & AMIGA_BROKER_VAR_LOCAL) != 0;
    *global = (flags & AMIGA_BROKER_VAR_GLOBAL) != 0;
    if (!*local && !*global)
        *local = *global = true;
    return *local && *global &&
           (flags & (AMIGA_BROKER_VAR_LOCAL | AMIGA_BROKER_VAR_GLOBAL)) != 0;
}

static struct variable_entry *lookup_variable(struct broker_session *session,
                                              const char *name, uint32_t flags)
{
    bool local, global;
    struct variable_entry *entry;
    uint32_t type = (flags & AMIGA_BROKER_VAR_ALIAS) ?
                    AMIGA_BROKER_VAR_ALIAS : AMIGA_BROKER_VAR_VARIABLE;

    if (variable_scope(flags, &local, &global))
        return NULL;
    if (local) {
        entry = find_variable(session->local_vars, name, type);
        if (entry)
            return entry;
    }
    return global ? find_variable(global_vars, name, type) : NULL;
}

static int append_variable_list(char *result, size_t result_size, size_t *used,
                                const struct variable_entry *variables,
                                uint32_t flags)
{
    bool aliases = (flags & AMIGA_BROKER_VAR_ALIAS) != 0;
    bool any = (flags & AMIGA_BROKER_VAR_ANY) != 0;

    for (size_t i = 0; i < MAX_VARS; i++) {
        int written;
        if (!variables[i].name[0])
            continue;
        if (!any && ((variables[i].type == AMIGA_BROKER_VAR_ALIAS) != aliases))
            continue;
        written = snprintf(result + *used, result_size - *used,
                           "%u\t%s\n", variables[i].type == AMIGA_BROKER_VAR_ALIAS,
                           variables[i].name);
        if (written < 0 || (size_t)written >= result_size - *used)
            return -1;
        *used += (size_t)written;
    }
    return 0;
}

static int normalize_amiga_path(struct broker_session *session,
                                 const char *path, char *result,
                                 size_t result_size)
{
    char floor[PATH_MAX];
    const char *cursor = path;
    size_t slashes = 0;

    while (*cursor == '/') {
        slashes++;
        cursor++;
    }
    if (strlen(session->cwd) >= result_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(result, session->cwd);
    /* AmigaDOS uses each leading slash as a parent traversal: / is the
       parent and // is the grandparent. An empty relative path is the
       current directory, rather than inventing a POSIX-style dot component. */
    for (size_t index = 0; index < slashes; index++)
        pop_host_component(result);
    while (*cursor) {
        const char *end = strchr(cursor, '/');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        char component[PATH_MAX];

        if (length != 0) {
            if (length >= sizeof(component)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(component, cursor, length);
            component[length] = '\0';
            if (normalize_mapped_path(result, component, result,
                                      result_size) != 0)
                return -1;
        }
        if (!end)
            break;
        cursor = end;
        slashes = 0;
        while (*cursor == '/') {
            slashes++;
            cursor++;
        }
        /* The first slash separates components. Any remaining slashes walk
           back up from the component just resolved. */
        for (size_t index = 1; index < slashes; index++)
            pop_host_component(result);
    }
    if (ace_dos_devices_volume_root_for_path(session->cwd, floor,
                                              sizeof(floor)) == 0 &&
        normalize_mapped_path_beneath(floor, result, result, result_size) != 0) {
        int failure = errno;
        char volume_name[PATH_MAX];
        char covered[PATH_MAX];
        char *colon;
        const char *relative;

        /* The explicit-volume resolver below already retries a covered
         * directory through this volume's private device root.  A relative
         * path from that same volume is the same AmigaDOS question, so it
         * must receive the same answer. */
        if (failure == ENOENT &&
            ace_dos_devices_name_from_path(floor, volume_name,
                                            sizeof(volume_name)) == 0 &&
            (colon = strchr(volume_name, ':')) != NULL) {
            *colon = '\0';
            relative = result + strlen(floor);
            while (*relative == '/')
                relative++;
            if (ace_dos_devices_device_view_root(volume_name, covered,
                                                  sizeof(covered)) == 0 &&
                strcmp(covered, floor) != 0)
                return normalize_mapped_path_beneath(covered, relative,
                                                     result, result_size);
        }
        errno = failure;
        return -1;
    }
    return 0;
}

static int resolve_path(struct broker_session *session, const char *input,
                        char *result, size_t result_size, bool host_path);

/*
 * Whether the caller may open what it just asked to have resolved.
 *
 * Answered here, before anything is opened, because for these objects the
 * attempt is the damage.  A FIFO with no writer blocks in open() and the
 * block ignores SIGINT, so Ctrl-C cannot end it and the shell is wedged until
 * something kills it.  A character device opens instantly and then never
 * ends: Type runs forever and Copy fills the disk it is writing to.  A socket
 * cannot be opened at all and says so with an errno nobody can read.
 *
 * The entry itself stays entirely real.  Only a caller that said it was about
 * to open gets this answer, so Lock(), Examine(), Rename() and Delete() go on
 * working on it exactly as they do on anything else -- which they must, since
 * the object is there and is named.
 *
 * ENXIO is what Linux itself answers when a socket or an absent device is
 * opened, and the DOS seam turns it into ERROR_OBJECT_WRONG_TYPE: AmigaDOS's
 * own sentence for "not the kind of object you can do that to".  A stat that
 * fails is not an answer, so it does not produce one -- the object may simply
 * be somewhere this process cannot look, and the open that follows will
 * escalate and find out.
 */
static int refuse_unopenable_object(const char *path)
{
    struct stat information;

    if (stat(path, &information) != 0)
        return 0;
    if (S_ISREG(information.st_mode) || S_ISDIR(information.st_mode))
        return 0;
    return ENXIO;
}

static int resolve_assign_target(struct broker_session *session,
                                 struct assign_entry *assign,
                                 char *result, size_t result_size)
{
    char candidate[PATH_MAX];
    size_t count = assign->target_count ? assign->target_count : 1;

    for (size_t index = 0; index < count; index++) {
        const char *target = assign->target_count ?
                             assign->targets[index] : assign->root;

        if (!assign->host_backed) {
            if (resolve_path(session, target, candidate, sizeof(candidate),
                             false) != 0)
                continue;
        } else if (snprintf(candidate, sizeof(candidate), "%s", target) >=
                   (int)sizeof(candidate)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (access(candidate, F_OK) != 0)
            continue;
        if (snprintf(result, result_size, "%s", candidate) >=
            (int)result_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (assign->type == ASSIGN_LATE) {
            assign->type = ASSIGN_DIRECTORY;
        }
        return 0;
    }
    errno = ENOENT;
    return -1;
}

static int canonical_assign_target(struct broker_session *session,
                                   const char *value, char *logical,
                                   size_t logical_size, char *host,
                                   size_t host_size)
{
    struct stat information;

    if (resolve_path(session, value, host, host_size, false) != 0 ||
        stat(host, &information) != 0) {
        if (!errno)
            errno = ENOTDIR;
        return -1;
    }
    /* Reached without any call having failed, so errno holds whatever the
       last unrelated one left there rather than a reason for this. */
    if (!S_ISDIR(information.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return name_from_host_with_mappings(host, logical, logical_size);
}

static int resolve_path(struct broker_session *session, const char *input,
                        char *result, size_t result_size, bool host_path)
{
    const char *colon = strchr(input, ':');
    char base[PATH_MAX];
    const char *relative = input;

    if (host_path)
        return normalize_path(session->cwd, input, result, result_size);

    /* AROS treats a lone leading colon as the current volume, not as a
       relative filename.  GetDeviceProc(":") starts with the current
       directory's volume root; the text after the colon is then resolved
       beneath that root.  Keeping this here, at the volume-dispatch seam,
       makes the rule apply consistently to Lock(), Open(), MakeDir(), and
       every other DOS operation that accepts a path. */
    if (colon == input) {
        char volume_name[PATH_MAX];
        char covered[PATH_MAX];
        int failure;

        if (ace_dos_devices_volume_root_for_path(session->cwd, base,
                                                 sizeof(base)) != 0)
            return -1;
        relative = colon + 1;
        while (*relative == '/')
            relative++;
        if (normalize_mapped_path_beneath(base, relative, result,
                                          result_size) == 0)
            return 0;

        /* This is the same volume-relative question as "VOLUME:path".
           The first lookup is necessarily as the user; if a nested Linux
           mount covers the named directory, retry it through this volume's
           CRM-provided device view.  Returning directly from this branch
           used to skip that second step, making ":dev" fail while plain
           "dev" from the volume root succeeded. */
        failure = errno;
        if (failure == ENOENT &&
            ace_dos_devices_name_from_path(base, volume_name,
                                            sizeof(volume_name)) == 0) {
            char *name_end = strchr(volume_name, ':');

            if (name_end) {
                *name_end = '\0';
                if (ace_dos_devices_device_view_root(volume_name, covered,
                                                      sizeof(covered)) == 0 &&
                    strcmp(covered, base) != 0)
                    return normalize_mapped_path_beneath(covered, relative,
                                                         result, result_size);
            }
        }
        errno = failure;
        return -1;
    }

    if (colon && colon != input) {
        char assign_name[MAX_NAME];
        size_t name_length = (size_t)(colon - input);
        if (name_length < sizeof(assign_name)) {
            memcpy(assign_name, input, name_length);
            assign_name[name_length] = '\0';
            struct assign_entry *assign = find_assign(session, assign_name);
            /* CLIPS: is a boot-time virtual assign backed by the shared
             * clipboard store.  A session can outlive the broker revision
             * that created it, so repair this one special assign lazily when
             * an older session has lost it.  This keeps DeleteFile("CLIPS:n")
             * and clipboard.device pointed at the same unit instead of
             * silently resolving to a host filename named "CLIPS:n". */
            if (!assign && strcasecmp(assign_name, "CLIPS") == 0) {
                char clips_path[PATH_MAX];

                if (ace_clipboard_store_prepare() == 0 &&
                    ace_clipboard_store_root(clips_path,
                                             sizeof(clips_path)) == 0)
                    set_directory_assign(session, "CLIPS", clips_path);
                assign = find_assign(session, assign_name);
            }
            if (assign) {
                char clips_component[32];

                relative = colon + 1;
                while (*relative == '/')
                    relative++;
                if (resolve_assign_target(session, assign, base,
                                          sizeof(base)) != 0)
                    return -1;
                /* The real CLIPS: handler presents unit names as 0..255,
                 * while ACE keeps the backing files visibly separate from
                 * its lock files as clip0..clip255. Keep that spelling
                 * translation at the DOS handler seam so ordinary clients
                 * continue to use CLIPS:7. */
                if (strcasecmp(assign_name, "CLIPS") == 0 && *relative) {
                    char *end;
                    unsigned long unit;

                    errno = 0;
                    unit = strtoul(relative, &end, 10);
                    if (!errno && *end == '\0' && unit < 256) {
                        snprintf(clips_component, sizeof(clips_component),
                                 "clip%lu", unit);
                        relative = clips_component;
                    }
                }
                return normalize_mapped_path_beneath(base, relative, result,
                                                     result_size);
            }
            switch (ace_dos_devices_lookup(assign_name)) {
            case 1: {
                char covered[PATH_MAX];
                int failure;

                if (ace_dos_devices_root(assign_name, base, sizeof(base)) != 0)
                    return -1;
                relative = colon + 1;
                while (*relative == '/')
                    relative++;
                if (normalize_mapped_path_beneath(base, relative, result,
                                                  result_size) == 0)
                    return 0;
                /*
                 * Missing from the host's tree is not always missing from the
                 * device.  Something mounted over a directory hides what is
                 * underneath, and what is underneath is still this volume's:
                 * AmigaDOS has no word for a mount covering a directory, so
                 * the object simply has to be there.  The view holds this
                 * device mounted once with nothing nested inside it, which is
                 * the only place the covered object can be seen.
                 *
                 * Tried second, never first.  A path that resolved on the
                 * host resolves there, and stays an ordinary path the user
                 * can open without asking anyone.
                 */
                failure = errno;
                if (failure == ENOENT &&
                    ace_dos_devices_device_view_root(assign_name, covered,
                                                     sizeof(covered)) == 0 &&
                    strcmp(covered, base) != 0)
                    return normalize_mapped_path_beneath(covered, relative,
                                                         result, result_size);
                errno = failure;
                return -1;
            }
            case -1:
                errno = EEXIST;
                return -1;
            default:
                /*
                 * A name with a colon names a device, a volume or an assign,
                 * and this one names none of them.  Saying so is the whole
                 * answer, because AmigaDOS has no second reading available: a
                 * colon cannot appear in a filename, so "NAME:" is never
                 * something else worth trying.
                 *
                 * Falling through to resolve it against the current directory
                 * invented a file nobody had asked for.  "Copy x TO NIL:"
                 * made a drawer called "NIL:" and put a real copy of x inside
                 * it; "Type something >NIL:" tried to write into that drawer
                 * and failed with a raw EISDIR.  Every unimplemented device
                 * behaved that way, quietly, and the quiet is the problem --
                 * an unmounted device is an ordinary thing to ask about and
                 * has an ordinary answer.
                 *
                 * Only where the colon comes before any slash, which is the
                 * only place AmigaDOS reads one as a device.
                 */
                if (!memchr(input, '/', name_length)) {
                    errno = ENODEV;
                    return -1;
                }
                break;
            }
        }
    }
    return normalize_amiga_path(session, relative, result, result_size);
}

static int canonical_command_path(struct broker_session *session,
                                  const char *input, char *result,
                                  size_t result_size)
{
    char resolved[PATH_MAX];
    struct stat information;
    char canonical[PATH_MAX];

    if (!input || !*input ||
        resolve_path(session, input, resolved, sizeof(resolved), false) != 0)
        return -1;
    if (stat(resolved, &information) != 0)
        return -1;
    if (!S_ISDIR(information.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    if (realpath(resolved, canonical)) {
        if (strlen(canonical) >= sizeof(resolved)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(resolved, canonical);
    }
    if (strlen(resolved) >= result_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(result, resolved);
    return 0;
}

static size_t find_command_path(struct broker_session *session,
                                const char *path)
{
    for (size_t index = 0; index < session->command_path_count; index++)
        if (strcmp(session->command_paths[index], path) == 0)
            return index;
    return session->command_path_count;
}

/*
 * Replies with a counted payload of arbitrary bytes.
 *
 * The wire has always been counted -- payload_length is a uint32 and the
 * bytes follow it -- but every reply used to be measured with strlen(), which
 * quietly made the protocol text-only: a payload containing a NUL was cut
 * short at the NUL, and one containing high bytes survived only by luck. Exec
 * did not work that way. PutMsg() takes a Message with an mn_Length and never
 * looks inside it; what a port carries is opaque to the system moving it.
 * This is the broker doing the same.
 *
 * Names, paths and lists are still text and still go through send_response()
 * below, which is now a thin wrapper. The distinction is real rather than
 * cosmetic: an assign name is a string in AmigaDOS too, while a Rexx
 * argstring is counted bytes.
 */
static int send_response_bytes(int fd, int status, const void *payload,
                               size_t length)
{
    struct amiga_broker_response response;

    if (length > UINT32_MAX)
        return -1;
    response.magic = AMIGA_BROKER_MAGIC;
    response.status = status;
    response.payload_length = (uint32_t)length;
    response.flags = 0;
    if (write_all(fd, &response, sizeof(response)) != 0)
        return -1;
    return length ? write_all(fd, payload, length) : 0;
}

/*
 * A response that carries one descriptor.
 *
 * SCM_RIGHTS has to ride with real data, so the header is the data it
 * accompanies.  This is how a protected object reaches the command that asked
 * for it: the CRM opened it, the broker never looked inside it, and
 * what the command receives is a handle to that one object rather than any
 * ability to open another.
 */
static int send_response_with_fd(int fd, int status, int passed_fd)
{
    struct amiga_broker_response response;
    struct iovec io;
    struct msghdr message;
    union {
        char bytes[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control;
    struct cmsghdr *entry;
    ssize_t sent;

    memset(&response, 0, sizeof(response));
    response.magic = AMIGA_BROKER_MAGIC;
    response.status = status;
    response.payload_length = 0;
    response.flags = passed_fd >= 0 ? AMIGA_BROKER_RESPONSE_HAS_FD : 0;

    memset(&message, 0, sizeof(message));
    io.iov_base = &response;
    io.iov_len = sizeof(response);
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    if (passed_fd >= 0) {
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        entry = CMSG_FIRSTHDR(&message);
        entry->cmsg_level = SOL_SOCKET;
        entry->cmsg_type = SCM_RIGHTS;
        entry->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(entry), &passed_fd, sizeof(passed_fd));
    }
    do {
        sent = sendmsg(fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent < 0 ? -1 : 0;
}

/*
 * Ask the fmm's CRM to do one thing, on behalf of a command.
 *
 * Everything privileged in an ACE session funnels through here.  The command
 * has already tried the operation as itself and been refused, or the path is
 * one only the fmm's namespace contains; either way this is the single
 * point where a user process's request becomes a root process's action, and
 * it is worth it being one function in one program.
 *
 * The first live --root shell starts the FMM; the CRM is still created on its
 * first file-operation use.  Both remain available until the final --root
 * shell leaves the broker.
 */
static int ensure_access_worker(void)
{
    if (crm)
        return 0;
    if (!ace_mode_is_root()) {
        /* Not an authorised session.  The command's own refusal stands, which
           is what it would have got without --root. */
        errno = EACCES;
        return -1;
    }
    /* Authorised, but the privileged side could not be built.  Reported as
       EIO rather than EACCES: "the service is not there" is a fact about this
       machine, not about the object, and a caller that mistook it for one
       would tell the user a name they mistyped was protected. */
    if (!fmm && start_root_services() != 0) {
        errno = EIO;
        return -1;
    }
    crm = ace_fmm_start_crm(fmm);
    if (!crm) {
        errno = EIO;
        return -1;
    }
    return 0;
}

/*
 * Which domain a path is in, decided here because path translation lives
 * here.
 *
 * A path inside the device view goes as a view-relative one, so the worker
 * resolves it with RESOLVE_BENEATH and it cannot leave its volume by any
 * route.  Anything else goes as a host path.  The command never has to know
 * the difference and is never asked to state it.
 */
static const char *privop_domain_path(const char *path, uint32_t *flags)
{
    const char *view_root = ace_dos_devices_view_root();
    size_t length = view_root ? strlen(view_root) : 0;

    if (length && strncmp(path, view_root, length) == 0 &&
        path[length] == '/' && path[length + 1]) {
        *flags &= ~(uint32_t)ACE_PRIVILEGE_FLAG_HOST_PATH;
        return path + length + 1;
    }
    *flags |= ACE_PRIVILEGE_FLAG_HOST_PATH;
    /* An absolute path becomes one relative to the worker's root, which is
       the real root.  The leading slash is dropped rather than kept, because
       the worker refuses an absolute path outright: which root a path is
       relative to is stated by the domain flag, not by punctuation. */
    return path[0] == '/' ? path + 1 : path;
}

static int perform_privileged_operation(const struct amiga_broker_request *request,
                                        const char *path, const char *value,
                                        int *received_fd)
{
    uint32_t flags = request->flags;
    const char *first;
    const char *second = NULL;
    int status;

    *received_fd = -1;
    if (!path || !*path || path[0] != '/')
        return EINVAL;
    if (ensure_access_worker() != 0)
        return errno ? errno : EIO;

    first = privop_domain_path(path, &flags);
    if (request->privop == ACE_PRIVILEGE_ACCESS_SYMLINK) {
        /* The target travels exactly as the caller wrote it.  It is the
           link's contents, not a name this broker is resolving, and a
           translation applied here would be a translation applied again by
           whatever reads the link later. */
        if (!value || !*value)
            return EINVAL;
        second = value;
    } else if (request->privop == ACE_PRIVILEGE_ACCESS_RENAME ||
               request->privop == ACE_PRIVILEGE_ACCESS_LINK) {
        uint32_t second_flags = flags;

        if (!value || !*value || value[0] != '/')
            return EINVAL;
        second = privop_domain_path(value, &second_flags);
        /* Both halves must be in the same domain.  A rename or a hard link
           that crossed from a volume into the host tree is not an operation
           this layer can express, and pretending otherwise would resolve the
           two ends under different rules. */
        if ((second_flags & ACE_PRIVILEGE_FLAG_HOST_PATH) !=
            (flags & ACE_PRIVILEGE_FLAG_HOST_PATH))
            return EXDEV;
    }

    status = ace_crm(crm, request->privop, first, second,
                                 flags, request->privop_mode,
                                 request->privop_time, received_fd);
    if (status < 0) {
        /* The channel failed, not the operation.  Drop the worker so the next
           request builds a fresh one rather than talking to a corpse. */
        ace_privilege_connection_close(crm);
        crm = NULL;
        return EIO;
    }
    if (status == ACE_PRIVILEGE_OK)
        return 0;
    if (status == ACE_PRIVILEGE_ESCAPED)
        return ELOOP;
    if (status == ACE_PRIVILEGE_UNAUTHORISED || status == ACE_PRIVILEGE_REFUSED)
        return EACCES;
    if (status == ACE_PRIVILEGE_UNSUPPORTED)
        return ENOSYS;
    /* A malformed exchange is not a fact about the file.  Without this the
       status falls through to whatever errno happened to be left over, and a
       protocol mistake arrives at the user as a missing object -- which sends
       whoever debugs it looking at the filesystem. */
    if (status == ACE_PRIVILEGE_PROTOCOL_ERROR)
        return EPROTO;
    return errno ? errno : EIO;
}

/* The broker owns the current directory, so setting it has to be able to
 * validate a directory the broker itself cannot see.  A device-view path is
 * the usual case -- it exists only in the FMM's private mount namespace, so
 * the broker's own stat says ENOENT about an object that is plainly there --
 * but the broker does not work out which case it is in, because it cannot.
 * It stats as itself, and if that fails it asks the CRM, exactly as every
 * other operation in ACE now does.  The path takes no part in the decision. */
static int stat_for_current_directory(const char *path, struct stat *information)
{
    struct amiga_broker_request request;
    int failure;
    int descriptor = -1;
    int status;

    if (stat(path, information) == 0)
        return 0;
    failure = errno;

    memset(&request, 0, sizeof(request));
    request.privop = ACE_PRIVILEGE_ACCESS_STAT;
    status = perform_privileged_operation(&request, path, NULL, &descriptor);
    if (status != 0) {
        /* The privileged attempt is a second chance, not a second opinion:
           when it fails too, what the broker's own stat said about the object
           is the answer, rather than a status describing the channel. */
        errno = failure;
        return -1;
    }
    if (descriptor < 0 || fstat(descriptor, information) != 0) {
        int failure = descriptor < 0 ? EPROTO : errno;

        if (descriptor >= 0)
            close(descriptor);
        errno = failure;
        return -1;
    }
    close(descriptor);
    return 0;
}

/*
 * The current directory, after making sure it is still there.
 *
 * Linux removes a directory that processes are standing in and says nothing,
 * so an ACE session can be left holding a name that no longer refers to
 * anything: the prompt goes on printing it, and the next relative name
 * resolved against it fails -- or lands somewhere else entirely, which is
 * worse, because nothing about it looks wrong.
 *
 * AmigaOS never had to answer this. The current directory is held by a Lock,
 * a Lock is a real reference, and Delete on a locked object answers
 * ERROR_OBJECT_IN_USE. ACE has no such reference to offer, so it settles the
 * question afterwards instead: walk up to the nearest ancestor that does
 * exist, which is what a person would do by hand and is the one answer that
 * cannot be surprising. The walk stops on its own at the volume root, because
 * a mounted volume's root exists; it only goes past there if the volume
 * itself has gone.
 *
 * Only absence moves the session. A directory that cannot be stat'ed for some
 * other reason -- a parent the user may not traverse, above all -- is still
 * there, and is left exactly where it is. Treating a refusal as a deletion
 * would quietly move an authorised session out of the drawer it was working
 * in, which is the kind of help nobody asked for.
 */
static void settle_current_directory(struct broker_session *session)
{
    struct stat information;
    char *slash;

    for (;;) {
        if (stat_for_current_directory(session->cwd, &information) == 0) {
            if (S_ISDIR(information.st_mode))
                return;
            /* Something else holds the name now. The drawer is still gone. */
        } else if (errno != ENOENT && errno != ENOTDIR) {
            return;
        }
        if (strcmp(session->cwd, "/") == 0)
            return;
        slash = strrchr(session->cwd, '/');
        if (!slash)
            return;
        if (slash == session->cwd)
            session->cwd[1] = '\0';
        else
            *slash = '\0';
    }
}

static int send_response(int fd, int status, const char *payload)
{
    return send_response_bytes(fd, status, payload,
                               payload ? strlen(payload) : 0);
}

/*
 * One connection. anchor is the index of the session this connection has
 * claimed with ATTACH, or -1 for a connection that is only using a session
 * it does not own.
 */
struct broker_connection {
    int fd;
    int anchor;
    uint64_t task_id;
    uint64_t port_channel_id;
};

static struct broker_task *find_task_id(uint64_t id)
{
    for (size_t index = 0; index < MAX_TASKS; index++)
        if (tasks[index].id == id)
            return &tasks[index];
    return NULL;
}

static void drop_task_connection(struct broker_connection *connection)
{
    struct broker_task *task;

    if (!connection->task_id)
        return;
    task = find_task_id(connection->task_id);
    if (task)
        memset(task, 0, sizeof(*task));
    connection->task_id = 0;
}

/*
 * A port outlives neither its process nor its connection. One connection may
 * hold several ports -- an application with a command port and a reply port
 * is the ordinary case -- so this drops every port it registered rather than
 * one id kept on the connection.
 */
/* Defined below, beside the message table they work on. Declared here
   because releasing a port or a channel is what makes messages unanswerable,
   and those two live above it. */
static void abandon_messages_for_port(uint64_t port_id, const char *reason);
static void abandon_messages_for_owner(pid_t owner_pid, const char *reason);

static void drop_connection_ports(struct broker_connection *connection)
{
    for (size_t index = 0; index < MAX_PORTS; index++) {
        if (ports[index].id && ports[index].fd == connection->fd) {
            abandon_messages_for_port(ports[index].id,
                                      "the port's owner disconnected");
            memset(&ports[index], 0, sizeof(ports[index]));
        }
    }
}

static struct broker_port_channel *find_port_channel_pid(pid_t pid)
{
    for (size_t index = 0; index < MAX_PORT_CHANNELS; index++)
        if (port_channels[index].id && port_channels[index].pid == pid)
            return &port_channels[index];
    return NULL;
}

static void drop_connection_port_channels(struct broker_connection *connection)
{
    for (size_t index = 0; index < MAX_PORT_CHANNELS; index++) {
        if (port_channels[index].id &&
            port_channels[index].fd == connection->fd) {
            /* A backstop rather than the usual path: a process that dies
               loses its ports first (drop_connection_ports, above), and that
               releases its senders by port. This catches a channel that goes
               without the ports going with it. */
            abandon_messages_for_owner(port_channels[index].pid,
                                       "the receiving process is gone");
            memset(&port_channels[index], 0, sizeof(port_channels[index]));
        }
    }
}

static int push_port_record(int channel_fd, uint32_t operation,
                            uint64_t message_id, uint64_t port_id,
                            const void *payload, size_t length,
                            uint32_t flags, const int *fds, size_t fd_count);

static void remember_broadcast_event(const void *payload, size_t length)
{
    struct broker_broadcast_event *event;

    if (length > AMIGA_BROKER_MAX_PAYLOAD || (length && !payload))
        return;
    if (broadcast_event_count == MAX_BROADCAST_EVENTS) {
        free(broadcast_events[0].payload);
        memmove(broadcast_events, broadcast_events + 1,
                (MAX_BROADCAST_EVENTS - 1) * sizeof(broadcast_events[0]));
        broadcast_event_count--;
    }
    event = &broadcast_events[broadcast_event_count++];
    event->payload = NULL;
    event->length = length;
    if (length) {
        event->payload = malloc(length);
        if (!event->payload) {
            event->length = 0;
            return;
        }
        memcpy(event->payload, payload, length);
    }
}

static int replay_broadcast_events(int channel_fd)
{
    for (size_t index = 0; index < broadcast_event_count; index++) {
        struct broker_broadcast_event *event = &broadcast_events[index];

        if (push_port_record(channel_fd, AMIGA_BROKER_PORT_BROADCAST, 0, 0,
                             event->payload, event->length, 0, NULL, 0) != 0)
            return -1;
    }
    return 0;
}

/*
 * Claims this connection as the delivery channel for one process.
 *
 * A process gets one. A second attach from the same pid replaces the first
 * rather than being refused: the ordinary way to reach this is a process that
 * exec()ed, where the old connection is closed by the kernel but this may be
 * served before that close is noticed. Refusing would leave the new process
 * unable to receive anything, which is worse than dropping a channel nothing
 * can reach any more.
 */
static int attach_port_channel(struct broker_connection *connection,
                               const char *pid_text, char *result,
                               size_t result_size)
{
    char *end;
    long pid;
    struct broker_port_channel *existing;

    if (connection->port_channel_id || !pid_text || !pid_text[0]) {
        errno = EINVAL;
        return -1;
    }
    pid = strtol(pid_text, &end, 10);
    if (*end || pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    existing = find_port_channel_pid((pid_t)pid);
    if (existing) {
        /* Anything delivered down the channel being replaced can no longer be
           read, let alone answered: after an exec() the process holding it is
           running a different program entirely. Releasing the senders here
           rather than waiting for the old connection's close to be noticed,
           because by then this record is gone and the close would match
           nothing. */
        abandon_messages_for_owner((pid_t)pid,
                                   "the receiving process was replaced");
        memset(existing, 0, sizeof(*existing));
    }
    for (size_t index = 0; index < MAX_PORT_CHANNELS; index++) {
        struct broker_port_channel *channel = &port_channels[index];

        if (channel->id)
            continue;
        channel->id = next_port_channel_id++;
        if (!next_port_channel_id)
            next_port_channel_id = 1;
        channel->fd = connection->fd;
        channel->pid = (pid_t)pid;
        connection->port_channel_id = channel->id;
        snprintf(result, result_size, "%llu",
                 (unsigned long long)channel->id);
        return 0;
    }
    errno = ENOSPC;
    return -1;
}

/*
 * Who is on the other end of this connection.
 *
 * Asked of the kernel rather than of the caller. PORT_ADD and TASK_ATTACH
 * take a pid as text because they name a process that is registering itself,
 * and a wrong answer there costs that caller its own registration. A message
 * is different: the pid decides where a reply is routed, so a caller that
 * could name any pid could have another process's replies delivered to it.
 */
static pid_t connection_peer_pid(int fd)
{
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
        peer_size != sizeof(peer))
        return -1;
    return peer.pid;
}

/*
 * Pushes one record down a delivery channel: header, then the bytes.
 *
 * The payload is opaque here and stays that way. The broker is moving a
 * message, not reading one -- Exec's PutMsg() does not look inside a Message
 * either.
 */
static int push_port_record(int channel_fd, uint32_t operation,
                            uint64_t message_id, uint64_t port_id,
                            const void *payload, size_t length,
                            uint32_t flags, const int *fds, size_t fd_count)
{
    struct amiga_broker_port_record record;
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int) * AMIGA_BROKER_PORT_MAX_FDS)];
    } control;
    struct msghdr message;
    struct iovec vector;
    ssize_t sent;

    if (length > AMIGA_BROKER_MAX_PAYLOAD)
        return -1;
    record.magic = AMIGA_BROKER_MAGIC;
    record.operation = operation;
    record.message_id = message_id;
    record.port_id = port_id;
    record.payload_length = (uint32_t)length;
    record.flags = flags;

    if (!fd_count) {
        if (write_all(channel_fd, &record, sizeof(record)) != 0)
            return -1;
        return length ? write_all(channel_fd, payload, length) : 0;
    }
    if (fd_count > AMIGA_BROKER_PORT_MAX_FDS)
        return -1;
    memset(&control, 0, sizeof(control));
    memset(&message, 0, sizeof(message));
    vector.iov_base = &record;
    vector.iov_len = sizeof(record);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
    {
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);

        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
        memcpy(CMSG_DATA(header), fds, sizeof(int) * fd_count);
    }
    do {
        sent = sendmsg(channel_fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent <= 0)
        return -1;
    if ((size_t)sent < sizeof(record) &&
        write_all(channel_fd, (const char *)&record + sent,
                  sizeof(record) - sent) != 0)
        return -1;
    return length ? write_all(channel_fd, payload, length) : 0;
}

static struct broker_port_message *find_port_message(uint64_t id)
{
    for (size_t index = 0; index < MAX_PORT_MESSAGES; index++)
        if (port_messages[index].id == id)
            return &port_messages[index];
    return NULL;
}

/*
 * A sender that goes away is no longer waiting for anything, so its
 * conversations are dropped with its connection.
 */
static void drop_connection_port_messages(struct broker_connection *connection)
{
    for (size_t index = 0; index < MAX_PORT_MESSAGES; index++) {
        if (port_messages[index].id &&
            port_messages[index].sender_channel_fd == connection->fd)
            memset(&port_messages[index], 0, sizeof(port_messages[index]));
    }
}

/*
 * Tells a sender that its message will never be answered.
 *
 * The sender is in WaitPort(), which has no timeout and cannot be broken out
 * of, so a message whose receiver is gone has to be ended explicitly by the
 * only party that can still see both ends. Silently forgetting it -- which is
 * what happened before this existed -- leaves the sender waiting for the life
 * of the process.
 *
 * Reported on stderr because it cannot happen on AmigaOS and should not pass
 * unremarked here: it means a process died holding somebody else's message.
 */
static void abandon_port_message(struct broker_port_message *message,
                                 const char *reason)
{
    fprintf(stderr,
            "ace-broker: message %llu to port %llu abandoned: %s "
            "(owner pid %ld, sender pid %ld)\n",
            (unsigned long long)message->id,
            (unsigned long long)message->port_id, reason,
            (long)message->owner_pid, (long)message->sender_pid);
    (void)push_port_record(message->sender_channel_fd,
                           AMIGA_BROKER_PORT_ABANDONED, message->id,
                           message->port_id, NULL, 0, 0, NULL, 0);
    memset(message, 0, sizeof(*message));
}

static void abandon_messages_for_port(uint64_t port_id, const char *reason)
{
    for (size_t index = 0; index < MAX_PORT_MESSAGES; index++)
        if (port_messages[index].id &&
            port_messages[index].port_id == port_id)
            abandon_port_message(&port_messages[index], reason);
}

static void abandon_messages_for_owner(pid_t owner_pid, const char *reason)
{
    for (size_t index = 0; index < MAX_PORT_MESSAGES; index++)
        if (port_messages[index].id &&
            port_messages[index].owner_pid == owner_pid)
            abandon_port_message(&port_messages[index], reason);
}

static struct broker_port *find_port_name(const char *name)
{
    for (size_t index = 0; index < MAX_PORTS; index++) {
        if (ports[index].id && strcmp(ports[index].name, name) == 0)
            return &ports[index];
    }
    return NULL;
}

static int add_port(struct broker_connection *connection,
                    struct broker_session *session, const char *name,
                    const char *pid_text, char *result, size_t result_size)
{
    char *end;
    long pid = strtol(pid_text, &end, 10);

    if (!name || !name[0] || strlen(name) >= MAX_NAME || !pid_text ||
        !pid_text[0] || *end || pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    /* A name is claimed by one port at a time, as on the Amiga: the second
       application to offer REXX does not quietly displace the first, and
       finding out which one a message would reach is exactly what the caller
       is asking. */
    if (find_port_name(name)) {
        errno = EEXIST;
        return -1;
    }
    for (size_t index = 0; index < MAX_PORTS; index++) {
        struct broker_port *port = &ports[index];

        if (port->id)
            continue;
        port->id = next_port_id++;
        if (!next_port_id)
            next_port_id = 1;
        port->fd = connection->fd;
        port->session = (int)(session - sessions);
        port->pid = (pid_t)pid;
        strcpy(port->name, name);
        snprintf(result, result_size, "%llu", (unsigned long long)port->id);
        return 0;
    }
    errno = ENOSPC;
    return -1;
}

static int attach_task(struct broker_connection *connection,
                       struct broker_session *session, const char *name,
                       const char *pid_text, char *result, size_t result_size)
{
    char *end;
    long pid = strtol(pid_text, &end, 10);

    if (connection->task_id || !name[0] || strlen(name) >= MAX_NAME ||
        !pid_text[0] || *end || pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    for (size_t index = 0; index < MAX_TASKS; index++) {
        struct broker_task *task = &tasks[index];

        if (task->id)
            continue;
        task->id = next_task_id++;
        if (!next_task_id)
            next_task_id = 1;
        task->fd = connection->fd;
        task->session = (int)(session - sessions);
        task->pid = (pid_t)pid;
        strcpy(task->name, name);
        connection->task_id = task->id;
        if (session->foreground_pid == task->pid)
            session->foreground_task = task->id;
        snprintf(result, result_size, "%llu",
                 (unsigned long long)task->id);
        return 0;
    }
    errno = ENOSPC;
    return -1;
}

/*
 * Serves one request on one connection.
 *
 * Returns 0 to keep the connection, -1 to drop it. A protocol error drops
 * it: the stream carries length-prefixed messages back to back, so once a
 * header has been misread there is no way to find where the next one starts.
 */
static int handle_client(struct broker_connection *connection)
{
    int fd = connection->fd;
    struct amiga_broker_request request;
    char *session_id = NULL;
    char *path = NULL;
    char *value = NULL;
    struct broker_session *session;
    char result[MAX_LIST_RESULT];
    int status = 0;
    int outcome = -1;
    uint32_t value_limit;
    /* Descriptors that arrived with this request, if any. They belong to this
       process from the moment recvmsg() hands them over, so every path out of
       here has to either pass them on or close them -- hence the single exit
       below rather than a bare return. */
    int passed_fds[AMIGA_BROKER_PORT_MAX_FDS];
    size_t passed_fd_count = 0;

    int privop_fd = -1;
    int privop_used = 0;

    if (read_message_with_fds(fd, &request, sizeof(request), passed_fds,
                              AMIGA_BROKER_PORT_MAX_FDS,
                              &passed_fd_count) != 1)
        goto drop;
    if (request.magic != AMIGA_BROKER_MAGIC) {
        char message[160];

        /* The magic carries the peer's protocol version, so say which one
           turned up rather than only that the request was rejected.  The
           reply's own magic lets the client name this broker in turn. */
        snprintf(message, sizeof(message),
                 "broker protocol 0x%08x, client sent 0x%08x",
                 (unsigned)AMIGA_BROKER_PROTOCOL_VERSION,
                 (unsigned)amiga_broker_version_from_magic(request.magic));
        send_response(fd, EPROTO, message);
        goto drop;
    }
    {
        uint32_t expected_mode =
            (ace_mode_is_root() ? AMIGA_BROKER_MODE_ROOT : 0) |
            (ace_mode_is_device_view() ?
                 AMIGA_BROKER_MODE_DEVICEVIEW : 0);

        if (request.mode != expected_mode ||
            request.owner_uid != (uint32_t)ace_mode_owner_uid()) {
            send_response(fd, EXDEV, "broker mode mismatch");
            goto drop;
        }
    }
    /*
     * A value is normally a path, a variable's contents or a list, so PATH_MAX
     * is the right bound and several handlers copy into fixed buffers sized
     * on that assumption. A message is not: it is up to a full payload of
     * opaque bytes. Raising the limit for everything would let a 16K value
     * reach a handler expecting at most 4K, so the two message operations
     * are named explicitly instead.
     */
    value_limit = (request.operation == AMIGA_BROKER_PORT_PUT ||
                   request.operation == AMIGA_BROKER_PORT_REPLY ||
                   request.operation == AMIGA_BROKER_PORT_BROADCAST)
                  ? AMIGA_BROKER_MAX_PAYLOAD : (uint32_t)PATH_MAX;
    if (request.session_length > 4096 || request.path_length > PATH_MAX ||
        request.value_length > value_limit) {
        send_response(fd, EPROTO, "invalid request");
        goto drop;
    }

    session_id = calloc(request.session_length + 1, 1);
    path = calloc(request.path_length + 1, 1);
    value = calloc(request.value_length + 1, 1);
    if (!session_id || !path || !value ||
        read_all(fd, session_id, request.session_length) != 0 ||
        read_all(fd, path, request.path_length) != 0 ||
        read_all(fd, value, request.value_length) != 0) {
        send_response(fd, ENOMEM, "request allocation failed");
        goto done;
    }

    session = get_session(session_id);
    if (!session) {
        send_response(fd, ENOSPC, "too many sessions");
        outcome = 0;
        goto done;
    }
    outcome = 0;
    result[0] = '\0';

    switch (request.operation) {
    case AMIGA_BROKER_RESOLVE:
        if (resolve_path(session, path, result, sizeof(result), false) != 0)
            status = errno;
        else if (request.flags & AMIGA_BROKER_FLAG_FOR_OPEN)
            status = refuse_unopenable_object(result);
        break;

    case AMIGA_BROKER_RESOLVE_BENEATH:
        if (normalize_mapped_path_beneath(path, value, result,
                                          sizeof(result)) != 0)
            status = errno;
        break;

    case AMIGA_BROKER_NAMEFROMHOST:
        if (name_from_host_with_mappings(path, result, sizeof(result)) != 0)
            status = errno;
        break;

    case AMIGA_BROKER_GETCWD:
        /* Asked once per prompt, among other times, which is where a session
           standing in a deleted drawer needs to find out about it. */
        settle_current_directory(session);
        strcpy(result, session->cwd);
        break;

    case AMIGA_BROKER_SETCWD: {
        struct stat information;
        if (resolve_path(session, path, result, sizeof(result),
                         (request.flags & AMIGA_BROKER_PATH_HOST) != 0) != 0 ||
            stat_for_current_directory(result, &information) != 0 ||
            !S_ISDIR(information.st_mode)) {
            status = errno ? errno : ENOTDIR;
        } else {
            strcpy(session->cwd, result);
        }
        break;
    }

    case AMIGA_BROKER_ASSIGN: {
        char assign_name[MAX_NAME];
        size_t assign_length = strlen(path);
        struct assign_entry *assign;

        /* Assigns are DOS objects, not aliases for implementation paths.
         * Store a canonical, volume-qualified DOS target and resolve it on
         * use so neither mountpoint view nor device view leaks into the
         * session's logical state. */
        if (request.flags & AMIGA_BROKER_PATH_HOST) {
            status = EINVAL;
            break;
        }

        if (assign_length && path[assign_length - 1] == ':')
            assign_length--;

        if (assign_length == 0 || assign_length >= sizeof(assign_name)) {
            status = EINVAL;
            break;
        }
        memcpy(assign_name, path, assign_length);
        assign_name[assign_length] = '\0';
        assign = find_assign(session, assign_name);

        if (request.flags & AMIGA_BROKER_ASSIGN_REMOVE) {
            if (assign)
                memset(assign, 0, sizeof(*assign));
            break;
        }
        if (request.flags & AMIGA_BROKER_ASSIGN_REMOVE_ITEM) {
            char target[PATH_MAX];
            char host[PATH_MAX];

            if (!assign || canonical_assign_target(
                               session, value, target, sizeof(target), host,
                               sizeof(host)) != 0) {
                status = errno ? errno : ENOENT;
                break;
            }
            for (size_t index = 0; index < assign->target_count; index++) {
                if (strcasecmp(assign->targets[index], target) == 0) {
                    memmove(&assign->targets[index],
                            &assign->targets[index + 1],
                            (assign->target_count - index - 1) *
                            sizeof(assign->targets[0]));
                    assign->target_count--;
                    break;
                }
            }
            if (assign->target_count == 0)
                memset(assign, 0, sizeof(*assign));
            break;
        }

        if (request.flags & (AMIGA_BROKER_ASSIGN_ADD |
                             AMIGA_BROKER_ASSIGN_PREPEND)) {
            char target[PATH_MAX];
            char host[PATH_MAX];

            if (!assign || assign->target_count >= MAX_ASSIGN_TARGETS ||
                canonical_assign_target(session, value, target,
                                        sizeof(target), host,
                                        sizeof(host)) != 0) {
                status = errno ? errno : ENOSPC;
                break;
            }
            if (request.flags & AMIGA_BROKER_ASSIGN_PREPEND) {
                memmove(&assign->targets[1], &assign->targets[0],
                        assign->target_count * sizeof(assign->targets[0]));
                strcpy(assign->targets[0], target);
            } else {
                strcpy(assign->targets[assign->target_count], target);
            }
            assign->target_count++;
            strcpy(assign->root, assign->targets[0]);
            assign->type = ASSIGN_DIRECTORY;
            assign->host_backed = false;
            break;
        }

        assign = allocate_assign(session, assign_name);
        if (!assign) {
            status = ENOSPC;
            break;
        }
        if (request.flags & (AMIGA_BROKER_ASSIGN_PATH |
                             AMIGA_BROKER_ASSIGN_DEFER)) {
            if (strlen(value) >= sizeof(assign->root)) {
                status = ENAMETOOLONG;
                break;
            }
            memset(assign->targets, 0, sizeof(assign->targets));
            assign->target_count = 0;
            strcpy(assign->root, value);
            assign->type = (request.flags & AMIGA_BROKER_ASSIGN_PATH) ?
                           ASSIGN_NONBINDING : ASSIGN_LATE;
            assign->host_backed = false;
            break;
        }
        {
            char logical[PATH_MAX];

            if (canonical_assign_target(session, value, logical,
                                        sizeof(logical), result,
                                        sizeof(result)) != 0) {
                status = errno ? errno : ENOENT;
                break;
            }
            memset(assign->targets, 0, sizeof(assign->targets));
            assign->target_count = 1;
            strcpy(assign->targets[0], logical);
            strcpy(assign->root, logical);
            assign->type = ASSIGN_DIRECTORY;
            assign->host_backed = false;
        }
        break;
    }

    case AMIGA_BROKER_LISTASSIGNS: {
        size_t used = 0;

        result[0] = '\0';
        for (size_t index = 0; index < MAX_ASSIGNS; index++) {
            struct assign_entry *assign = &session->assigns[index];
            char serialized_root[PATH_MAX];
            int written;

            if (!assign->name[0])
                continue;
            if (assign->host_backed) {
                if (name_from_host_with_mappings(assign->root,
                                                 serialized_root,
                                                 sizeof(serialized_root)) != 0)
                    continue;
            } else {
                snprintf(serialized_root, sizeof(serialized_root), "%s",
                         assign->root);
            }
            written = snprintf(result + used, sizeof(result) - used,
                               "%s\t%d\t%s\n", assign->name, assign->type,
                               serialized_root);
            if (written < 0 || (size_t)written >= sizeof(result) - used) {
                status = ENOSPC;
                break;
            }
            used += (size_t)written;
            for (size_t target = 1; target < assign->target_count; target++) {
                written = snprintf(result + used, sizeof(result) - used,
                                   "%s\t%d\t%s\n", assign->name,
                                   ASSIGN_DIRECTORY,
                                   assign->targets[target]);
                if (written < 0 ||
                    (size_t)written >= sizeof(result) - used) {
                    status = ENOSPC;
                    break;
                }
                used += (size_t)written;
            }
        }
        break;
    }

    case AMIGA_BROKER_LISTDOS:
        if (ace_dos_devices_list(result, sizeof(result)) != 0)
            status = errno;
        break;

    case AMIGA_BROKER_RELABEL:
        if (ace_dos_devices_relabel(path, value) != 0)
            status = errno;
        break;

    case AMIGA_BROKER_LISTPATH: {
        size_t used = 0;

        result[0] = '\0';
        for (size_t index = 0; index < session->command_path_count; index++) {
            int written = snprintf(result + used, sizeof(result) - used,
                                   "%s\n", session->command_paths[index]);

            if (written < 0 || (size_t)written >= sizeof(result) - used) {
                status = ENOSPC;
                break;
            }
            used += (size_t)written;
        }
        break;
    }

    case AMIGA_BROKER_PATH: {
        char canonical[PATH_MAX];

        if (request.flags & AMIGA_BROKER_PATH_RESET)
            session->command_path_count = 0;
        if (!path[0])
            break;
        if (canonical_command_path(session, path, canonical,
                                   sizeof(canonical)) != 0) {
            status = errno;
            break;
        }
        if (request.flags & AMIGA_BROKER_PATH_REMOVE) {
            size_t index = find_command_path(session, canonical);

            if (index == session->command_path_count) {
                status = ENOENT;
                break;
            }
            memmove(&session->command_paths[index],
                    &session->command_paths[index + 1],
                    (session->command_path_count - index - 1) *
                    sizeof(session->command_paths[0]));
            session->command_path_count--;
        } else if (find_command_path(session, canonical) ==
                   session->command_path_count) {
            if (session->command_path_count >= MAX_COMMAND_PATHS) {
                status = ENOSPC;
                break;
            }
            if (request.flags & AMIGA_BROKER_PATH_PREPEND) {
                memmove(&session->command_paths[1],
                        &session->command_paths[0],
                        session->command_path_count *
                        sizeof(session->command_paths[0]));
                strcpy(session->command_paths[0], canonical);
            } else {
                strcpy(session->command_paths[session->command_path_count],
                       canonical);
            }
            session->command_path_count++;
        }
        break;
    }

    /*
     * The shell claiming its session. From here the session's lifetime is
     * this connection's: it cannot be reclaimed while the connection is
     * open, and it is freed when the connection closes.
     *
     * A second ATTACH on the same connection is refused rather than counted,
     * so a connection can never hold more than the one reference that
     * closing it will give back.
     */
    case AMIGA_BROKER_ATTACH:
        if (connection->anchor >= 0) {
            status = EALREADY;
        } else if (ace_mode_is_root() && root_shells == 0 &&
                   start_root_services() != 0) {
            status = errno ? errno : EACCES;
        } else {
            session->anchors++;
            connection->anchor = (int)(session - sessions);
            if (ace_mode_is_root())
                root_shells++;
        }
        break;

    case AMIGA_BROKER_TASK_ATTACH:
        if (attach_task(connection, session, path, value, result, sizeof(result)) != 0)
            status = errno;
        break;

    case AMIGA_BROKER_PORT_ATTACH:
        if (attach_port_channel(connection, path, result,
                                sizeof(result)) != 0)
            status = errno;
        break;

    case AMIGA_BROKER_PORT_ADD:
        if (add_port(connection, session, path, value, result,
                     sizeof(result)) != 0)
            status = errno;
        break;

    case AMIGA_BROKER_PORT_REM: {
        char *end;
        unsigned long long id = strtoull(path, &end, 10);
        int removed = 0;

        if (!path[0] || *end) {
            status = EINVAL;
            break;
        }
        for (size_t index = 0; index < MAX_PORTS; index++) {
            if (ports[index].id == id) {
                /* Same reasoning as a process dying: once the port is gone
                   nothing can answer what was sent to it, and a sender in
                   WaitPort() has no way out on its own. */
                abandon_messages_for_port(ports[index].id,
                                          "the port was deleted");
                memset(&ports[index], 0, sizeof(ports[index]));
                removed = 1;
                break;
            }
        }
        if (!removed)
            status = ESRCH;
        break;
    }

    case AMIGA_BROKER_PORT_PUT: {
        struct broker_port *port;
        struct broker_port_channel *owner_channel;
        struct broker_port_channel *sender_channel;
        struct broker_port_message *message = NULL;
        pid_t sender_pid;

        if (!path[0]) {
            status = EINVAL;
            break;
        }
        /* ESRCH and nothing else for "no such port": with nobody starting
           RexxMast automatically, a send to an unregistered REXX port is the
           ordinary first experience, and it has to be told apart from a real
           failure so the caller can say what to do about it. */
        port = find_port_name(path);
        if (!port) {
            status = ESRCH;
            break;
        }
        sender_pid = connection_peer_pid(fd);
        if (sender_pid <= 0) {
            status = EPERM;
            break;
        }
        owner_channel = find_port_channel_pid(port->pid);
        sender_channel = find_port_channel_pid(sender_pid);
        /* Both ends need a channel: the owner to be given the message, the
           sender to be given the reply. A sender without one would wait for
           something that could never be delivered. */
        if (!owner_channel || !sender_channel) {
            status = ENOTCONN;
            break;
        }
        for (size_t index = 0; index < MAX_PORT_MESSAGES; index++) {
            if (port_messages[index].id)
                continue;
            message = &port_messages[index];
            break;
        }
        if (!message) {
            status = ENOSPC;
            break;
        }
        message->id = next_port_message_id++;
        if (!next_port_message_id)
            next_port_message_id = 1;
        message->port_id = port->id;
        message->sender_channel_fd = sender_channel->fd;
        message->sender_pid = sender_pid;
        message->owner_pid = port->pid;
        /* The sender's own streams go with the message. The broker does not
           look at what they refer to; it hands them to the process the
           message is for, which is the whole point -- a script sent to
           another process writes on the console of the one that sent it. */
        if (push_port_record(owner_channel->fd, AMIGA_BROKER_PORT_PUT,
                             message->id, port->id, value,
                             request.value_length, request.flags, passed_fds,
                             passed_fd_count) != 0) {
            memset(message, 0, sizeof(*message));
            status = EIO;
            break;
        }
        snprintf(result, sizeof(result), "%llu",
                 (unsigned long long)message->id);
        break;
    }

    case AMIGA_BROKER_PORT_REPLY: {
        char *end;
        unsigned long long id = strtoull(path, &end, 10);
        struct broker_port_message *message;
        pid_t replier_pid;

        if (!path[0] || *end) {
            status = EINVAL;
            break;
        }
        message = find_port_message(id);
        if (!message) {
            status = ESRCH;
            break;
        }
        /* Only the process the message was delivered to may answer it.
           Without this, any process that guessed an id could answer somebody
           else's message, and the sender -- which waits for exactly one
           reply and then stops waiting -- would take it as the real one. */
        replier_pid = connection_peer_pid(fd);
        if (replier_pid <= 0 || replier_pid != message->owner_pid) {
            status = EPERM;
            break;
        }
        if (push_port_record(message->sender_channel_fd,
                             AMIGA_BROKER_PORT_REPLY, message->id,
                             message->port_id, value,
                             request.value_length, 0, NULL, 0) != 0)
            status = EIO;
        /* Cleared either way: a reply that could not be written is not going
           to be written by trying again, and leaving it would leak the slot. */
        memset(message, 0, sizeof(*message));
        break;
    }

    case AMIGA_BROKER_PORT_BROADCAST: {
        pid_t sender_pid = connection_peer_pid(fd);
        int failed = 0;

        if (sender_pid <= 0) {
            status = EPERM;
            break;
        }
        remember_broadcast_event(value, request.value_length);
        for (size_t index = 0; index < MAX_PORT_CHANNELS; index++) {
            struct broker_port_channel *channel = &port_channels[index];

            if (!channel->id || channel->pid == sender_pid)
                continue;
            if (push_port_record(channel->fd, AMIGA_BROKER_PORT_BROADCAST,
                                 0, 0, value, request.value_length, 0,
                                 NULL, 0) != 0)
                failed = 1;
        }
        if (failed)
            status = EIO;
        break;
    }

    case AMIGA_BROKER_PORT_FIND: {
        struct broker_port *port = find_port_name(path);

        if (port)
            snprintf(result, sizeof(result), "%llu",
                     (unsigned long long)port->id);
        else
            status = ESRCH;
        break;
    }

    case AMIGA_BROKER_TASK_FIND:
        for (size_t index = 0; index < MAX_TASKS; index++) {
            if (tasks[index].id && strcmp(tasks[index].name, path) == 0) {
                snprintf(result, sizeof(result), "%llu",
                         (unsigned long long)tasks[index].id);
                break;
            }
        }
        if (!result[0])
            status = ESRCH;
        break;

    case AMIGA_BROKER_TASK_SIGNAL: {
        char *end;
        unsigned long long id = strtoull(path, &end, 10);
        unsigned long mask;
        struct broker_task *task;
        struct amiga_broker_task_signal signal;

        if (!path[0] || *end || id == 0) {
            status = EINVAL;
            break;
        }
        mask = strtoul(value, &end, 10);
        if (!value[0] || *end || mask > UINT32_MAX) {
            status = EINVAL;
            break;
        }
        task = find_task_id((uint64_t)id);
        if (!task) {
            status = ESRCH;
            break;
        }
        signal.magic = AMIGA_BROKER_MAGIC;
        signal.operation = AMIGA_BROKER_TASK_SIGNAL;
        signal.task_id = task->id;
        signal.signals = (uint32_t)mask;
        if (write_all(task->fd, &signal, sizeof(signal)) != 0)
            status = ESRCH;
        break;
    }

    case AMIGA_BROKER_TASK_SET_FOREGROUND: {
        char *end;
        long pid = strtol(path, &end, 10);

        if (!path[0] || *end || pid < 0) {
            status = EINVAL;
            break;
        }
        session->foreground_pid = (pid_t)pid;
        session->foreground_task = 0;
        if (pid) {
            for (size_t index = 0; index < MAX_TASKS; index++)
                if (tasks[index].id && tasks[index].pid == pid) {
                    session->foreground_task = tasks[index].id;
                    break;
                }
        } else {
            session->pending_foreground_signals = 0;
        }
        break;
    }

    case AMIGA_BROKER_TASK_BREAK_FOREGROUND: {
        char *end;
        unsigned long mask = strtoul(path, &end, 10);
        struct broker_task *task = find_task_id(session->foreground_task);
        struct amiga_broker_task_signal signal;

        if (!path[0] || *end || mask > UINT32_MAX) {
            status = EINVAL;
            break;
        }
        if (!task) {
            /* A foreground child need not use ACE's native DOS layer (for
               example, an Exec-only command can call SetSignal directly).
               Its process has no broker task socket, so hand its break to
               the host signal bridge instead of leaving it queued forever.
               Registered tasks still take the broker route below. */
            if (!session->foreground_pid) {
                status = ESRCH;
                break;
            }
            if ((mask & (1U << 12)) &&
                kill(session->foreground_pid, SIGUSR1) != 0)
                status = errno;
            if (!status && (mask & (1U << 13)) &&
                kill(session->foreground_pid, SIGUSR2) != 0)
                status = errno;
            if (!status && (mask & (1U << 14)) &&
                kill(session->foreground_pid, SIGRTMIN) != 0)
                status = errno;
            if (!status && (mask & (1U << 15)) &&
                kill(session->foreground_pid, SIGRTMIN + 1) != 0)
                status = errno;
            if (status == ESRCH)
                session->foreground_pid = 0;
            break;
        }
        signal.magic = AMIGA_BROKER_MAGIC;
        signal.operation = AMIGA_BROKER_TASK_SIGNAL;
        signal.task_id = task->id;
        signal.signals = (uint32_t)mask;
        if (write_all(task->fd, &signal, sizeof(signal)) != 0)
            status = ESRCH;
        break;
    }

    case AMIGA_BROKER_STATUS: {
        char executable[PATH_MAX];
        ssize_t length;
        size_t used = 0;
        size_t live_sessions = 0;
        size_t live_tasks = 0;

        reap_say_voices();
        size_t live_ports = 0;
        size_t live_port_channels = 0;
        size_t live_port_messages = 0;
        int written;

        length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        if (length > 0)
            executable[length] = '\0';
        else
            strcpy(executable, "(unknown)");

        for (size_t i = 0; i < MAX_SESSIONS; i++)
            if (sessions[i].in_use)
                live_sessions++;
        for (size_t i = 0; i < MAX_TASKS; i++)
            if (tasks[i].id)
                live_tasks++;
        for (size_t i = 0; i < MAX_PORTS; i++)
            if (ports[i].id)
                live_ports++;
        for (size_t i = 0; i < MAX_PORT_CHANNELS; i++)
            if (port_channels[i].id)
                live_port_channels++;
        for (size_t i = 0; i < MAX_PORT_MESSAGES; i++)
            if (port_messages[i].id)
                live_port_messages++;

        written = snprintf(result, sizeof(result),
                           "protocol\t0x%08x\n"
                           "executable\t%s\n"
                           "pid\t%ld\n"
                           "uptime\t%lld\n"
                           "socket\t%s\n"
                           "sys\t%s\n"
                           "owner-uid\t%lu\n"
                           "privilege\t%s\n"
                           "view\t%s\n"
                           "sessions\t%zu\n"
                           "tasks\t%zu\n"
                           "ports\t%zu\n"
                           "port-channels\t%zu\n"
                           "port-messages\t%zu\n",
                           (unsigned)AMIGA_BROKER_PROTOCOL_VERSION,
                           executable, (long)getpid(),
                           (long long)(time(NULL) - broker_started),
                           socket_path, system_root,
                           (unsigned long)ace_mode_owner_uid(),
                           ace_mode_is_root() ? "root" : "user",
                           ace_mode_is_device_view() ? "device" : "mount",
                           live_sessions, live_tasks, live_ports,
                           live_port_channels, live_port_messages);
        if (written < 0 || (size_t)written >= sizeof(result)) {
            status = ENAMETOOLONG;
            break;
        }
        used = (size_t)written;

        /* Named, not just counted. "Which process is REXX right now" is the
           first question anyone debugging ARexx asks, and reading it out of
           ps is not an answer. */
        for (size_t i = 0; i < MAX_PORTS; i++) {
            if (!ports[i].id)
                continue;
            written = snprintf(result + used, sizeof(result) - used,
                               "port\t%s\t%ld\n", ports[i].name,
                               (long)ports[i].pid);
            if (written < 0 || (size_t)written >= sizeof(result) - used) {
                status = ENAMETOOLONG;
                break;
            }
            used += (size_t)written;
        }
        if (status)
            break;

        for (size_t i = 0; i < MAX_SAY_VOICES; i++) {
            if (!say_voices[i].pid)
                continue;
            written = snprintf(result + used, sizeof(result) - used,
                               "say\t%s\t%ld\n", say_voices[i].name,
                               (long)say_voices[i].pid);
            if (written < 0 || (size_t)written >= sizeof(result) - used) {
                status = ENAMETOOLONG;
                break;
            }
            used += (size_t)written;
        }
        if (status)
            break;

        for (size_t i = 0; i < MAX_SESSIONS; i++) {
            if (!sessions[i].in_use)
                continue;
            written = snprintf(result + used, sizeof(result) - used,
                               "session\t%s\t%u\t%s\n", sessions[i].id,
                               sessions[i].anchors, sessions[i].cwd);
            if (written < 0 || (size_t)written >= sizeof(result) - used)
                break; /* report what fits; this is a diagnostic, not a feed */
            used += (size_t)written;
        }
        (void)used;
        break;
    }

    case AMIGA_BROKER_TASK_LIST: {
        size_t used = 0;

        for (size_t index = 0; index < MAX_TASKS; index++) {
            int written;

            if (!tasks[index].id)
                continue;
            written = snprintf(result + used, sizeof(result) - used,
                               "%llu\t%ld\t%s\n",
                               (unsigned long long)tasks[index].id,
                               (long)tasks[index].pid, tasks[index].name);
            if (written < 0 || (size_t)written >= sizeof(result) - used) {
                status = EOVERFLOW;
                break;
            }
            used += (size_t)written;
        }
        break;
    }

    case AMIGA_BROKER_GETVAR: {
        struct variable_entry *variable = lookup_variable(session, path,
                                                           request.flags);
        if (variable)
            strcpy(result, variable->value);
        else
            status = ENOENT;
        break;
    }

    case AMIGA_BROKER_SETVAR: {
        struct variable_entry *variable;
        bool local = (request.flags & AMIGA_BROKER_VAR_LOCAL) != 0;
        bool global = (request.flags & AMIGA_BROKER_VAR_GLOBAL) != 0;

        if (request.flags & AMIGA_BROKER_VAR_SAVE)
            status = ENOTSUP;
        else if (local && global)
            status = EINVAL;
        else {
            if (!local && !global)
                local = true;
            if (strlen(path) >= MAX_NAME || strlen(value) >= MAX_VALUE)
                status = EOVERFLOW;
        }
        if (!status) {
            variable = allocate_variable(global ? global_vars : session->local_vars,
                                         path,
                                         (request.flags & AMIGA_BROKER_VAR_ALIAS) ?
                                         AMIGA_BROKER_VAR_ALIAS :
                                         AMIGA_BROKER_VAR_VARIABLE);
            if (!variable)
                status = ENOSPC;
            else
                strcpy(variable->value, value);
        }
        break;
    }

    case AMIGA_BROKER_DELVAR: {
        struct variable_entry *variable = lookup_variable(session, path,
                                                           request.flags);
        if (request.flags & AMIGA_BROKER_VAR_SAVE)
            status = ENOTSUP;
        else if (!variable)
            status = ENOENT;
        else
            memset(variable, 0, sizeof(*variable));
        break;
    }

    case AMIGA_BROKER_LISTVARS: {
        size_t used = 0;
        bool local = (request.flags & AMIGA_BROKER_VAR_LOCAL) != 0;
        bool global = (request.flags & AMIGA_BROKER_VAR_GLOBAL) != 0;

        if (!local && !global)
            local = true;
        if (local && append_variable_list(result, sizeof(result), &used,
                                          session->local_vars,
                                          request.flags) != 0)
            status = EOVERFLOW;
        if (!status && global &&
            append_variable_list(result, sizeof(result), &used, global_vars,
                                 request.flags) != 0)
            status = EOVERFLOW;
        break;
    }

    case AMIGA_BROKER_GETRESULT:
        snprintf(result, sizeof(result), "%ld,%ld",
                 (long)session->return_code, (long)session->result2);
        break;

    case AMIGA_BROKER_GETCLI:
        snprintf(result, sizeof(result), "%ld\n%ld\n%ld\n%s",
                 (long)session->return_code, (long)session->result2,
                 (long)session->fail_level, session->prompt);
        break;

    case AMIGA_BROKER_SETFAILLEVEL: {
        char *end;
        long fail_level = strtol(path, &end, 10);
        if (!path[0] || *end || fail_level < INT32_MIN || fail_level > INT32_MAX)
            status = EINVAL;
        else
            session->fail_level = (int32_t)fail_level;
        break;
    }

    case AMIGA_BROKER_SETPROMPT:
        if (strlen(path) >= sizeof(session->prompt))
            status = EOVERFLOW;
        else
            strcpy(session->prompt, path);
        break;

    case AMIGA_BROKER_CLONESESSION: {
        struct broker_session *child;
        if (!path[0] || strlen(path) >= sizeof(session->id))
            status = EINVAL;
        else if (strcmp(path, session->id) == 0)
            status = EINVAL;
        else {
            if (find_session(path)) {
                status = EEXIST;
                break;
            }
            child = get_session(path);
            if (!child)
                status = ENOSPC;
            else {
                strcpy(child->cwd, session->cwd);
                memcpy(child->command_paths, session->command_paths,
                       sizeof(child->command_paths));
                child->command_path_count = session->command_path_count;
                memcpy(child->assigns, session->assigns, sizeof(child->assigns));
                memcpy(child->local_vars, session->local_vars,
                       sizeof(child->local_vars));
                child->return_code = session->return_code;
                child->result2 = session->result2;
                child->fail_level = session->fail_level;
                strcpy(child->prompt, session->prompt);
            }
        }
        break;
    }

    case AMIGA_BROKER_TALLY:
        switch (request.flags) {
        case AMIGA_BROKER_TALLY_ON:
            session->tally_enabled = 1;
            /* Anything counted before this moment was counted while nobody
               was watching, and belongs to no reporting period. */
            session->privileged_operations = 0;
            break;
        case AMIGA_BROKER_TALLY_OFF:
            session->tally_enabled = 0;
            session->privileged_operations = 0;
            break;
        case AMIGA_BROKER_TALLY_STATE:
            snprintf(result, sizeof(result), "%s",
                     session->tally_enabled ? "ON" : "OFF");
            break;
        case AMIGA_BROKER_TALLY_REPORT:
            snprintf(result, sizeof(result), "%lu",
                     session->tally_enabled ?
                         (unsigned long)session->privileged_operations : 0UL);
            /* Cleared whether or not it was reported, so the count always
               covers one prompt to the next rather than accumulating while
               the tally happens to be off. */
            session->privileged_operations = 0;
            break;
        default:
            status = EINVAL;
            break;
        }
        break;

    case AMIGA_BROKER_SAY_WARM:
        if (warm_say_voice(path) != 0)
            status = errno;
        break;

    case AMIGA_BROKER_SAY_STATUS: {
        size_t used = 0;

        reap_say_voices();
        for (size_t index = 0; index < MAX_SAY_VOICES; index++) {
            struct broker_say_voice *voice = &say_voices[index];
            int written;

            if (!voice->pid)
                continue;
            written = snprintf(result + used, sizeof(result) - used,
                               "%s\t%ld\n", voice->name, (long)voice->pid);
            if (written < 0 || (size_t)written >= sizeof(result) - used) {
                status = ENOSPC;
                break;
            }
            used += (size_t)written;
        }
        break;
    }

    case AMIGA_BROKER_SETRESULT: {
        char *end;
        long return_code = strtol(path, &end, 10);
        long result2;
        if (!path[0] || *end || !value[0]) {
            status = EINVAL;
            break;
        }
        result2 = strtol(value, &end, 10);
        if (*end || return_code < INT32_MIN || return_code > INT32_MAX ||
            result2 < INT32_MIN || result2 > INT32_MAX) {
            status = EINVAL;
        } else {
            session->return_code = (int32_t)return_code;
            session->result2 = (int32_t)result2;
        }
        break;
    }

    case AMIGA_BROKER_VIEWROOT:
        snprintf(result, sizeof(result), "%s", ace_dos_devices_view_root());
        break;

    case AMIGA_BROKER_PRIVOP:
        privop_used = 1;
        status = perform_privileged_operation(&request, path, value,
                                              &privop_fd);
        /* Only when it actually happened.  An operation the CRM could not
           perform either did not require root to complete -- it did not
           complete -- and counting the attempt would tell the user that
           privilege had been used where none had. */
        if (status == 0 && session->privileged_operations < UINT32_MAX)
            session->privileged_operations++;
        break;

    default:
        status = EINVAL;
        break;
    }
    if (privop_used) {
        /* Its own response, because it may carry a descriptor and the
           ordinary path cannot. */
        if (send_response_with_fd(fd, status, privop_fd) != 0)
            outcome = -1;
        if (privop_fd >= 0)
            close(privop_fd);
    } else if (status) {
        if (send_response(fd, status, strerror(status)) != 0)
            outcome = -1;
    } else if (request.operation == AMIGA_BROKER_RESOLVE ||
               request.operation == AMIGA_BROKER_RESOLVE_BENEATH ||
               request.operation == AMIGA_BROKER_NAMEFROMHOST ||
               request.operation == AMIGA_BROKER_GETCWD ||
               request.operation == AMIGA_BROKER_GETVAR ||
               request.operation == AMIGA_BROKER_LISTVARS ||
               request.operation == AMIGA_BROKER_GETCLI ||
               request.operation == AMIGA_BROKER_GETRESULT ||
               request.operation == AMIGA_BROKER_TALLY ||
               request.operation == AMIGA_BROKER_LISTDOS ||
               request.operation == AMIGA_BROKER_LISTASSIGNS ||
               request.operation == AMIGA_BROKER_LISTPATH ||
               request.operation == AMIGA_BROKER_TASK_ATTACH ||
               request.operation == AMIGA_BROKER_TASK_FIND ||
               request.operation == AMIGA_BROKER_PORT_ADD ||
               request.operation == AMIGA_BROKER_PORT_FIND ||
               request.operation == AMIGA_BROKER_PORT_ATTACH ||
               request.operation == AMIGA_BROKER_PORT_PUT ||
               request.operation == AMIGA_BROKER_PORT_BROADCAST ||
               request.operation == AMIGA_BROKER_TASK_LIST ||
               request.operation == AMIGA_BROKER_VIEWROOT ||
               request.operation == AMIGA_BROKER_STATUS ||
               request.operation == AMIGA_BROKER_SAY_STATUS) {
        if (send_response(fd, 0, result) != 0)
            outcome = -1;
    } else {
        if (send_response(fd, 0, NULL) != 0)
            outcome = -1;
    }
    /* PORT_ATTACH's ordinary response must be the first record the client
       reads. Only after it has accepted that response can the delivery
       reader consume replayed opaque broadcast records. */
    if (outcome == 0 && status == 0 &&
        request.operation == AMIGA_BROKER_PORT_ATTACH &&
        replay_broadcast_events(fd) != 0)
        outcome = -1;
    if (outcome == 0 && request.operation == AMIGA_BROKER_TASK_ATTACH &&
        session->foreground_task == connection->task_id &&
        session->pending_foreground_signals) {
        struct amiga_broker_task_signal signal;

        signal.magic = AMIGA_BROKER_MAGIC;
        signal.operation = AMIGA_BROKER_TASK_SIGNAL;
        signal.task_id = connection->task_id;
        signal.signals = session->pending_foreground_signals;
        session->pending_foreground_signals = 0;
        if (write_all(fd, &signal, sizeof(signal)) != 0)
            outcome = -1;
    }

done:
    free(session_id);
    free(path);
    free(value);
drop:
    /*
     * Always, forwarded or not. sendmsg() with SCM_RIGHTS *duplicates* a
     * descriptor into the receiver; it does not hand this process's copy
     * over. Leaving them open on the assumption that forwarding consumed them
     * leaks two descriptors per message with streams, and the broker outlives
     * every process that talks to it -- it would run out.
     */
    for (size_t index = 0; index < passed_fd_count; index++)
        close(passed_fds[index]);
    return outcome;
}

/*
 * Held connections, and the one place a session's lifetime is decided.
 *
 * The cap is a backstop, not a working limit: a connection lasts as long as
 * the process behind it, so this is one per live shell plus one per command
 * currently running, against a session table of sixty-four.
 */
#define MAX_CONNECTIONS 256

static struct broker_connection connections[MAX_CONNECTIONS];
static struct pollfd poll_fds[MAX_CONNECTIONS + 1];
static size_t connection_count;

static void stop_server(int signal_number);

/*
 * How long a broker nobody is attached to waits before exiting, in seconds.
 * Overridable so a test need not wait one out; zero means "leave with the
 * last connection".
 */
#define BROKER_IDLE_SECONDS (30ul * 60ul)

static unsigned long broker_idle_seconds(void)
{
    const char *configured = getenv("ACE_BROKER_IDLE_SECONDS");
    char *end;
    unsigned long seconds;

    if (!configured || !*configured) {
        return BROKER_IDLE_SECONDS;
    }
    errno = 0;
    seconds = strtoul(configured, &end, 10);
    if (errno || *end) {
        return BROKER_IDLE_SECONDS;
    }
    return seconds;
}

/*
 * When this broker exits if nobody attaches, or zero while somebody is.
 *
 * A broker belongs to its shells. It used to run until something sent it
 * SIGTERM, which left a machine accumulating unreachable brokers, each still
 * holding whatever it had been given. It waits rather than leaving with the
 * last shell because closing one window and opening another a moment later is
 * ordinary, and the second window is meant to find the first one's assigns,
 * variables and current directory.
 *
 * A connection stops the clock, not an attached session: a connection that
 * has not sent its ATTACH yet is a shell in the act of arriving, and timing
 * sessions would let a broker exit between accept() and ATTACH.
 */
static time_t idle_deadline;

static void refresh_idle_deadline(void)
{
    if (connection_count) {
        idle_deadline = 0;
        return;
    }
    idle_deadline = time(NULL) + (time_t)broker_idle_seconds();
}

/*
 * Closes a connection and, if it was the last one holding the session it
 * claimed, frees the session. This is the whole point of holding the
 * connection open: the kernel reports the close whether the shell exited
 * normally, was killed, or crashed, so a session's current directory,
 * assigns and variables are released exactly when their owner is gone
 * instead of being guessed at.
 */
static void drop_connection(size_t index)
{
    struct broker_connection *connection = &connections[index];

    drop_task_connection(connection);
    /* Order matters: release the messages this process can no longer answer
       before forgetting the messages it was itself waiting on. A process that
       is both a sender and a receiver would otherwise have its outgoing
       conversations dropped before its incoming ones were released. */
    drop_connection_ports(connection);
    drop_connection_port_channels(connection);
    drop_connection_port_messages(connection);

    if (connection->anchor >= 0) {
        struct broker_session *session = &sessions[connection->anchor];

        if (session->anchors && !--session->anchors)
            release_session(session);
        if (ace_mode_is_root() && root_shells && !--root_shells)
            stop_root_services();
    }
    close(connection->fd);
    connections[index] = connections[--connection_count];
    refresh_idle_deadline();
}

static void accept_connection(void)
{
    int fd = accept(server_fd, NULL, NULL);

    if (fd < 0)
        return;
    /* The broker execs mount(8) and udisksctl to bring volumes up
     * (dos_devices.c); without this every one of them would inherit every
     * client connection the broker is holding. */
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (connection_count == MAX_CONNECTIONS) {
        fprintf(stderr, "ace-broker: connection table full, refusing\n");
        close(fd);
        return;
    }
    connections[connection_count].fd = fd;
    connections[connection_count].anchor = -1;
    connections[connection_count].task_id = 0;
    connections[connection_count].port_channel_id = 0;
    connection_count++;
    refresh_idle_deadline();
}

/*
 * How long poll() may sleep: until the next idle voice is due to be reaped,
 * or until this broker's own idle deadline, whichever comes first.
 */
static int broker_poll_timeout(void)
{
    int timeout = say_poll_timeout();

    if (idle_deadline) {
        time_t now = time(NULL);
        long long remaining = now >= idle_deadline
                                  ? 0
                                  : (long long)(idle_deadline - now) * 1000;

        if (remaining > INT_MAX) {
            remaining = INT_MAX;
        }
        if (remaining < timeout) {
            timeout = (int)remaining;
        }
    }
    return timeout;
}

static int lock_fd = -1;
static bool already_running;

/*
 * Takes the exclusive lock that marks this process as the broker for this
 * socket path. The lock lives in a file beside the socket and is released by
 * the kernel when the process exits, however it exits, so a broker that
 * crashes does not leave the path permanently claimed.
 *
 * The client's own start lock (<socket>.start.lock, in broker_client.c) is a
 * different file: that one serialises "should I spawn a broker?" and is held
 * only across the spawn, and a broker taking it would deadlock against the
 * client waiting for that broker to come up.
 */
static int acquire_socket_lock(void)
{
    char lock_path[PATH_MAX];

    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", socket_path) >=
        (int)sizeof(lock_path)) {
        fprintf(stderr, "broker socket path is too long\n");
        return -1;
    }
    lock_fd = open(lock_path,
                   O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0) {
        perror("broker lock");
        return -1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            already_running = true;
            fprintf(stderr, "ace-broker: already running for %s\n",
                    socket_path);
        } else {
            perror("broker lock");
        }
        close(lock_fd);
        lock_fd = -1;
        return -1;
    }

    /* Record who holds it, so broker-stop can find this process without
     * guessing from command lines. The lock, not the contents, is what makes
     * this authoritative: a stale pid cannot be read out of an unlocked file
     * by anything that checks the lock first. */
    {
        char line[32];
        int length = snprintf(line, sizeof(line), "%ld\n", (long)getpid());

        if (ftruncate(lock_fd, 0) == 0 && length > 0)
            (void)!write(lock_fd, line, (size_t)length);
    }
    return 0;
}

/*
 * The idle exit.  Unlike SIGTERM this is not a signal handler, so it can be
 * polite: the mediator is told to go, and in the right order, rather than
 * left to read the EOF a dying broker leaves behind.
 */
static void stop_idle_server(void)
{
    stop_root_services();
    stop_server(0);
}

static void stop_server(int signal_number)
{
    (void)signal_number;
    stop_say_voices();
    ace_dos_devices_shutdown();
    if (server_fd >= 0)
        close(server_fd);
    unlink(socket_path);
    _exit(0);
}

int main(int argc, char **argv)
{
    struct sockaddr_un address;
    struct ace_mode_options modes;

    /* argv was copied here once, so that the process could re-execute itself
       through sudo with the switches it started with.  Nothing re-executes
       any more: privilege arrives as a separate process, not as a new self. */
    if (ace_mode_parse(&argc, argv, &modes) != 0) {
        fprintf(stderr, "usage: %s [--root] "
                        "[socket-path | --print-socket]\n", argv[0]);
        return 2;
    }
    if (argc == 2 && strcmp(argv[1], "--print-socket") == 0) {
        /* So the start/stop scripts do not have to reimplement the naming
           rule in shell and drift away from it. */
        if (ace_mode_configure_identity(&modes) != 0) {
            return 2;
        }
        printf("%s\n", amiga_broker_socket_path());
        return 0;
    }
    if (ace_mode_configure(&modes) != 0) {
        /* Being root is the one failure worth explaining rather than
           reporting: the user did something reasonable and needs to know that
           ACE gets its privilege from somewhere else entirely. */
        if (errno == EPERM)
            fprintf(stderr,
                    "ace-broker: ACE must be started as a normal user; "
                    "privileged operations\nare provided by the ACE "
                    "fmm.\n");
        else
            fprintf(stderr, "ace-broker: requested mode is unavailable: %s\n",
                    strerror(errno));
        return 1;
    }
    if (argc > 2 || (argc == 2 && argv[1][0] == '\0')) {
        fprintf(stderr, "usage: %s [--root] "
                        "[socket-path | --print-socket]\n", argv[0]);
        return 2;
    }
    socket_path = amiga_broker_socket_path();
    if (argc == 2)
        socket_path = argv[1];

    /*
     * One broker per socket, enforced by the kernel rather than by
     * convention. The lock is held for the broker's whole life, so a second
     * broker started on the same path finds it taken and leaves; without
     * this, the unlink() below silently stole the path from the live broker
     * and stranded it -- still listening, on a socket nobody could reach any
     * more, together with every session it was holding.
     *
     * Reaching this point means the lock is ours, so any socket still on
     * disk belongs to a broker that is gone, and clearing it is safe.
     */
    if (acquire_socket_lock() != 0)
        return already_running ? 0 : 1;

    broker_started = time(NULL);
    ace_dos_devices_discover();
    resolve_system_root();
    restore_environment_archive();

    signal(SIGINT, stop_server);
    signal(SIGTERM, stop_server);
    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    unlink(socket_path);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        fprintf(stderr, "broker socket path is too long\n");
        return 1;
    }
    strcpy(address.sun_path, socket_path);
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(socket_path, 0600) != 0 || listen(server_fd, 16) != 0) {
        perror("broker setup");
        return 1;
    }

    /* The clock starts here, not at the first attach: a broker started by
       hand, or left behind by a shell that died before it could attach, is
       exactly as idle as one whose last shell has gone. */
    refresh_idle_deadline();

    for (;;) {
        nfds_t watched = 1;

        poll_fds[0].fd = server_fd;
        poll_fds[0].events = POLLIN;
        poll_fds[0].revents = 0;
        for (size_t i = 0; i < connection_count; i++) {
            poll_fds[watched].fd = connections[i].fd;
            poll_fds[watched].events = POLLIN;
            poll_fds[watched].revents = 0;
            watched++;
        }

        if (poll(poll_fds, watched, broker_poll_timeout()) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return 1;
        }

        reap_say_voices();

        /*
         * Connections first, and backwards, so that dropping one can fill
         * its slot from the end of the table without disturbing an index
         * still to be visited.
         */
        for (size_t i = connection_count; i-- > 0;) {
            short events = poll_fds[i + 1].revents;

            if (!events)
                continue;
            if ((events & POLLIN) && handle_client(&connections[i]) == 0)
                continue;
            drop_connection(i);
        }

        if (poll_fds[0].revents & POLLIN)
            accept_connection();

        /* Last, so a connection that arrived on this same wakeup is accepted
           -- and has stopped the clock -- first. */
        if (idle_deadline && time(NULL) >= idle_deadline) {
            stop_idle_server();
        }
    }
}
