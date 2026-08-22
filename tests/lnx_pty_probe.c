#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
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
    fflush(stdout);
    return 0;
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
