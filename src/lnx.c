#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
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
#include <time.h>
#include <unistd.h>

#include <dos/dos.h>

#include "lnx_pty.h"

#define RELAY_BUFFER_SIZE 65536
#define ACE_INPUT_SEQUENCE_MAX 64
#define ACE_OUTPUT_EXPANSION_MAX 8
#define ACE_GEOMETRY_FALLBACK_ROWS 24
#define ACE_GEOMETRY_FALLBACK_COLS 80

#define LNX_EVENT_CTRL_C 0x03
#define LNX_EVENT_CTRL_D 0x04
#define LNX_EVENT_CTRL_E 0x05
#define LNX_EVENT_CTRL_F 0x06
#define LNX_EVENT_SHUTDOWN_HUP 0x80
#define LNX_EVENT_SHUTDOWN_TERM 0x81

static const unsigned char ace_bounds_query[] = "\2330 q";
static const unsigned char ace_resize_enable[] = "\23312{";
static const unsigned char ace_resize_disable[] = "\23312}";

static int lnx_signal_pipe[2] = {-1, -1};

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

struct ace_input_parser {
    unsigned char sequence[ACE_INPUT_SEQUENCE_MAX];
    size_t sequence_length;
    int utf8_continuations;
    int resize_pending;
};

struct terminal_output_parser {
    unsigned char sequence[ACE_INPUT_SEQUENCE_MAX];
    size_t sequence_length;
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

static int relay_append(struct relay_buffer *buffer, const void *bytes,
                        size_t length)
{
    size_t space = relay_space(buffer);

    if (length > space)
        return -1;
    memcpy(buffer->bytes + buffer->length, bytes, length);
    buffer->length += length;
    return 0;
}

static int set_nonblocking(int descriptor)
{
    int flags = fcntl(descriptor, F_GETFL);

    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static void lnx_signal_handler(int signal_number)
{
    unsigned char event = signal_number == SIGUSR1 ? LNX_EVENT_CTRL_C :
                          signal_number == SIGUSR2 ? LNX_EVENT_CTRL_D :
                          signal_number == SIGRTMIN ? LNX_EVENT_CTRL_E :
                          signal_number == SIGRTMIN + 1 ? LNX_EVENT_CTRL_F :
                          signal_number == SIGHUP ? LNX_EVENT_SHUTDOWN_HUP :
                          signal_number == SIGTERM ? LNX_EVENT_SHUTDOWN_TERM :
                          0;

    if (event && lnx_signal_pipe[1] >= 0)
        (void)write(lnx_signal_pipe[1], &event, sizeof(event));
}

static int install_lnx_signal_handlers(void)
{
    struct sigaction action;
    const int signal_numbers[] = {
        SIGUSR1, SIGUSR2, SIGRTMIN, SIGRTMIN + 1, SIGHUP, SIGTERM,
    };
    size_t index;

    if (pipe2(lnx_signal_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
        return -1;
    memset(&action, 0, sizeof(action));
    action.sa_handler = lnx_signal_handler;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR1);
    sigaddset(&action.sa_mask, SIGUSR2);
    sigaddset(&action.sa_mask, SIGRTMIN);
    sigaddset(&action.sa_mask, SIGRTMIN + 1);
    sigaddset(&action.sa_mask, SIGHUP);
    sigaddset(&action.sa_mask, SIGTERM);
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) < 0) {
        int error = errno;

        close(lnx_signal_pipe[0]);
        close(lnx_signal_pipe[1]);
        lnx_signal_pipe[0] = lnx_signal_pipe[1] = -1;
        errno = error;
        return -1;
    }
    action.sa_handler = lnx_signal_handler;
    for (index = 0; index < sizeof(signal_numbers) / sizeof(signal_numbers[0]);
         index++) {
        if (sigaction(signal_numbers[index], &action, NULL) < 0) {
            int error = errno;

            close(lnx_signal_pipe[0]);
            close(lnx_signal_pipe[1]);
            lnx_signal_pipe[0] = lnx_signal_pipe[1] = -1;
            errno = error;
            return -1;
        }
    }
    return 0;
}

static void restore_lnx_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGUSR1, &action, NULL);
    (void)sigaction(SIGUSR2, &action, NULL);
    (void)sigaction(SIGRTMIN, &action, NULL);
    (void)sigaction(SIGRTMIN + 1, &action, NULL);
    (void)sigaction(SIGHUP, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGPIPE, &action, NULL);
}

