#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <dos/dos.h>

#include "lnx_pty.h"

#define RELAY_BUFFER_SIZE 65536

/*
 * LNX is the deliberate escape hatch from the AmigaDOS command world.
 * It executes one Linux program directly. No shell is involved: arguments
 * are already separated by the AROS command line machinery, and the
 * inherited standard descriptors lead back to ACE's CON: stream.
 */
static int exec_linux_program(const char *program, char **arguments)
{
    const char *path;
    const char *cursor;
    int saved_error = ENOENT;

    if (strchr(program, '/')) {
        execv(program, arguments);
        return -1;
    }

    path = getenv("PATH");
    if (!path || !*path)
        path = ".";
    cursor = path;
    while (1) {
        const char *separator = strchr(cursor, ':');
        size_t directory_length = separator ?
                                  (size_t)(separator - cursor) : strlen(cursor);
        char candidate[PATH_MAX];
        int written;

        written = snprintf(candidate, sizeof(candidate), "%.*s%s%s",
                           (int)directory_length,
                           directory_length ? cursor : ".",
                           directory_length ? "/" : "",
                           program);
        if (written >= 0 && (size_t)written < sizeof(candidate)) {
            execv(candidate, arguments);
            if (errno != ENOENT && errno != ENOTDIR)
                saved_error = errno;
        } else {
            saved_error = ENAMETOOLONG;
        }
        if (!separator)
            break;
        cursor = separator + 1;
    }
    errno = saved_error;
    return -1;
}

struct relay_buffer {
    unsigned char bytes[RELAY_BUFFER_SIZE];
    size_t offset;
    size_t length;
};

static size_t relay_pending(const struct relay_buffer *buffer)
{
    return buffer->length - buffer->offset;
}

static size_t relay_space(struct relay_buffer *buffer)
{
    if (buffer->offset && buffer->length == RELAY_BUFFER_SIZE) {
        size_t pending = relay_pending(buffer);

        memmove(buffer->bytes, buffer->bytes + buffer->offset, pending);
        buffer->length = pending;
        buffer->offset = 0;
    }
    return RELAY_BUFFER_SIZE - buffer->length;
}

static void relay_consume(struct relay_buffer *buffer, size_t amount)
{
    buffer->offset += amount;
    if (buffer->offset == buffer->length)
        buffer->offset = buffer->length = 0;
}

static int set_nonblocking(int descriptor)
{
    int flags = fcntl(descriptor, F_GETFL);

    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static void restore_default_signals(void)
{
    struct sigaction action;
    int signal_number;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (signal_number = 1; signal_number < NSIG; signal_number++) {
        if (signal_number != SIGKILL && signal_number != SIGSTOP)
            (void)sigaction(signal_number, &action, NULL);
    }
}

static void close_unrelated_descriptors(void)
{
    long maximum;
    int descriptor;

    if (syscall(SYS_close_range, STDERR_FILENO + 1, UINT_MAX, 0) == 0)
        return;
    maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < STDERR_FILENO + 1)
        maximum = 1024;
    for (descriptor = STDERR_FILENO + 1; descriptor < maximum; descriptor++)
        (void)close(descriptor);
}

static void child_exec(const char *program, char **arguments, int master,
                       int slave)
{
    sigset_t empty_mask;

    if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0)
        _exit(RETURN_FAIL);
    restore_default_signals();
    sigemptyset(&empty_mask);
    (void)sigprocmask(SIG_SETMASK, &empty_mask, NULL);
    if (dup2(slave, STDIN_FILENO) < 0 ||
        dup2(slave, STDOUT_FILENO) < 0 ||
        dup2(slave, STDERR_FILENO) < 0)
        _exit(RETURN_FAIL);
    if (slave > STDERR_FILENO)
        close(slave);
    close(master);
    close_unrelated_descriptors();
    (void)unsetenv(ACE_LNX_PTY_VARIABLE);
    if (setenv("TERM", ACE_LNX_PTY_TERM, 1) != 0)
        _exit(RETURN_FAIL);
    if (exec_linux_program(program, arguments) < 0) {
        int saved_error = errno;

        dprintf(STDERR_FILENO, "LNX: %s: %s\n", program,
                strerror(saved_error));
        _exit(RETURN_FAIL);
    }
    _exit(RETURN_FAIL);
}

