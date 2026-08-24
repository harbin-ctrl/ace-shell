#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "broker_client.h"
#include "broker_protocol.h"
#include "ace_modes.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static const char *broker_socket_path(void)
{
    return amiga_broker_socket_path();
}

static const char *broker_session(void)
{
    const char *session = getenv("ACE_SESSION");
    return session && *session ? session : "default";
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const char *bytes = buffer;
    while (length) {
        /* A broker can disappear between requests. Do not let a stale or
         * restarted broker turn that ordinary transport failure into a
         * process-wide SIGPIPE; broker_request() can then reconnect or return
         * a normal error to its caller. */
        ssize_t written = send(fd, bytes, length, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0)
            return -1;
        bytes += written;
        length -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, void *buffer, size_t length);

/*
 * Sends a buffer with descriptors attached, for the one operation that needs
 * them: a message carrying the sender's own streams.
 *
 * SCM_RIGHTS has to ride along with real data, so the descriptors go with the
 * first bytes and the rest follows normally. On a stream socket the ancillary
 * data is delivered with the byte range it was sent with, so a receiver that
 * reads that first range gets them.
 */
static int write_all_with_fds(int fd, const void *buffer, size_t length,
                              const int *fds, size_t fd_count)
{
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int) * AMIGA_BROKER_PORT_MAX_FDS)];
    } control;
    struct msghdr message;
    struct iovec vector;
    struct cmsghdr *header;
    ssize_t sent;

    if (!fd_count || fd_count > AMIGA_BROKER_PORT_MAX_FDS)
        return write_all(fd, buffer, length);
    memset(&control, 0, sizeof(control));
    memset(&message, 0, sizeof(message));
    vector.iov_base = (void *)buffer;
    vector.iov_len = length;
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
    header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
    memcpy(CMSG_DATA(header), fds, sizeof(int) * fd_count);
    do {
        sent = sendmsg(fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent <= 0)
        return -1;
    /* Whatever the first sendmsg did not take goes without ancillary data:
       the descriptors have already been handed over. */
    if ((size_t)sent < length)
        return write_all(fd, (const char *)buffer + sent, length - sent);
    return 0;
}

/*
 * Reads a fixed-size record and collects any descriptors that came with it.
 *
 * Only the first read can carry ancillary data, so it is the only one that
 * needs recvmsg; the remainder of a short read is ordinary.
 */
static int read_all_with_fds(int fd, void *buffer, size_t length,
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
    if (received <= 0)
        return -1;
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
            /* More than expected is a protocol error, but the descriptors are
               already ours and leaking them would be worse. */
            if (*fd_count < max_fds)
                fds[(*fd_count)++] = passed;
            else
                close(passed);
        }
    }
    if ((size_t)received < length)
        return read_all(fd, (char *)buffer + received, length - received);
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
        if (received == 0)
            return -1;
        bytes += received;
        length -= (size_t)received;
    }
    return 0;
}

/*
 * SOCK_CLOEXEC is not an optimisation here, it is what makes session
 * ownership mean anything. The connection below is held open for the life of
 * the process, and ACE runs every command by fork()ing and exec()ing a
 * separate one (native_command.c). Without CLOEXEC each of those children
 * would inherit the shell's connection, so a command that outlived its shell
 * would hold the session open after the shell it belongs to was gone --
 * exactly the leak this connection exists to close.
 */