static void close_lnx_signal_pipe(void)
{
    int read_descriptor = lnx_signal_pipe[0];
    int write_descriptor = lnx_signal_pipe[1];

    lnx_signal_pipe[0] = lnx_signal_pipe[1] = -1;
    if (read_descriptor >= 0)
        close(read_descriptor);
    if (write_descriptor >= 0)
        close(write_descriptor);
}

static int write_control_sequence(const unsigned char *bytes, size_t length)
{
    while (length) {
        ssize_t written = write(STDOUT_FILENO, bytes, length);

        if (written > 0) {
            bytes += written;
            length -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd output = {
                .fd = STDOUT_FILENO,
                .events = POLLOUT,
            };

            if (poll(&output, 1, 200) > 0)
                continue;
        }
        return -1;
    }
    return 0;
}

static int parse_decimal(const unsigned char *bytes, size_t length,
                         size_t *position, unsigned long *value)
{
    unsigned long result = 0;
    size_t index = *position;

    if (index == length || bytes[index] < '0' || bytes[index] > '9')
        return -1;
    while (index < length && bytes[index] >= '0' && bytes[index] <= '9') {
        if (result > (ULONG_MAX - (unsigned long)(bytes[index] - '0')) / 10)
            return -1;
        result = result * 10 + (unsigned long)(bytes[index] - '0');
        index++;
    }
    *position = index;
    *value = result;
    return 0;
}

static int find_bounds_reply(const unsigned char *bytes, size_t length,
                             size_t *start_out, size_t *end_out,
                             struct winsize *size)
{
    size_t start;

    for (start = 0; start < length; start++) {
        size_t position;
        unsigned long rows;
        unsigned long cols;

        if (bytes[start] == 0x9b)
            position = start + 1;
        else if (bytes[start] == '\033' && start + 1 < length &&
                 bytes[start + 1] == '[')
            position = start + 2;
        else
            continue;
        if (position + 4 > length || bytes[position] != '1' ||
            bytes[position + 1] != ';' || bytes[position + 2] != '1' ||
            bytes[position + 3] != ';')
            continue;
        position += 4;
        if (parse_decimal(bytes, length, &position, &rows) != 0 ||
            position == length || bytes[position++] != ';' ||
            parse_decimal(bytes, length, &position, &cols) != 0)
            continue;
        /* The established ACE console reply spells this as "COLS r".  Be
           tolerant of the no-space CSI spelling too, since the reply is a
           public byte protocol rather than a host-terminal dependency. */
        if (position < length && bytes[position] == ' ')
            position++;
        if (position == length || bytes[position++] != 'r' ||
            rows == 0 || cols == 0 || rows > USHRT_MAX || cols > USHRT_MAX)
            continue;
        size->ws_row = (unsigned short)rows;
        size->ws_col = (unsigned short)cols;
        *start_out = start;
        *end_out = position;
        return 0;
    }
    return -1;
}

/* Query the public console bounds protocol.  The complete bounds reply is
 * consumed here because it is supervisor metadata, not target typeahead;
 * anything else is returned to the streaming keyboard parser below. */
static void query_console_geometry(struct relay_buffer *typeahead,
                                   struct winsize *size)
{
    unsigned char received[4096];
    size_t received_length = 0;
    size_t reply_start;
    size_t reply_end;
    int attempt;