static int open_pty_pair(int *master_result, int *slave_result)
{
    char slave_name[PATH_MAX];
    int master;
    int slave;
    int error;

    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0)
        return -1;
    if (grantpt(master) < 0 || unlockpt(master) < 0) {
        error = errno;
        close(master);
        errno = error;
        return -1;
    }
    error = ptsname_r(master, slave_name, sizeof(slave_name));
    if (error != 0) {
        close(master);
        errno = error;
        return -1;
    }
    slave = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        error = errno;
        close(master);
        errno = error;
        return -1;
    }
    *master_result = master;
    *slave_result = slave;
    return 0;
}

static int initialize_pty_termios(int slave)
{
    struct termios attributes;

    if (tcgetattr(slave, &attributes) < 0)
        return -1;
    attributes.c_lflag |= ICANON | ECHO | ISIG;
    attributes.c_iflag |= ICRNL;
    attributes.c_oflag |= OPOST | ONLCR;
    return tcsetattr(slave, TCSANOW, &attributes);
}

static void terminate_child(pid_t child)
{
    if (child > 0)
        (void)kill(child, SIGHUP);
}

static void relay_read_input(int descriptor, struct relay_buffer *buffer,
                             int *input_open)
{
    ssize_t amount;
    size_t space = relay_space(buffer);

    if (!space)
        return;
    amount = read(descriptor, buffer->bytes + buffer->length, space);
    if (amount > 0)
        buffer->length += (size_t)amount;
    else if (amount == 0 || (errno != EINTR && errno != EAGAIN &&
                             errno != EWOULDBLOCK))
        *input_open = 0;
}

static void relay_write_master(int master, struct relay_buffer *buffer,
                               int *master_write_open)
{
    ssize_t amount;

    if (!relay_pending(buffer))
        return;
    amount = write(master, buffer->bytes + buffer->offset,
                   relay_pending(buffer));
    if (amount > 0)
        relay_consume(buffer, (size_t)amount);
    else if (amount < 0 && errno != EINTR && errno != EAGAIN &&
             errno != EWOULDBLOCK)
        *master_write_open = 0;
}

/* Return nonzero when the nonblocking master had no bytes ready. */
static int relay_read_master(int master, struct relay_buffer *buffer,
                             int *master_read_open)
{
    ssize_t amount;
    size_t space = relay_space(buffer);

    if (!space)
        return 0;
    amount = read(master, buffer->bytes + buffer->length, space);
    if (amount > 0) {
        buffer->length += (size_t)amount;
        return 0;
    }
    if (amount == 0 || (amount < 0 && (errno == EIO || errno == EBADF))) {
        *master_read_open = 0;
        return 0;
    }
    if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 1;
    if (amount < 0 && errno != EINTR)
        *master_read_open = 0;
    return 0;
}

static void relay_write_output(int descriptor, struct relay_buffer *buffer,
                               int *output_open)
{
    ssize_t amount;

    if (!relay_pending(buffer))
        return;
    amount = write(descriptor, buffer->bytes + buffer->offset,
                   relay_pending(buffer));
    if (amount > 0)
        relay_consume(buffer, (size_t)amount);
    else if (amount < 0 && errno != EINTR && errno != EAGAIN &&
             errno != EWOULDBLOCK)
        *output_open = 0;
}