static int connect_broker(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address;

    if (fd < 0)
        return -1;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(broker_socket_path()) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(address.sun_path, broker_socket_path());

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static int broker_is_reachable(void)
{
    int fd = connect_broker();

    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

static int broker_executable(char *result, size_t result_size)
{
    const char *configured = getenv("ACE_BROKER_BINARY");
    char executable[PATH_MAX];
    char *slash;
    ssize_t length;

    if (configured && *configured) {
        if (strlen(configured) >= result_size)
            return -1;
        strcpy(result, configured);
        return 0;
    }
    length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0 || (size_t)length >= sizeof(executable) - 1)
        return -1;
    executable[length] = '\0';
    slash = strrchr(executable, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    if (snprintf(result, result_size, "%s/ace-broker", executable) >=
        (int)result_size)
        return -1;
    return 0;
}

static void broker_sleep(void)
{
    struct timespec delay = {0, 20000000L};

    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
        ;
}

static int broker_wait_until_reachable(void)
{
    for (int attempt = 0; attempt < 100; attempt++) {
        if (broker_is_reachable() == 0)
            return 0;
        broker_sleep();
    }
    return -1;
}

/*
 * There used to be a join_broker_mount_namespace() here, and a
 * connect_broker() that called it before returning a connection.  In
 * front of the setns() sat an unshare(CLONE_FS), because some ACE executables
 * have the Exec host-signal dispatcher running before main(), threads share an
 * fs_struct by default, and Linux refuses a mount-namespace setns() with
 * EINVAL while that sharing lasts.
 *
 * All of it is gone, and not because the problem was solved.  No ACE client
 * enters a mount namespace any more, because none can: setns() with
 * CLONE_NEWNS requires CAP_SYS_ADMIN in the user namespace that owns the
 * target, and every process on this side of the design is now an ordinary
 * user process.  The device view is reached instead by asking the FMM/CRM service's
 * CRM to open an object and passing the descriptor back, which gets
 * across that boundary without anybody crossing it.
 *
 * Recorded rather than removed in silence, because the CLONE_FS discovery was
 * expensive and someone who reintroduces a setns() here will otherwise meet
 * the same EINVAL and think it is new.
 */

int native_broker_ensure(void)
{
    char lock_path[PATH_MAX];
    char executable[PATH_MAX];
    int lock_fd;
    pid_t child;

    if (broker_is_reachable() == 0)
        return 0;
    if (snprintf(lock_path, sizeof(lock_path), "%s.start.lock",
                 broker_socket_path()) >= (int)sizeof(lock_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                   0600);
    if (lock_fd < 0)
        return -1;
    if (flock(lock_fd, LOCK_EX) != 0) {
        close(lock_fd);
        return -1;
    }
    if (broker_is_reachable() == 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }
    if (broker_executable(executable, sizeof(executable)) != 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        errno = ENOENT;
        return -1;
    }

    child = fork();
    if (child == 0) {
        int null_fd;

        if (setsid() < 0)
            _exit(127);
        null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        execl(executable, executable, broker_socket_path(), (char *)NULL);
        _exit(127);
    }
    if (child < 0 || broker_wait_until_reachable() != 0) {
        if (child > 0)
            (void)kill(child, SIGTERM);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        errno = ECONNREFUSED;
        return -1;
    }
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    return 0;
}

/*
 * The connection to the broker, held open for the life of the process.
 *
 * It used to be one connect()/close() per request, which meant the broker
 * never learned that a shell had exited: sessions could only be reclaimed by
 * guesswork (evict the least recently used one), and a live but idle shell
 * could have its current directory, assigns and variables taken away from
 * underneath it. A connection that lasts as long as the process turns that
 * into something the kernel reports exactly.
 */
static int broker_fd = -1;
/* Set once native_broker_attach() succeeds; makes a replacement connection
   re-claim the session rather than silently becoming an ownerless user. */
static int broker_attached;
static int task_fd = -1;
static pthread_t task_thread;
static native_broker_task_signal_handler task_handler;
static void *task_handler_context;
/* The message-delivery channel: a third connection, held open for the life of
   the process once anything has used a public port.  Kept apart from task_fd
   for the reason AMIGA_BROKER_PORT_ATTACH gives -- a delivered message is
   variable length and can be large, a task signal is neither, and a Ctrl-C
   must not queue behind a script. */
static int port_fd = -1;
static pthread_t port_thread;
static native_broker_port_record_handler port_handler;
static void *port_handler_context;
static uint64_t port_channel_id;

#define NATIVE_BROKER_EXTRA_HANDLER_LIMIT 8
struct native_broker_extra_handler {
    native_broker_port_record_handler handler;
    void *context;
};
static struct native_broker_extra_handler
    port_extra_handlers[NATIVE_BROKER_EXTRA_HANDLER_LIMIT];
static size_t port_extra_handler_count;
static pthread_mutex_t port_handler_lock = PTHREAD_MUTEX_INITIALIZER;

static void *task_signal_reader(void *unused)
{
    struct amiga_broker_task_signal signal;

    (void)unused;
    while (read_all(task_fd, &signal, sizeof(signal)) == 0) {
        if (signal.magic == AMIGA_BROKER_MAGIC &&
            signal.operation == AMIGA_BROKER_TASK_SIGNAL && task_handler)
            task_handler(signal.signals, task_handler_context);
    }
    return NULL;
}

/*
 * Reads delivered messages and replies until the broker closes the channel.
 *
 * The payload is read even when the record is not one this process can use,
 * because the stream is framed by length: skipping the header alone would
 * leave the payload to be misread as the next header, and there is no way to
 * resynchronise afterwards.  An oversized length is the one case that cannot
 * be drained safely, so it ends the loop and closes the channel.
 */
static void *port_record_reader(void *unused)
{
    struct amiga_broker_port_record record;
    char *payload = NULL;
    int fds[AMIGA_BROKER_PORT_MAX_FDS];
    size_t fd_count = 0;

    (void)unused;
    while (read_all_with_fds(port_fd, &record, sizeof(record), fds,
                             AMIGA_BROKER_PORT_MAX_FDS, &fd_count) == 0) {
        native_broker_port_record_handler primary_handler;
        void *primary_context;
        struct native_broker_extra_handler extra_handlers[
            NATIVE_BROKER_EXTRA_HANDLER_LIMIT];
        size_t extra_handler_count;
        int stdin_fd = -1;
        int stdout_fd = -1;
        size_t next = 0;

        if (record.magic != AMIGA_BROKER_MAGIC ||
            record.payload_length > AMIGA_BROKER_MAX_PAYLOAD) {
            for (size_t index = 0; index < fd_count; index++)
                close(fds[index]);
            break;
        }
        if ((record.flags & AMIGA_BROKER_PORT_HAS_STDIN) && next < fd_count)
            stdin_fd = fds[next++];
        if ((record.flags & AMIGA_BROKER_PORT_HAS_STDOUT) && next < fd_count)
            stdout_fd = fds[next++];
        /* Anything the flags did not account for is closed rather than
           leaked; a descriptor handed over is ours whether we wanted it or
           not. */
        for (; next < fd_count; next++)
            close(fds[next]);
        payload = calloc((size_t)record.payload_length + 1, 1);
        if (!payload) {
            if (stdin_fd >= 0)
                close(stdin_fd);
            if (stdout_fd >= 0)
                close(stdout_fd);
            break;
        }
        if (record.payload_length &&
            read_all(port_fd, payload, record.payload_length) != 0) {
            free(payload);
            if (stdin_fd >= 0)
                close(stdin_fd);
            if (stdout_fd >= 0)
                close(stdout_fd);
            break;
        }
        pthread_mutex_lock(&port_handler_lock);
        primary_handler = port_handler;
        primary_context = port_handler_context;
        extra_handler_count = port_extra_handler_count;
        memcpy(extra_handlers, port_extra_handlers,
               extra_handler_count * sizeof(extra_handlers[0]));
        pthread_mutex_unlock(&port_handler_lock);
        if (primary_handler)
            primary_handler(record.operation, record.message_id,
                            record.port_id, payload,
                            (size_t)record.payload_length, stdin_fd, stdout_fd,
                            primary_context);
        for (size_t index = 0; index < extra_handler_count; index++)
            extra_handlers[index].handler(
                record.operation, record.message_id, record.port_id, payload,
                (size_t)record.payload_length, -1, -1,
                extra_handlers[index].context);
        if (!primary_handler && extra_handler_count == 0) {
            if (stdin_fd >= 0)
                close(stdin_fd);
            if (stdout_fd >= 0)
                close(stdout_fd);
        }
        free(payload);
        payload = NULL;
    }
    return NULL;
}

static void broker_disconnect(void)
{
    if (broker_fd >= 0)
        close(broker_fd);
    broker_fd = -1;
}

void native_broker_reset_after_fork(void)
{
    /* See broker_client.h for why: a fork()ed child that used the inherited
       fd would share one socket with the parent, and both would read and
       write on it with no coordination, desyncing the framing for whichever
       one loses the race for the other's bytes.  Closing costs the parent
       nothing -- each fd is its own reference to the same kernel object --
       and forgetting it here means the next call in this process opens a
       connection of its own. */
    if (broker_fd >= 0)
        close(broker_fd);
    broker_fd = -1;
    broker_attached = 0;
    /* task_fd's reader thread does not exist in the child -- fork() only
       carries over the calling thread -- so the fd is merely open and idle,
       not in a race.  It is still someone else's connection, and this
       process is not obliged to keep dragging it around until it happens to
       exec() or exit(). */
    if (task_fd >= 0)
        close(task_fd);
    task_fd = -1;
    /* Same reasoning for the port channel, with one addition: messages
       addressed to the parent must not be delivered to a child that merely
       inherited the fd.  Dropping it here means a child that goes on to use a
       port attaches a channel of its own, under its own pid. */
    if (port_fd >= 0)
        close(port_fd);
    port_fd = -1;
    port_handler = NULL;
    port_handler_context = NULL;
    port_extra_handler_count = 0;
}

/*
 * Outcome of one attempt on one connection:
 *   0  the broker answered, successfully
 *   1  the broker answered with an error status (errno set); the connection
 *      is fine and the request must not be retried -- the failure is the
 *      answer
 *  -1  the exchange itself broke; the connection is unusable
 */
/*
 * One request and its response are a unit on this socket, and there is one
 * socket per process rather than one per caller.  Interleaving two of them
 * does not lose a reply, it hands each caller the other's -- and the header
 * check below is what notices, which is why it already says a bad magic most
 * likely means "the tail of a previous reply".
 *
 * Serialised rather than given a connection per thread: an ACE process is
 * single-threaded almost all of the time, so the lock is uncontended, and a
 * second connection would need its own ATTACH and its own session identity
 * to go with it.  CreateNewProc() is what made this reachable -- an
 * AmigaDOS process created with NP_Entry runs in this address space, so
 * parent and child share this fd.
 *
 * The task socket at task_fd is deliberately not covered: it is a separate
 * connection read by its own thread, and carries no request/response pairs.
 */
static int broker_exchange_bytes_locked(int fd, uint32_t operation,
                                        const void *path, size_t path_length,
                                        const void *value, size_t value_length,
                                        uint32_t flags, const int *fds,
                                        size_t fd_count, void *result,
                                        size_t result_size,
                                        size_t *result_length);

static pthread_mutex_t broker_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * The counted form: path and value are arbitrary bytes, and the reply's
 * length is reported rather than inferred.
 *
 * Both used to be measured with strlen() here and with strlen() again in the
 * broker's reply, which made the protocol text-only even though the wire
 * format was counted all along -- path_length, value_length and
 * payload_length are all uint32 on the wire. A payload with a NUL in it was
 * silently cut short at the NUL.
 *
 * Exec did not work that way, and the thing this has to carry is an ARexx
 * argstring, which is counted bytes that may contain anything. So the counted
 * form is now the real implementation and the string form below is the
 * wrapper, rather than the other way round.
 */
static int broker_exchange_bytes(int fd, uint32_t operation,
                                 const void *path, size_t path_length,
                                 const void *value, size_t value_length,
                                 uint32_t flags, const int *fds,
                                 size_t fd_count, void *result,
                                 size_t result_size, size_t *result_length)
{
    int status;

    pthread_mutex_lock(&broker_lock);
    status = broker_exchange_bytes_locked(fd, operation, path, path_length,
                                          value, value_length, flags, fds,
                                          fd_count, result,
                                          result_size, result_length);
    pthread_mutex_unlock(&broker_lock);
    return status;
}

static int broker_exchange(int fd, uint32_t operation, const char *path,
                           const char *value, uint32_t flags,
                           char *result, size_t result_size)
{
    return broker_exchange_bytes(fd, operation, path, path ? strlen(path) : 0,
                                 value, value ? strlen(value) : 0, flags,
                                 NULL, 0, result, result_size, NULL);
}

static int broker_exchange_bytes_locked(int fd, uint32_t operation,
                                        const void *path, size_t path_length,
                                        const void *value, size_t value_length,
                                        uint32_t flags, const int *fds,
                                        size_t fd_count, void *result,
                                        size_t result_size,
                                        size_t *result_length)
{
    const char *session = broker_session();
    size_t session_length = strlen(session);
    struct amiga_broker_request request;
    struct amiga_broker_response response;

    /* Zeroed first, as privop_exchange() does.  Every field below is assigned
       except privop_time, which this path has no use for -- so without this
       the request carried whatever was on the stack, and the padding between
       the fields went out to the broker as well.  Harmless in what the broker
       does with it, and noisy where it matters: 483 valgrind reports in a run
       that was being read for a real memory error. */
    memset(&request, 0, sizeof(request));
    request.magic = AMIGA_BROKER_MAGIC;
    request.operation = operation;
    request.session_length = (uint32_t)session_length;
    request.path_length = (uint32_t)path_length;
    request.value_length = (uint32_t)value_length;
    request.flags = flags;
    request.mode = (ace_mode_is_root() ? AMIGA_BROKER_MODE_ROOT : 0) |
                   (ace_mode_is_device_view() ?
                        AMIGA_BROKER_MODE_DEVICEVIEW : 0);
    request.owner_uid = (uint32_t)ace_mode_owner_uid();
    request.privop = 0;
    request.privop_mode = 0;

    /* The descriptors ride with the request header, which is the first thing
       written and always non-empty. */
    if (write_all_with_fds(fd, &request, sizeof(request), fds, fd_count) != 0 ||
        write_all(fd, session, session_length) != 0 ||
        write_all(fd, path, path_length) != 0 ||
        write_all(fd, value, value_length) != 0 ||
        read_all(fd, &response, sizeof(response)) != 0)
        return -1;

    if (response.magic != AMIGA_BROKER_MAGIC) {
        /*
         * Either the running broker is a different build, or this connection
         * has lost sync and what was read is not a response header at all.
         * They are told apart by the rest of the header: a real broker of any
         * version sends a plausible length, so a wild one means the bytes are
         * something else entirely -- the tail of a previous reply, most
         * likely.  Saying "older broker, run broker-stop" for that would send
         * the reader after the wrong thing.
         *
         * Reported once per process: every ACE command would otherwise repeat
         * it, and a shell runs many.
         */
        static int reported;

        if (!reported) {
            unsigned theirs =
                (unsigned)amiga_broker_version_from_magic(response.magic);

            reported = 1;
            if (response.payload_length <= AMIGA_BROKER_MAX_PAYLOAD)
                fprintf(stderr,
                        "ace: the broker on %s speaks protocol 0x%08x; this "
                        "build speaks 0x%08x.\n"
                        "ace: it is an older or newer ace-broker still "
                        "running. Stop it with broker-stop and retry.\n",
                        broker_socket_path(), theirs,
                        (unsigned)AMIGA_BROKER_PROTOCOL_VERSION);
            else
                fprintf(stderr,
                        "ace: malformed reply from the broker on %s "
                        "(magic 0x%08x, length %u): this connection has lost "
                        "sync rather than met a different build.\n",
                        broker_socket_path(), (unsigned)response.magic,
                        (unsigned)response.payload_length);
        }
        errno = EPROTO;
        return -1;
    }
    if (response.payload_length > AMIGA_BROKER_MAX_PAYLOAD) {
        errno = EPROTO;
        return -1;
    }

    if (response.status != 0) {
        int error = response.status;
        char ignored[AMIGA_BROKER_MAX_PAYLOAD];

        if (response.payload_length &&
            read_all(fd, ignored, response.payload_length) != 0)
            return -1;
        errno = error;
        return 1;
    }

    /*
     * A caller that asked for no payload is not asking for a size limit: it
     * simply does not want the answer. Drain whatever arrived, to keep the
     * connection's framing, and report success.
     *
     * This used to fall into the size check below, where result_size 0 made
     * "payload_length >= result_size" true for even an empty reply -- so
     * every operation that returns nothing reported ENAMETOOLONG on success.
     * PORT_REM is the one such caller today, which is why a successful
     * DeletePort() came back as a failure; it went unnoticed because its only
     * caller casts the result to (void).
     */
    if (!result || result_size == 0) {
        char ignored[AMIGA_BROKER_MAX_PAYLOAD];

        if (response.payload_length &&
            read_all(fd, ignored, response.payload_length) != 0)
            return -1;
        if (result_length)
            *result_length = 0;
        return 0;
    }
    if (response.payload_length >= result_size) {
        char ignored[AMIGA_BROKER_MAX_PAYLOAD];

        if (response.payload_length &&
            read_all(fd, ignored, response.payload_length) != 0)
            return -1;
        errno = ENAMETOOLONG;
        return 1;
    }
    if (response.payload_length &&
        read_all(fd, result, response.payload_length) != 0)
        return -1;
    /* Still NUL-terminated: every string caller relies on it, and the
       ENAMETOOLONG check above guarantees the room. Callers that asked for
       bytes read the length instead and ignore the terminator. */
    if (result)
        ((char *)result)[response.payload_length] = '\0';
    if (result_length)
        *result_length = response.payload_length;
    return 0;
}

/*
 * Returns the live connection, opening one if there is none. A connection
 * opened here while this process owns a session re-sends ATTACH, so a broker
 * that was restarted, or a connection lost for any other reason, does not
 * quietly leave the session ownerless for the rest of the process's life.
 */
static int broker_connection(void)
{
    int fd;

    if (broker_fd >= 0)
        return broker_fd;
    fd = connect_broker();
    if (fd < 0)
        return -1;
    broker_fd = fd;
    if (broker_attached) {
        char ignored[1];

        if (broker_exchange(broker_fd, AMIGA_BROKER_ATTACH, NULL, NULL, 0,
                            ignored, sizeof(ignored)) < 0)
            broker_disconnect();
    }
    return broker_fd;
}

static int broker_request_bytes(uint32_t operation,
                                const void *path, size_t path_length,
                                const void *value, size_t value_length,
                                uint32_t flags, const int *fds,
                                size_t fd_count, void *result,
                                size_t result_size, size_t *result_length)
{
    const char *session = broker_session();

    if (strlen(session) > UINT32_MAX || path_length > UINT32_MAX ||
        value_length > UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    /*
     * Two attempts, because a held connection can die between requests --
     * the broker was restarted, or stopped and started by broker-stop /
     * broker-start. The first failure drops the dead connection and the
     * second attempt opens a fresh one. An error *from* the broker is an
     * answer, not a transport failure, and is never retried.
     */
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = broker_connection();
        int outcome;

        if (fd < 0) {
            if (native_broker_ensure() != 0)
                return -1;
            fd = broker_connection();
        }
        if (fd < 0)
            return -1;
        outcome = broker_exchange_bytes(fd, operation, path, path_length,
                                        value, value_length, flags, fds,
                                        fd_count, result, result_size,
                                        result_length);
        if (outcome == 0)
            return 0;
        if (outcome > 0)
            return -1;
        broker_disconnect();
    }
    return -1;
}

/*
 * One privileged file operation, asked of the broker.
 *
 * Its own exchange rather than a flag on the ordinary one, because it is the
 * only request whose reply can carry a descriptor and the only one that names
 * a FMM/CRM service operation.  Folding it into the general path would put an
 * ancillary-data read in front of every getvar in the system.
 *
 * The command never speaks to a root process.  It asks the broker, the broker
 * asks the CRM, and what comes back is a status and -- for the
 * operations that open something -- a handle to that one object.
 *
 * Three outcomes rather than two: 0 for success, 1 for an exchange that
 * completed and carried a refusal, -1 for an exchange that did not complete.
 * The caller needs the difference.  Only the middle one is an answer about
 * the object, and only an answer about the object is fit to replace what the
 * caller's own unprivileged attempt already said.
 */
static int privop_exchange(int fd, uint32_t privop, const char *path,
                           const char *second, uint32_t flags, uint32_t mode,
                           int64_t when, int *received_fd)
{
    const char *session = broker_session();
    size_t session_length = strlen(session);
    size_t path_length = path ? strlen(path) : 0;
    size_t second_length = second ? strlen(second) : 0;
    struct amiga_broker_request request;
    struct amiga_broker_response response;
    int fds[AMIGA_BROKER_PORT_MAX_FDS];
    size_t fd_count = 0;

    memset(&request, 0, sizeof(request));
    request.magic = AMIGA_BROKER_MAGIC;
    request.operation = AMIGA_BROKER_PRIVOP;
    request.session_length = (uint32_t)session_length;
    request.path_length = (uint32_t)path_length;
    request.value_length = (uint32_t)second_length;
    request.flags = flags;
    request.privop_time = when;
    /* The session's privilege and view, as every request carries: the broker
       refuses a client that believes it is in a different session. */
    request.mode = (ace_mode_is_root() ? AMIGA_BROKER_MODE_ROOT : 0) |
                   (ace_mode_is_device_view() ?
                        AMIGA_BROKER_MODE_DEVICEVIEW : 0);
    request.owner_uid = (uint32_t)ace_mode_owner_uid();
    request.privop = privop;
    request.privop_mode = mode;

    if (write_all(fd, &request, sizeof(request)) != 0 ||
        write_all(fd, session, session_length) != 0 ||
        write_all(fd, path, path_length) != 0 ||
        write_all(fd, second, second_length) != 0)
        return -1;
    if (read_all_with_fds(fd, &response, sizeof(response), fds,
                          AMIGA_BROKER_PORT_MAX_FDS, &fd_count) != 0)
        return -1;
    if (response.magic != AMIGA_BROKER_MAGIC) {
        while (fd_count--)
            close(fds[fd_count]);
        errno = EPROTO;
        return -1;
    }
    /* A descriptor that arrived without the flag, or a flag without its
       descriptor, means the two sides disagree about what was sent.  Close
       what came and refuse: guessing here would hand a command a handle to
       something nobody named. */
    if ((response.flags & AMIGA_BROKER_RESPONSE_HAS_FD) != 0) {
        if (fd_count != 1) {
            while (fd_count--)
                close(fds[fd_count]);
            errno = EPROTO;
            return -1;
        }
        if (received_fd)
            *received_fd = fds[0];
        else
            close(fds[0]);
    } else {
        while (fd_count--)
            close(fds[fd_count]);
    }
    if (response.status != 0) {
        errno = response.status;
        return 1;
    }
    return 0;
}

int native_broker_privop(uint32_t privop, const char *path, const char *second,
                         uint32_t flags, uint32_t mode, int64_t when,
                         int *received_fd)
{
    if (received_fd)
        *received_fd = -1;
    /* Two attempts, for the same reason every other request gets two: a held
       connection can die between requests.  A refusal from the broker is an
       answer and is never retried. */
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = broker_connection();
        int outcome;

        if (fd < 0) {
            if (native_broker_ensure() != 0)
                return -1;
            fd = broker_connection();
        }
        if (fd < 0)
            return -1;
        outcome = privop_exchange(fd, privop, path, second, flags, mode, when,
                                  received_fd);
        if (outcome >= 0)
            return outcome;
        broker_disconnect();
    }
    return -1;
}

/*
 * The string form, which is what almost every operation wants: a name, a
 * path, a list. Those are text in AmigaDOS too, so measuring them with
 * strlen() is right rather than merely convenient.
 */
static int broker_request(uint32_t operation, const char *path,
                          const char *value, uint32_t flags,
                          char *result, size_t result_size)
{
    return broker_request_bytes(operation, path, path ? strlen(path) : 0,
                                value, value ? strlen(value) : 0, flags,
                                NULL, 0, result, result_size, NULL);
}

/*
 * Claims this process's session, tying its lifetime to this process.
 *
 * Only a shell should call this. Everything else -- commands, brokerctl --
 * is a transient user of a session it does not own, and a session with no
 * owner keeps the old behaviour of surviving between separate processes,
 * which is what makes the documented standalone command sequences work.
 */
int native_broker_attach(void)
{
    char ignored[1];
    int fd, outcome;

    if (broker_attached)
        return 0;
    fd = broker_connection();
    if (fd < 0)
        return -1;
    outcome = broker_exchange(fd, AMIGA_BROKER_ATTACH, NULL, NULL, 0, ignored,
                              sizeof(ignored));
    if (outcome < 0) {
        broker_disconnect();
        return -1;
    }
    if (outcome > 0)
        return -1;
    broker_attached = 1;
    return 0;
}

int native_broker_task_attach(const char *name,
                              native_broker_task_signal_handler handler,
                              void *context, uint64_t *task_id)
{
    char result[32];
    int outcome;

    if (!name || !*name || !handler || !task_id || task_fd >= 0) {
        errno = EINVAL;
        return -1;
    }
    task_fd = connect_broker();
    if (task_fd < 0)
        return -1;
    char pid[32];

    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    outcome = broker_exchange(task_fd, AMIGA_BROKER_TASK_ATTACH, name, pid,
                              0, result, sizeof(result));
    if (outcome != 0) {
        close(task_fd);
        task_fd = -1;
        return -1;
    }
    *task_id = strtoull(result, NULL, 10);
    if (!*task_id) {
        close(task_fd);
        task_fd = -1;
        errno = EPROTO;
        return -1;
    }
    task_handler = handler;
    task_handler_context = context;
    if (pthread_create(&task_thread, NULL, task_signal_reader, NULL) != 0) {
        close(task_fd);
        task_fd = -1;
        task_handler = NULL;
        errno = EAGAIN;
        return -1;
    }
    (void)pthread_detach(task_thread);
    return 0;
}

/*
 * Opens this process's delivery channel, or confirms it is already open.
 *
 * Idempotent on purpose: every entry point that can be the first port use in
 * a process calls it, and which one gets there first depends on whether this
 * process is registering a port or sending to somebody else's.  A second call
 * that names the same handler is the normal case and must not fail.
 */
int native_broker_port_attach(native_broker_port_record_handler handler,
                              void *context, uint64_t *channel_id)
{
    char result[32];
    char pid[32];
    uint64_t id;
    int outcome;

    if (!handler) {
        errno = EINVAL;
        return -1;
    }
    if (port_fd >= 0) {
        if (port_handler != handler || port_handler_context != context) {
            errno = EBUSY;
            return -1;
        }
        if (channel_id)
            *channel_id = port_channel_id;
        return 0;
    }
    port_fd = connect_broker();
    if (port_fd < 0)
        return -1;
    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    outcome = broker_exchange(port_fd, AMIGA_BROKER_PORT_ATTACH, pid, NULL,
                              0, result, sizeof(result));
    if (outcome != 0) {
        close(port_fd);
        port_fd = -1;
        return -1;
    }
    id = strtoull(result, NULL, 10);
    if (!id) {
        close(port_fd);
        port_fd = -1;
        errno = EPROTO;
        return -1;
    }
    /* Published before the thread starts, so the first record cannot arrive
       to find no handler set. */
    port_handler = handler;
    port_handler_context = context;
    port_channel_id = id;
    if (pthread_create(&port_thread, NULL, port_record_reader, NULL) != 0) {
        close(port_fd);
        port_fd = -1;
        port_handler = NULL;
        port_handler_context = NULL;
        port_channel_id = 0;
        errno = EAGAIN;
        return -1;
    }
    (void)pthread_detach(port_thread);
    if (channel_id)
        *channel_id = id;
    return 0;
}

int native_broker_port_add_handler(native_broker_port_record_handler handler,
                                   void *context)
{
    if (!handler) {
        errno = EINVAL;
        return -1;
    }
    if (port_fd < 0)
        return native_broker_port_attach(handler, context, NULL);
    pthread_mutex_lock(&port_handler_lock);
    if (port_handler == handler && port_handler_context == context)
        goto already_registered;
    for (size_t index = 0; index < port_extra_handler_count; index++)
        if (port_extra_handlers[index].handler == handler &&
            port_extra_handlers[index].context == context)
            goto already_registered;
    if (port_extra_handler_count >= NATIVE_BROKER_EXTRA_HANDLER_LIMIT) {
        pthread_mutex_unlock(&port_handler_lock);
        errno = EBUSY;
        return -1;
    }
    port_extra_handlers[port_extra_handler_count].handler = handler;
    port_extra_handlers[port_extra_handler_count].context = context;
    port_extra_handler_count++;
already_registered:
    pthread_mutex_unlock(&port_handler_lock);
    return 0;
}

/*
 * Sends a message to a named port, and answers one that arrived.
 *
 * The name rather than an id, so that finding the owner and handing it the
 * message cannot be interleaved with the port going away -- see
 * AMIGA_BROKER_PORT_PUT. The payload is counted bytes and is never inspected
 * on the way through.
 *
 * Both require this process to have a delivery channel already
 * (native_broker_port_attach): the sender needs one to be given the reply,
 * which is why even a process owning no port must attach before it can send.
 */
int native_broker_port_put(const char *name, const void *message,
                           size_t length, int stdin_fd, int stdout_fd,
                           uint64_t *message_id)
{
    char result[32];
    size_t result_length = 0;
    int fds[AMIGA_BROKER_PORT_MAX_FDS];
    size_t fd_count = 0;
    uint32_t flags = 0;

    if (!name || !*name || (length && !message)) {
        errno = EINVAL;
        return -1;
    }
    if (port_fd < 0) {
        errno = ENOTCONN;
        return -1;
    }
    /* Order is fixed: stdin first when present, then stdout. The flags say
       which, because either can be absent on its own. */
    if (stdin_fd >= 0) {
        flags |= AMIGA_BROKER_PORT_HAS_STDIN;
        fds[fd_count++] = stdin_fd;
    }
    if (stdout_fd >= 0) {
        flags |= AMIGA_BROKER_PORT_HAS_STDOUT;
        fds[fd_count++] = stdout_fd;
    }
    if (broker_request_bytes(AMIGA_BROKER_PORT_PUT, name, strlen(name),
                             message, length, flags, fds, fd_count, result,
                             sizeof(result), &result_length) != 0)
        return -1;
    result[result_length] = '\0';
    if (message_id)
        *message_id = strtoull(result, NULL, 10);
    return 0;
}

int native_broker_port_reply(uint64_t message_id, const void *reply,
                             size_t length)
{
    char id_text[32];

    if (!message_id || (length && !reply)) {
        errno = EINVAL;
        return -1;
    }
    snprintf(id_text, sizeof(id_text), "%llu",
             (unsigned long long)message_id);
    return broker_request_bytes(AMIGA_BROKER_PORT_REPLY, id_text,
                                strlen(id_text), reply, length, 0, NULL, 0,
                                NULL, 0, NULL);
}

int native_broker_port_broadcast(const void *event, size_t length)
{
    if (length && !event) {
        errno = EINVAL;
        return -1;
    }
    return broker_request_bytes(AMIGA_BROKER_PORT_BROADCAST, NULL, 0,
                                event, length, 0, NULL, 0, NULL, 0, NULL);
}

/*
 * Public ports. The broker holds names, not ports: what comes back is an id
 * standing for "the port some process registered under this name", which is
 * all another process can be told about memory it does not share.
 */
int native_broker_port_add(const char *name, uint64_t *port_id)
{
    char result[32];
    char pid_text[32];

    if (!name || !port_id)
        return -1;
    snprintf(pid_text, sizeof(pid_text), "%ld", (long)getpid());
    if (broker_request(AMIGA_BROKER_PORT_ADD, name, pid_text, 0, result,
                       sizeof(result)) != 0)
        return -1;
    *port_id = strtoull(result, NULL, 10);
    return *port_id ? 0 : -1;
}

int native_broker_port_remove(uint64_t port_id)
{
    char id_text[32];

    snprintf(id_text, sizeof(id_text), "%llu", (unsigned long long)port_id);
    return broker_request(AMIGA_BROKER_PORT_REM, id_text, NULL, 0, NULL, 0);
}

int native_broker_port_find(const char *name, uint64_t *port_id)
{
    char result[32];

    if (!name || !port_id ||
        broker_request(AMIGA_BROKER_PORT_FIND, name, NULL, 0, result,
                       sizeof(result)) != 0)
        return -1;
    *port_id = strtoull(result, NULL, 10);
    return *port_id ? 0 : -1;
}

int native_broker_task_find(const char *name, uint64_t *task_id)
{
    char result[32];

    if (!name || !task_id || broker_request(AMIGA_BROKER_TASK_FIND, name,
                                            NULL, 0, result,
                                            sizeof(result)) != 0)
        return -1;
    *task_id = strtoull(result, NULL, 10);
    return *task_id ? 0 : -1;
}

int native_broker_task_signal(uint64_t task_id, uint32_t signals)
{
    char id[32];
    char mask[32];
    char ignored[1];

    if (!task_id) {
        errno = EINVAL;
        return -1;
    }
    snprintf(id, sizeof(id), "%llu", (unsigned long long)task_id);
    snprintf(mask, sizeof(mask), "%u", signals);
    return broker_request(AMIGA_BROKER_TASK_SIGNAL, id, mask, 0, ignored,
                          sizeof(ignored));
}

int native_broker_task_set_foreground_pid(pid_t pid)
{
    char value[32];
    char ignored[1];

    snprintf(value, sizeof(value), "%ld", (long)pid);
    return broker_request(AMIGA_BROKER_TASK_SET_FOREGROUND, value, NULL, 0,
                          ignored, sizeof(ignored));
}

int native_broker_task_break_foreground(uint32_t signals)
{
    char mask[32];
    char ignored[1];
    int fd;
    int result;

    if (native_broker_ensure() != 0)
        return -1;
    fd = connect_broker();
    if (fd < 0)
        return -1;
    snprintf(mask, sizeof(mask), "%u", signals);
    result = broker_exchange(fd, AMIGA_BROKER_TASK_BREAK_FOREGROUND, mask,
                             NULL, 0, ignored, sizeof(ignored));
    close(fd);
    return result == 0 ? 0 : -1;
}

int native_broker_task_list(char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_TASK_LIST, NULL, NULL, 0, result,
                          result_size);
}

/*
 * The session's tally of operations that needed root, and its on/off state.
 *
 * One call covers all four because they are one piece of session state; the
 * flags say which part of it is wanted.  The shell sends REPORT before each
 * prompt, and Tally sends the rest.
 */
int native_broker_tally(uint32_t what, char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_TALLY, "", "", what, result,
                          result_size);
}