    memset(size, 0, sizeof(*size));
    size->ws_row = ACE_GEOMETRY_FALLBACK_ROWS;
    size->ws_col = ACE_GEOMETRY_FALLBACK_COLS;
    if (write_control_sequence(ace_bounds_query, sizeof(ace_bounds_query) - 1)
        != 0)
        return;
    for (attempt = 0; attempt < 3 && received_length < sizeof(received);
         attempt++) {
        struct pollfd input = {
            .fd = STDIN_FILENO,
            .events = POLLIN | POLLHUP,
        };
        int polled = poll(&input, 1, 100);

        if (polled <= 0 || !(input.revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        while (received_length < sizeof(received)) {
            ssize_t amount = read(STDIN_FILENO, received + received_length,
                                  sizeof(received) - received_length);

            if (amount > 0) {
                received_length += (size_t)amount;
                continue;
            }
            if (amount < 0 && errno == EINTR)
                continue;
            break;
        }
        if (find_bounds_reply(received, received_length, &reply_start,
                              &reply_end, size) == 0) {
            (void)relay_append(typeahead, received, reply_start);
            (void)relay_append(typeahead, received + reply_end,
                               received_length - reply_end);
            return;
        }
    }
    (void)relay_append(typeahead, received, received_length);
}

static int ace_sequence_final(unsigned char byte)
{
    return byte >= 0x40 && byte <= 0x7e;
}

static int utf8_continuations(unsigned char byte)
{
    if (byte >= 0xc2 && byte <= 0xdf)
        return 1;
    if (byte >= 0xe0 && byte <= 0xef)
        return 2;
    if (byte >= 0xf0 && byte <= 0xf4)
        return 3;
    return 0;
}

static int ace_input_emit_sequence(struct ace_input_parser *parser,
                                   struct relay_buffer *output)
{
    static const struct {
        const char *ace;
        const char *xterm;
    } translations[] = {
        { "\233A", "\033[A" }, { "\233B", "\033[B" },
        { "\233C", "\033[C" }, { "\233D", "\033[D" },
        { "\233T", "\033[1;2A" }, { "\233S", "\033[1;2B" },
        { "\233 A", "\033[1;2D" }, { "\233 @", "\033[1;2C" },
        { "\233Z", "\033[Z" }, { "\23340~", "\033[2~" },
        { "\23341~", "\033[5~" }, { "\23342~", "\033[6~" },
        { "\23344~", "\033OH" }, { "\23345~", "\033OF" },
        { "\23350~", "\033[2;2~" }, { "\23354~", "\033[1;2H" },
        { "\23355~", "\033[1;2F" }, { "\2330~", "\033OP" },
        { "\2331~", "\033OQ" }, { "\2332~", "\033OR" },
        { "\2333~", "\033OS" }, { "\2334~", "\033[15~" },
        { "\2335~", "\033[17~" }, { "\2336~", "\033[18~" },
        { "\2337~", "\033[19~" }, { "\2338~", "\033[20~" },
        { "\2339~", "\033[21~" }, { "\23310~", "\033[1;2P" },
        { "\23311~", "\033[1;2Q" }, { "\23312~", "\033[1;2R" },
        { "\23313~", "\033[1;2S" }, { "\23314~", "\033[15;2~" },
        { "\23315~", "\033[17;2~" }, { "\23316~", "\033[18;2~" },
        { "\23317~", "\033[19;2~" }, { "\23318~", "\033[20;2~" },
        { "\23319~", "\033[21;2~" },
    };
    size_t index;
    int resize_report = 0;

    if (parser->sequence_length >= 5 && parser->sequence[0] == 0x9b &&
        memcmp(parser->sequence + 1, "12;", 3) == 0 &&
        parser->sequence[parser->sequence_length - 1] == '|') {
        resize_report = parser->sequence[4] >= '0' &&
                        parser->sequence[4] <= '9';
        for (index = 4; resize_report &&
             index + 1 < parser->sequence_length; index++) {
            unsigned char byte = parser->sequence[index];

            if ((byte < '0' || byte > '9') && byte != ';')
                resize_report = 0;
        }
    }
    if (resize_report) {
        parser->resize_pending = 1;
        parser->sequence_length = 0;
        return 0;
    }
    for (index = 0; index < sizeof(translations) / sizeof(translations[0]);
         index++) {
        size_t source_length = strlen(translations[index].ace);

        if (source_length == parser->sequence_length &&
            memcmp(parser->sequence, translations[index].ace, source_length) == 0) {
            parser->sequence_length = 0;
            return relay_append(output, translations[index].xterm,
                                strlen(translations[index].xterm));
        }
    }
    if (relay_append(output, parser->sequence, parser->sequence_length) != 0)
        return -1;
    parser->sequence_length = 0;
    return 0;
}

static int ace_input_feed(struct ace_input_parser *parser,
                          struct relay_buffer *output,
                          const unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        unsigned char byte = bytes[index];

        if (parser->utf8_continuations) {
            /* U+009B is encoded as C2 9B in UTF-8.  Forwarding the
             * continuation byte here prevents it from being mistaken for
             * a standalone ACE CSI introducer. */
            if (relay_append(output, &byte, 1) != 0)
                return -1;
            if (byte >= 0x80 && byte <= 0xbf)
                parser->utf8_continuations--;
            else
                parser->utf8_continuations = utf8_continuations(byte);
            continue;
        }
        if (parser->sequence_length) {
            parser->sequence[parser->sequence_length++] = byte;
            if (parser->sequence_length == sizeof(parser->sequence) ||
                ace_sequence_final(byte)) {
                if (ace_input_emit_sequence(parser, output) != 0)
                    return -1;
            }
            continue;
        }
        if (byte == 0x9b) {
            parser->sequence[0] = byte;
            parser->sequence_length = 1;
            continue;
        }
        parser->utf8_continuations = utf8_continuations(byte);
        if (byte == '\b')
            byte = 0x7f;
        else if (byte == 0x7f) {
            static const unsigned char delete_key[] = "\033[3~";

            if (relay_append(output, delete_key, sizeof(delete_key) - 1) != 0)
                return -1;
            continue;
        }
        if (relay_append(output, &byte, 1) != 0)
            return -1;
    }
    return 0;
}

static int ace_input_flush(struct ace_input_parser *parser,
                           struct relay_buffer *output)
{
    if (!parser->sequence_length)
        return 0;
    if (relay_append(output, parser->sequence, parser->sequence_length) != 0)
        return -1;
    parser->sequence_length = 0;
    return 0;
}

static int terminal_output_emit_sequence(struct terminal_output_parser *parser,
                                         struct relay_buffer *output)
{
    static const unsigned char clear_screen[] = "\2331;1H\233J";
    static const unsigned char home_cursor[] = "\2331;1H";
    const unsigned char *sequence = parser->sequence;
    size_t length = parser->sequence_length;
    int alternate_screen = 0;