static int supervise_pty(const char *program, char **arguments)
{
    struct relay_buffer input = {0};
    struct relay_buffer output = {0};
    struct pollfd descriptors[3];
    int descriptor_count;
    int input_open = 1;
    int output_open = 1;
    int master_read_open = 1;
    int master_write_open = 1;
    int master;
    int slave;
    int status = RETURN_FAIL;
    int child_reaped = 0;
    pid_t child;

    if (open_pty_pair(&master, &slave) < 0) {
        fprintf(stderr, "LNX: cannot allocate PTY: %s\n", strerror(errno));
        return RETURN_FAIL;
    }
    if (initialize_pty_termios(slave) < 0 ||
        set_nonblocking(master) < 0 || set_nonblocking(STDIN_FILENO) < 0 ||
        set_nonblocking(STDOUT_FILENO) < 0) {
        int error = errno;

        close(slave);
        close(master);
        fprintf(stderr, "LNX: cannot initialize PTY: %s\n", strerror(error));
        return RETURN_FAIL;
    }
    child = fork();
    if (child < 0) {
        int error = errno;

        close(slave);
        close(master);
        fprintf(stderr, "LNX: cannot fork PTY target: %s\n", strerror(error));
        return RETURN_FAIL;
    }
    if (child == 0)
        child_exec(program, arguments, master, slave);
    close(slave);

    while (1) {
        pid_t waited;
        int no_master_data = 0;

        if (!child_reaped) {
            waited = waitpid(child, &status, WNOHANG);
            if (waited == child)
                child_reaped = 1;
            else if (waited < 0 && errno != EINTR) {
                terminate_child(child);
                (void)waitpid(child, &status, 0);
                child_reaped = 1;
                status = RETURN_FAIL;
            }
        }
        if (child_reaped) {
            input_open = 0;
            input.offset = input.length = 0;
            master_write_open = 0;
        }
        if (!output_open) {
            output.offset = output.length = 0;
            master_read_open = 0;
        }
        if (child_reaped && master_read_open && output_open &&
            relay_space(&output) > 0)
            no_master_data = relay_read_master(master, &output,
                                               &master_read_open);
        if (child_reaped && no_master_data)
            master_read_open = 0;
        if (child_reaped && !master_read_open && !relay_pending(&output))
            break;

        descriptor_count = 0;
        if (input_open && relay_space(&input) > 0) {
            descriptors[descriptor_count].fd = STDIN_FILENO;
            descriptors[descriptor_count].events = POLLIN | POLLHUP;
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }
        if (master_read_open || (master_write_open && relay_pending(&input))) {
            descriptors[descriptor_count].fd = master;
            descriptors[descriptor_count].events =
                (master_read_open ? POLLIN | POLLHUP : 0) |
                (master_write_open && relay_pending(&input) ? POLLOUT : 0);
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }
        if (output_open && relay_pending(&output)) {
            descriptors[descriptor_count].fd = STDOUT_FILENO;
            descriptors[descriptor_count].events = POLLOUT;
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }
        if (!descriptor_count) {
            terminate_child(child_reaped ? -1 : child);
            if (!child_reaped) {
                (void)waitpid(child, &status, 0);
                child_reaped = 1;
            }
            master_read_open = 0;
            continue;
        }
        if (poll(descriptors, (nfds_t)descriptor_count, 100) < 0) {
            if (errno == EINTR)
                continue;
            terminate_child(child_reaped ? -1 : child);
            if (!child_reaped) {
                (void)waitpid(child, &status, 0);
                child_reaped = 1;
            }
            master_read_open = 0;
            output.offset = output.length = 0;
            continue;
        }

        descriptor_count = 0;
        if (input_open && relay_space(&input) > 0) {
            if (descriptors[descriptor_count].revents &
                (POLLIN | POLLHUP | POLLERR))
                relay_read_input(STDIN_FILENO, &input, &input_open);
            descriptor_count++;
        }
        if (master_read_open || (master_write_open && relay_pending(&input))) {
            short events = descriptors[descriptor_count].revents;

            if (master_write_open && relay_pending(&input) &&
                (events & (POLLOUT | POLLERR | POLLHUP)))
                relay_write_master(master, &input, &master_write_open);
            if (master_read_open && output_open && relay_space(&output) > 0 &&
                (events & (POLLIN | POLLERR | POLLHUP)))
                relay_read_master(master, &output, &master_read_open);
            if (events & POLLNVAL) {
                master_read_open = 0;
                master_write_open = 0;
            }
            descriptor_count++;
        }
        if (output_open && relay_pending(&output)) {
            if (descriptors[descriptor_count].revents &
                (POLLOUT | POLLERR | POLLHUP))
                relay_write_output(STDOUT_FILENO, &output, &output_open);
            descriptor_count++;
        }
    }
    close(master);
    if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        sigset_t empty_mask;

        (void)signal(signal_number, SIG_DFL);
        sigemptyset(&empty_mask);
        (void)sigprocmask(SIG_SETMASK, &empty_mask, NULL);
        (void)raise(signal_number);
        return RETURN_FAIL;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return RETURN_FAIL;
}

int main(int argc, char **argv)
{
    const char *pty_marker;

    if (argc < 2) {
        fprintf(stderr, "LNX: command required\n");
        return RETURN_FAIL;
    }

    pty_marker = getenv(ACE_LNX_PTY_VARIABLE);
    if (pty_marker && strcmp(pty_marker, ACE_LNX_PTY_VALUE) == 0)
        return supervise_pty(argv[1], &argv[1]);
    if (exec_linux_program(argv[1], &argv[1]) == 0)
        return RETURN_OK;
    fprintf(stderr, "LNX: %s: %s\n", argv[1], strerror(errno));
    return RETURN_FAIL;
}