int native_broker_say_warm(const char *voice)
{
    char ignored[1];

    if (!voice || !*voice) {
        errno = EINVAL;
        return -1;
    }
    return broker_request(AMIGA_BROKER_SAY_WARM, voice, NULL, 0, ignored,
                          sizeof(ignored));
}

int native_broker_say_status(char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_SAY_STATUS, NULL, NULL, 0, result,
                          result_size);
}

int native_broker_view_root(char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_VIEWROOT, NULL, NULL, 0, result,
                          result_size);
}

int native_broker_resolve_path(const char *path, char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_RESOLVE, path, NULL, 0, result, result_size);
}

/*
 * The same resolution, by a caller that means to open what comes back.
 *
 * Separate from the plain form rather than a mode of it, because most callers
 * are not opening anything and must not inherit the refusal: a name is still
 * a name whatever kind of object wears it.
 */
int native_broker_resolve_path_for_open(const char *path, char *result,
                                        size_t result_size)
{
    return broker_request(AMIGA_BROKER_RESOLVE, path, NULL,
                          AMIGA_BROKER_FLAG_FOR_OPEN, result, result_size);
}

int native_broker_resolve_beneath(const char *base, const char *relative,
                                  char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_RESOLVE_BENEATH, base, relative, 0,
                          result, result_size);
}

