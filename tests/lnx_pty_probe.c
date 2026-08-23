#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int write_all(const unsigned char *bytes, size_t length)
{
    while (length) {
        ssize_t amount = write(STDOUT_FILENO, bytes, length);

        if (amount > 0) {
            bytes += amount;
            length -= (size_t)amount;
        } else if (amount < 0 && errno == EINTR) {
            continue;
        } else {
            return 1;
        }
    }
    return 0;
}

static int report_terminal(void)
{
    struct winsize size = {0};
    pid_t session = getsid(0);
    pid_t process_group = getpgrp();
    pid_t terminal_session = tcgetsid(STDIN_FILENO);
    pid_t terminal_process_group = tcgetpgrp(STDIN_FILENO);
    const char *term = getenv("TERM");
    const char *marker = getenv("ACE_LNX_PTY");
    const char *color_term = getenv("COLORTERM");

    (void)ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
    printf("isatty %d %d %d\n",
           isatty(STDIN_FILENO), isatty(STDOUT_FILENO),
           isatty(STDERR_FILENO));
    printf("term %s\n", term ? term : "(unset)");
    printf("identity pid %ld sid %ld pgrp %ld tcgetsid %ld tcgetpgrp %ld\n",
           (long)getpid(), (long)session, (long)process_group,
           (long)terminal_session, (long)terminal_process_group);
    printf("winsize %u %u\n", (unsigned)size.ws_row, (unsigned)size.ws_col);
    printf("pty-marker %s\n", marker ? marker : "(unset)");
    printf("colorterm %s\n", color_term ? color_term : "(unset)");
    fflush(stdout);
    return 0;
}

static int read_exact(unsigned char *bytes, size_t length)
{
    while (length != 0) {
        ssize_t amount = read(STDIN_FILENO, bytes, length);

        if (amount > 0) {
            bytes += amount;
            length -= (size_t)amount;
        } else if (amount < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static int report_raw_bytes(unsigned long requested, int announce,
                            unsigned int delay_milliseconds)
{
    struct termios old_attributes;
    struct termios attributes;
    unsigned char *bytes;
    size_t index;

    if (requested > 131072 || tcgetattr(STDIN_FILENO, &old_attributes) != 0)
        return 2;
    attributes = old_attributes;
    attributes.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    attributes.c_cc[VMIN] = 1;
    attributes.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) != 0)
        return 2;
    if (announce) {
        printf("raw-ready\n");
        fflush(stdout);
    }
    if (delay_milliseconds) {
        struct timespec delay = {
            .tv_sec = delay_milliseconds / 1000,
            .tv_nsec = (long)(delay_milliseconds % 1000) * 1000000L,
        };

        while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
            continue;
    }
    bytes = calloc(requested ? (size_t)requested : 1, 1);
    if (!bytes)
        return 2;
    if (read_exact(bytes, (size_t)requested) != 0) {
        free(bytes);
        return 1;
    }
    printf("bytes");
    for (index = 0; index < requested; index++)
        printf(" %02x", bytes[index]);
    printf("\n");
    fflush(stdout);
    free(bytes);
    return 0;
}

static volatile sig_atomic_t resized;

static void resize_handler(int signal_number)
{
    (void)signal_number;
    resized = 1;
}

static int await_resize(void)
{
    struct sigaction action;
    struct winsize size = {0};

    memset(&action, 0, sizeof(action));
    action.sa_handler = resize_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGWINCH, &action, NULL) != 0)
        return 2;
    (void)ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
    printf("resize-ready %u %u\n", (unsigned)size.ws_row,
           (unsigned)size.ws_col);
    fflush(stdout);
    while (!resized)
        pause();
    (void)ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
    printf("resize %u %u\n", (unsigned)size.ws_row,
           (unsigned)size.ws_col);
    fflush(stdout);
    return 0;
}

static int terminal_output(void)
{
    static const unsigned char bytes[] =
        "plain\033[2J\033[Hcursor\033[31mred\033[0m"
        "\033[38;5;196mindexed\033[48;5;23mbackground\033[0m"
        "\033[?1049halt\033[?1049lrest"
        "Bot\033[1;28r\"filename\" 123L, 456B"
        "\033(Bpi@frambo\033(B:~ $\033(B"
        "\033]0;pi@frambo:~\007"
        "\033]0;another-title\033\\"
        "prompt\n";

    for (size_t index = 0; index < sizeof(bytes) - 1; index++) {
        struct timespec pause_time = {0, 1000000};

        if (write_all(bytes + index, 1) != 0)
            return 1;
        while (nanosleep(&pause_time, &pause_time) != 0 && errno == EINTR)
            continue;
    }
    return 0;
}

static int terminal_output_incomplete(void)
{
    static const unsigned char bytes[] = "incomplete\033[";

    return write_all(bytes, sizeof(bytes) - 1);
}

static int c1_output(void)
{
    static const unsigned char bytes[] = "target\23399~output\n";

    return write_all(bytes, sizeof(bytes) - 1);
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "report") == 0)
        return report_terminal();
    if (strcmp(argv[1], "line") == 0) {
        char line[4096];

        if (!fgets(line, sizeof(line), stdin))
            return 1;
        printf("received %s", line);
        fflush(stdout);
        return 0;
    }
    if (strcmp(argv[1], "bytes") == 0) {
        char *end;
        unsigned long requested;

        if (argc != 3)
            return 2;
        requested = strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end)
            return 2;
        return report_raw_bytes(requested, 0, 0);
    }
    if (strcmp(argv[1], "bytes-ready") == 0) {
        char *end;
        unsigned long requested;

        if (argc != 3)
            return 2;
        requested = strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end)
            return 2;
        return report_raw_bytes(requested, 1, 0);
    }
    if (strcmp(argv[1], "delay-bytes") == 0) {
        char *end;
        unsigned long requested;

        if (argc != 3)
            return 2;
        requested = strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end)
            return 2;
        return report_raw_bytes(requested, 1, 500);
    }
    if (strcmp(argv[1], "resize") == 0)
        return await_resize();
    if (strcmp(argv[1], "terminal-output") == 0)
        return terminal_output();
    if (strcmp(argv[1], "terminal-output-incomplete") == 0)
        return terminal_output_incomplete();
    if (strcmp(argv[1], "c1-output") == 0)
        return c1_output();
    if (strcmp(argv[1], "emit") == 0) {
        unsigned char buffer[8192];
        char *end;
        unsigned long requested;
        unsigned long written_total = 0;
        size_t index;

        if (argc != 3)
            return 2;
        requested = strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end || requested > 1024UL * 1024UL)
            return 2;
        while (requested) {
            size_t amount = requested < sizeof(buffer) ?
                            (size_t)requested : sizeof(buffer);

            for (index = 0; index < amount; index++)
                buffer[index] = (unsigned char)('A' +
                                                (written_total + index) % 26);
            if (write_all(buffer, amount) != 0)
                return 1;
            requested -= amount;
            written_total += amount;
        }
        return 0;
    }
    if (strcmp(argv[1], "exit") == 0) {
        char *end;
        long status;

        if (argc != 3)
            return 2;
        status = strtol(argv[2], &end, 10);
        if (!*argv[2] || *end || status < 0 || status > 255)
            return 2;
        return (int)status;
    }
    if (strcmp(argv[1], "signal") == 0) {
        char *end;
        long signal_number;

        if (argc != 3)
            return 2;
        signal_number = strtol(argv[2], &end, 10);
        if (!*argv[2] || *end || signal_number <= 0 ||
            signal_number > SIGRTMAX)
            return 2;
        raise((int)signal_number);
        return 1;
    }
    return 2;
}