    /* The ACE/AROS console recognizes its C1 cursor and erase commands, but
       its historical parser does not implement xterm's CSI 2 J form.  Home
       followed by the native erase-display command is the documented ACE
       full-window clear. */
    if (length == 4 && memcmp(sequence, "\033[2J", 4) == 0) {
        parser->sequence_length = 0;
        return relay_append(output, clear_screen, sizeof(clear_screen) - 1);
    }
    /* xterm's parameterless CUP/HVP means absolute home. ACE fills omitted
       CUP parameters with the current position instead, so make home
       explicit before handing it to the native parser. */
    if (length == 3 && sequence[0] == '\033' && sequence[1] == '[' &&
        (sequence[2] == 'H' || sequence[2] == 'f')) {
        parser->sequence_length = 0;
        return relay_append(output, home_cursor, sizeof(home_cursor) - 1);
    }
    if (length >= 5 && sequence[0] == '\033' && sequence[1] == '[' &&
        sequence[2] == '?' &&
        (sequence[length - 1] == 'h' || sequence[length - 1] == 'l')) {
        const unsigned char *parameter = sequence + 3;
        size_t parameter_length = length - 4;

        alternate_screen = (parameter_length == 2 &&
                            (memcmp(parameter, "47", 2) == 0 ||
                             memcmp(parameter, "49", 2) == 0)) ||
                           (parameter_length == 4 &&
                            memcmp(parameter, "1047", 4) == 0) ||
                           (parameter_length == 4 &&
                            memcmp(parameter, "1049", 4) == 0);
        /* Cursor visibility and private cursor-key mode are state the ACE
           console cannot render; omitting them is deterministic and leaves
           no escape text in scrollback. */
        if (alternate_screen) {
            parser->sequence_length = 0;
            return relay_append(output, clear_screen,
                                sizeof(clear_screen) - 1);
        }
        parser->sequence_length = 0;
        return 0;
    }
    /* The imported AROS stdconclass parser has no SGR dispatcher.  Keep the
       selected TERM truthful about cursor/input behavior, but reduce ANSI
       8/16/indexed color attributes to the ACE default text pen rather than
       passing unsupported SGR bytes through as visible garbage. */
    if (length >= 3 && sequence[0] == '\033' && sequence[1] == '[' &&
        sequence[length - 1] == 'm') {
        parser->sequence_length = 0;
        return 0;
    }
    parser->sequence_length = 0;
    return relay_append(output, sequence, length);
}

static int terminal_output_feed(struct terminal_output_parser *parser,
                                struct relay_buffer *output,
                                const unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        unsigned char byte = bytes[index];

        if (!parser->sequence_length) {
            if (byte == '\033') {
                parser->sequence[0] = byte;
                parser->sequence_length = 1;
            } else if (relay_append(output, &byte, 1) != 0) {
                return -1;
            }
            continue;
        }
        parser->sequence[parser->sequence_length++] = byte;
        if (parser->sequence_length == sizeof(parser->sequence) ||
            (parser->sequence_length == 2 && byte != '[') ||
            (parser->sequence_length > 2 && ace_sequence_final(byte))) {
            if (terminal_output_emit_sequence(parser, output) != 0)
                return -1;
        }
    }
    return 0;
}