int native_broker_name_from_host(const char *path, char *result,
                                 size_t result_size)
{
    return broker_request(AMIGA_BROKER_NAMEFROMHOST, path, NULL, 0,
                          result, result_size);
}

int native_broker_getcwd(char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_GETCWD, NULL, NULL, 0, result, result_size);
}

int native_broker_setcwd(const char *path)
{
    char ignored[1];
    return broker_request(AMIGA_BROKER_SETCWD, path, NULL,
                          AMIGA_BROKER_PATH_HOST, ignored, sizeof(ignored));
}

int native_broker_assign(const char *name, const char *path)
{
    return native_broker_assign_ex(name, path, 0);
}

int native_broker_assign_ex(const char *name, const char *path,
                            uint32_t flags)
{
    char ignored[1];
    return broker_request(AMIGA_BROKER_ASSIGN, name, path, flags,
                          ignored, sizeof(ignored));
}

int native_broker_listassigns(char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_LISTASSIGNS, NULL, NULL, 0,
                          result, result_size);
}

int native_broker_getvar(const char *name, uint32_t flags,
                         char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_GETVAR, name, NULL, flags,
                          result, result_size);
}

int native_broker_setvar(const char *name, const char *value, uint32_t flags)
{
    char ignored[1];
    return broker_request(AMIGA_BROKER_SETVAR, name, value, flags,
                          ignored, sizeof(ignored));
}