static int terminal_output_flush(struct terminal_output_parser *parser,
                                 struct relay_buffer *output)
{
    if (!parser->sequence_length)
        return 0;
    if (relay_append(output, parser->sequence, parser->sequence_length) != 0)
        return -1;
    parser->sequence_length = 0;
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
    (void)unsetenv(ACE_LNX_TARGET_TERM_VARIABLE);
    (void)unsetenv("COLORTERM");
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

static long long monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int reap_child_for(pid_t child, int *status, int *reaped,
                          int timeout_milliseconds)
{
    long long deadline = monotonic_milliseconds() + timeout_milliseconds;

    while (!*reaped) {
        pid_t waited = waitpid(child, status, WNOHANG);

        if (waited == child) {
            *reaped = 1;
            return 0;
        }
        if (waited < 0 && errno != EINTR) {
            *reaped = 1;
            *status = RETURN_FAIL;
            return -1;
        }
        if (monotonic_milliseconds() >= deadline)
            return 1;
        (void)poll(NULL, 0, 10);
    }
    return 0;
}

static void signal_pty_session(pid_t session, int signal_number)
{
    DIR *proc = opendir("/proc");
    struct dirent *entry;

    if (!proc)
        return;
    while ((entry = readdir(proc)) != NULL) {
        char path[64];
        char line[512];
        char state;
        long parent;
        long process_group;
        long process_session;
        char *end;
        long process;
        FILE *stat_file;

        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        process = strtol(entry->d_name, &end, 10);
        if (*end || process <= 0 || process == (long)getpid())
            continue;
        if (snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name) >=
            (int)sizeof(path))
            continue;
        stat_file = fopen(path, "r");
        if (!stat_file)
            continue;
        if (!fgets(line, sizeof(line), stat_file)) {
            fclose(stat_file);
            continue;
        }
        fclose(stat_file);
        end = strrchr(line, ')');
        if (!end || sscanf(end + 2, " %c %ld %ld %ld", &state, &parent,
                           &process_group, &process_session) != 4)
            continue;
        if ((pid_t)process_session == session)
            (void)kill((pid_t)process, signal_number);
    }
    closedir(proc);
}

static void signal_pty_target(int master, pid_t child, pid_t session,
                              int signal_number)
{
    pid_t foreground_group = master >= 0 ? tcgetpgrp(master) : -1;

    if (foreground_group > 0 && foreground_group != getpgrp())
        (void)kill(-foreground_group, signal_number);
    if (child > 0)
        (void)kill(child, signal_number);
    if (session > 0)
        signal_pty_session(session, signal_number);
}

static void shutdown_pty_target(int *master, pid_t child, pid_t session,
                                int *status, int *child_reaped)
{
    if (*child_reaped) {
        signal_pty_target(-1, child, session, SIGKILL);
        if (*master >= 0) {
            close(*master);
            *master = -1;
        }
        return;
    }
    /* Query the current PTY foreground group before closing the master.  The
       initial child may be Bash while the actual foreground job is a later
       process group created by Bash. */
    signal_pty_target(*master, child, session, SIGHUP);
    if (*master >= 0) {
        close(*master);
        *master = -1;
    }
    if (reap_child_for(child, status, child_reaped, 300) != 0) {
        signal_pty_target(-1, child, session, SIGTERM);
        if (reap_child_for(child, status, child_reaped, 300) != 0) {
            signal_pty_target(-1, child, session, SIGKILL);
            if (reap_child_for(child, status, child_reaped, 1000) != 0) {
                /* SIGKILL cannot be caught.  The blocking wait is only a
                   final kernel-level safeguard against returning with a
                   direct-child zombie. */
                while (waitpid(child, status, 0) < 0 && errno == EINTR)
                    ;
                *child_reaped = 1;
            }
        }
    }
}