int native_broker_deletevar(const char *name, uint32_t flags)
{
    char ignored[1];
    return broker_request(AMIGA_BROKER_DELVAR, name, NULL, flags,
                          ignored, sizeof(ignored));
}

int native_broker_listvars(uint32_t flags, char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_LISTVARS, NULL, NULL, flags,
                          result, result_size);
}

int native_broker_getcli(char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_GETCLI, NULL, NULL, 0,
                          result, result_size);
}

int native_broker_setfaillevel(int32_t fail_level)
{
    char value[32];
    char ignored[1];

    snprintf(value, sizeof(value), "%ld", (long)fail_level);
    return broker_request(AMIGA_BROKER_SETFAILLEVEL, value, NULL, 0,
                          ignored, sizeof(ignored));
}

int native_broker_setprompt(const char *prompt)
{
    char ignored[1];
    return broker_request(AMIGA_BROKER_SETPROMPT, prompt, NULL, 0,
                          ignored, sizeof(ignored));
}

int native_broker_clone_session(const char *child_session)
{
    char ignored[1];
    return broker_request(AMIGA_BROKER_CLONESESSION, child_session, NULL, 0,
                          ignored, sizeof(ignored));
}

int native_broker_getresult(char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_GETRESULT, NULL, NULL, 0,
                          result, result_size);
}

int native_broker_setresult(int32_t return_code, int32_t result2)
{
    char return_text[32];
    char result_text[32];
    char ignored[1];

    snprintf(return_text, sizeof(return_text), "%ld", (long)return_code);
    snprintf(result_text, sizeof(result_text), "%ld", (long)result2);
    return broker_request(AMIGA_BROKER_SETRESULT, return_text, result_text, 0,
                          ignored, sizeof(ignored));
}

int native_broker_listdos(char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_LISTDOS, NULL, NULL, 0, result,
                          result_size);
}

int native_broker_relabel(const char *drive, const char *name)
{
    char ignored[1];

    return broker_request(AMIGA_BROKER_RELABEL, drive, name, 0,
                          ignored, sizeof(ignored));
}

int native_broker_listpath(char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_LISTPATH, NULL, NULL, 0,
                          result, result_size);
}

int native_broker_path(const char *path, uint32_t flags)
{
    char ignored[1];

    return broker_request(AMIGA_BROKER_PATH, path, NULL, flags,
                          ignored, sizeof(ignored));
}

int native_broker_status(char *result, size_t result_size)
{
    return broker_request(AMIGA_BROKER_STATUS, NULL, NULL, 0, result,
                          result_size);
}