static void relay_read_input(int descriptor, struct ace_input_parser *parser,
                             struct relay_buffer *buffer, int *input_open,
                             int *console_hup)
{
    unsigned char bytes[4096];
    ssize_t amount;
    size_t space = relay_space(buffer) / 8;

    if (!space)
        return;
    if (space > sizeof(bytes))
        space = sizeof(bytes);
    amount = read(descriptor, bytes, space);
    if (amount > 0) {
        if (ace_input_feed(parser, buffer, bytes, (size_t)amount) != 0)
            *input_open = 0;
    }
    else if (amount == 0 || (errno != EINTR && errno != EAGAIN &&
                             errno != EWOULDBLOCK)) {
        *input_open = 0;
        *console_hup = 1;
    }
}

static void relay_read_signal_events(struct relay_buffer *input,
                                     int *shutdown_requested)
{
    unsigned char event;

    while (relay_space(input) > 0) {
        ssize_t amount = read(lnx_signal_pipe[0], &event, sizeof(event));

        if (amount == 1) {
            if (event == LNX_EVENT_SHUTDOWN_HUP ||
                event == LNX_EVENT_SHUTDOWN_TERM) {
                *shutdown_requested = 1;
                continue;
            }
            if (event == LNX_EVENT_CTRL_C || event == LNX_EVENT_CTRL_D ||
                event == LNX_EVENT_CTRL_E || event == LNX_EVENT_CTRL_F)
                (void)relay_append(input, &event, sizeof(event));
            continue;
        }
        if (amount < 0 && errno == EINTR)
            continue;
        if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        return;
    }
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

/* Return nonzero only when the PTY master has reached end-of-stream.  A
 * transient EAGAIN after the child exits is not EOF: unread PTY output can
 * still become available on the next poll. */
static int relay_read_master(int master, struct terminal_output_parser *parser,
                             struct relay_buffer *buffer, int *master_read_open)
{
    unsigned char bytes[4096];
    ssize_t amount;
    size_t space = relay_space(buffer);

    /* CSI 2 J becomes the longer native home-and-erase sequence.  Leave
       enough room even when an escape sequence was split across reads. */
    if (space < ACE_OUTPUT_EXPANSION_MAX)
        return 0;
    space /= 2;
    if (space > sizeof(bytes))
        space = sizeof(bytes);
    amount = read(master, bytes, space);
    if (amount > 0) {
        if (terminal_output_feed(parser, buffer, bytes, (size_t)amount) != 0)
            *master_read_open = 0;
        return 0;
    }
    if (amount == 0 || (amount < 0 && (errno == EIO || errno == EBADF))) {
        *master_read_open = 0;
        return 1;
    }
    if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (amount < 0 && errno != EINTR) {
        *master_read_open = 0;
        return 1;
    }
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
    struct relay_buffer typeahead = {0};
    struct ace_input_parser input_parser = {0};
    struct terminal_output_parser output_parser = {0};
    struct winsize geometry;
    struct pollfd descriptors[4];
    int descriptor_count;
    int input_index;
    int signal_index;
    int master_index;
    int output_index;
    int input_open = 1;
    int output_open = 1;
    int master_read_open = 1;
    int master_write_open = 1;
    int input_flushed = 0;
    int console_hup = 0;
    int resize_events_enabled = 0;
    int shutdown_requested = 0;
    long long console_hup_deadline = 0;
    long long master_eof_deadline = 0;
    int master;
    int slave;
    int status = RETURN_FAIL;
    int child_reaped = 0;
    pid_t child;
    pid_t session;
    static const unsigned char terminal_eof = LNX_EVENT_CTRL_D;

    if (open_pty_pair(&master, &slave) < 0) {
        fprintf(stderr, "LNX: cannot allocate PTY: %s\n", strerror(errno));
        return RETURN_FAIL;
    }
    if (initialize_pty_termios(slave) < 0 || set_nonblocking(master) < 0 ||
        set_nonblocking(STDIN_FILENO) < 0 || set_nonblocking(STDOUT_FILENO) < 0) {
        int error = errno;

        close(slave);
        close(master);
        fprintf(stderr, "LNX: cannot initialize PTY: %s\n", strerror(error));
        return RETURN_FAIL;
    }
    if (install_lnx_signal_handlers() < 0) {
        int error = errno;

        close(slave);
        close(master);
        fprintf(stderr, "LNX: cannot install PTY signal bridge: %s\n",
                strerror(error));
        return RETURN_FAIL;
    }
    query_console_geometry(&typeahead, &geometry);
    if (ioctl(slave, TIOCSWINSZ, &geometry) < 0) {
        int error = errno;

        close(slave);
        close(master);
        close_lnx_signal_pipe();
        fprintf(stderr, "LNX: cannot set PTY geometry: %s\n", strerror(error));
        return RETURN_FAIL;
    }
    if (ace_input_feed(&input_parser, &input, typeahead.bytes,
                       relay_pending(&typeahead)) != 0) {
        close(slave);
        close(master);
        close_lnx_signal_pipe();
        fprintf(stderr, "LNX: console typeahead exceeds relay buffer\n");
        return RETURN_FAIL;
    }
    if (write_control_sequence(ace_resize_enable,
                               sizeof(ace_resize_enable) - 1) == 0)
        resize_events_enabled = 1;
    child = fork();
    if (child < 0) {
        int error = errno;

        close(slave);
        close(master);
        close_lnx_signal_pipe();
        fprintf(stderr, "LNX: cannot fork PTY target: %s\n", strerror(error));
        return RETURN_FAIL;
    }
    if (child == 0)
        child_exec(program, arguments, master, slave);
    close(slave);
    session = child;

    while (1) {
        pid_t waited;
        int master_at_eof = 0;

        if (!child_reaped) {
            waited = waitpid(child, &status, WNOHANG);
            if (waited == child)
                child_reaped = 1;
            else if (waited < 0 && errno != EINTR) {
                shutdown_requested = 1;
                status = RETURN_FAIL;
            }
        }
        if (child_reaped) {
            input_open = 0;
            input.offset = input.length = 0;
            master_write_open = 0;
            input_flushed = 1;
        }
        if (console_hup && !child_reaped &&
            monotonic_milliseconds() >= console_hup_deadline)
            shutdown_requested = 1;
        if (!child_reaped && !master_read_open) {
            if (!master_eof_deadline)
                master_eof_deadline = monotonic_milliseconds() + 250;
            else if (monotonic_milliseconds() >= master_eof_deadline)
                shutdown_requested = 1;
        }
        if (shutdown_requested) {
            shutdown_pty_target(&master, child, session, &status,
                                &child_reaped);
            input_open = 0;
            master_read_open = 0;
            master_write_open = 0;
            output.offset = output.length = 0;
            break;
        }
        if (!input_open && !input_flushed) {
            if (ace_input_flush(&input_parser, &input) == 0 &&
                (!master_write_open ||
                 relay_append(&input, &terminal_eof, sizeof(terminal_eof)) == 0))
                input_flushed = 1;
        }
        if (input_parser.resize_pending) {
            struct relay_buffer resize_typeahead = {0};

            input_parser.resize_pending = 0;
            query_console_geometry(&resize_typeahead, &geometry);
            (void)ioctl(master, TIOCSWINSZ, &geometry);
            (void)ace_input_feed(&input_parser, &input,
                                 resize_typeahead.bytes,
                                 relay_pending(&resize_typeahead));
        }
        if (!output_open) {
            output.offset = output.length = 0;
            master_read_open = 0;
            shutdown_requested = 1;
        }
        if (master_read_open && output_open &&
            relay_space(&output) > 0)
            master_at_eof = relay_read_master(master, &output_parser, &output,
                                              &master_read_open);
        if (!master_read_open)
            (void)terminal_output_flush(&output_parser, &output);
        if (child_reaped && master_at_eof)
            master_read_open = 0;
        if (child_reaped && !master_read_open && !relay_pending(&output))
            break;

        descriptor_count = 0;
        input_index = signal_index = master_index = output_index = -1;
        if (input_open && relay_space(&input) > 0) {
            input_index = descriptor_count;
            descriptors[descriptor_count].fd = STDIN_FILENO;
            descriptors[descriptor_count].events = POLLIN | POLLHUP;
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }
        if (!child_reaped && lnx_signal_pipe[0] >= 0 &&
            relay_space(&input) > 0) {
            signal_index = descriptor_count;
            descriptors[descriptor_count].fd = lnx_signal_pipe[0];
            descriptors[descriptor_count].events = POLLIN | POLLHUP;
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }
        if (master_read_open || (master_write_open && relay_pending(&input))) {
            master_index = descriptor_count;
            descriptors[descriptor_count].fd = master;
            descriptors[descriptor_count].events =
                (master_read_open ? POLLIN | POLLHUP : 0) |
                (master_write_open && relay_pending(&input) ? POLLOUT : 0);
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }
        if (output_open && relay_pending(&output)) {
            output_index = descriptor_count;
            descriptors[descriptor_count].fd = STDOUT_FILENO;
            descriptors[descriptor_count].events = POLLOUT;
            descriptors[descriptor_count].revents = 0;
            descriptor_count++;
        }
        if (!descriptor_count) {
            shutdown_requested = !child_reaped;
            if (!shutdown_requested)
                break;
            continue;
        }
        if (poll(descriptors, (nfds_t)descriptor_count, 100) < 0) {
            if (errno == EINTR)
                continue;
            shutdown_requested = 1;
            continue;
        }

        if (input_index >= 0 &&
            descriptors[input_index].revents &
                (POLLIN | POLLHUP | POLLERR))
                relay_read_input(STDIN_FILENO, &input_parser, &input,
                                 &input_open, &console_hup);
        if (console_hup && !console_hup_deadline)
            console_hup_deadline = monotonic_milliseconds() + 250;
        if (signal_index >= 0 &&
            descriptors[signal_index].revents &
                (POLLIN | POLLHUP | POLLERR))
            relay_read_signal_events(&input, &shutdown_requested);
        if (master_index >= 0) {
            short events = descriptors[master_index].revents;

            if (master_write_open && relay_pending(&input) &&
                (events & (POLLOUT | POLLERR | POLLHUP)))
                relay_write_master(master, &input, &master_write_open);
            if (master_read_open && output_open && relay_space(&output) > 0 &&
                (events & (POLLIN | POLLERR | POLLHUP)))
                relay_read_master(master, &output_parser, &output,
                                  &master_read_open);
            if (events & POLLNVAL) {
                master_read_open = 0;
                master_write_open = 0;
            }
            /* A PTY can report EIO after the target has closed its slave but
             * before waitpid observes its exit.  Keep draining output during
             * that short race; a write-side failure still means the target
             * cannot be supervised and is shut down immediately. */
            if (!master_write_open && !child_reaped)
                shutdown_requested = 1;
        }
        if (output_index >= 0 &&
            descriptors[output_index].revents &
                (POLLOUT | POLLERR | POLLHUP))
                relay_write_output(STDOUT_FILENO, &output, &output_open);
    }
    if (!child_reaped)
        shutdown_pty_target(&master, child, session, &status, &child_reaped);
    if (resize_events_enabled)
        (void)write_control_sequence(ace_resize_disable,
                                     sizeof(ace_resize_disable) - 1);
    if (master >= 0)
        close(master);
    restore_lnx_signal_handlers();
    close_lnx_signal_pipe();
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
    const char *term_marker;

    if (argc < 2) {
        fprintf(stderr, "LNX: command required\n");
        return RETURN_FAIL;
    }

    pty_marker = getenv(ACE_LNX_PTY_VARIABLE);
    if (pty_marker && strcmp(pty_marker, ACE_LNX_PTY_VALUE) == 0)
        return supervise_pty(argv[1], &argv[1]);
    term_marker = getenv(ACE_LNX_TARGET_TERM_VARIABLE);
    if (term_marker && strcmp(term_marker, ACE_LNX_PTY_VALUE) == 0) {
        (void)unsetenv(ACE_LNX_PTY_VARIABLE);
        (void)unsetenv(ACE_LNX_TARGET_TERM_VARIABLE);
        (void)unsetenv("COLORTERM");
        if (setenv("TERM", ACE_LNX_PTY_TERM, 1) != 0) {
            fprintf(stderr, "LNX: cannot set TERM: %s\n", strerror(errno));
            return RETURN_FAIL;
        }
    }
    if (exec_linux_program(argv[1], &argv[1]) == 0)
        return RETURN_OK;
    fprintf(stderr, "LNX: %s: %s\n", argv[1], strerror(errno));
    return RETURN_FAIL;
}
