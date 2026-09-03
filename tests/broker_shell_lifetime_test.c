/*
 * A broker is as long-lived as its shells, plus an idle window.
 *
 * The window is what makes closing one window and opening another cheap: the
 * second shell finds the first one's broker, and the assigns, variables and
 * current directory it was holding. What the window is not is a licence to
 * stay: once it passes with nobody attached, the broker goes, instead of
 * accumulating one unreachable process per protocol change for the rest of
 * the login session.
 *
 * The real window is thirty minutes, which is not a thing to wait for here,
 * so this asks for a short one through ACE_BROKER_IDLE_SECONDS -- set before
 * the first request, because that request is what starts the broker that
 * inherits it.
 */

#define _POSIX_C_SOURCE 200809L

#include "broker_client.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Long enough that starting the next shell fits inside it on a loaded
   machine, short enough that waiting it out is not a test that hangs. */
#define IDLE_SECONDS 5

static int wait_for_pid(pid_t pid, int present, int seconds)
{
    struct timespec delay = {0, 10000000L};
    int tries = seconds * 100;

    for (int attempt = 0; attempt < tries; attempt++) {
        int alive = kill(pid, 0) == 0 || errno == EPERM;

        if (alive == present) {
            return 0;
        }
        nanosleep(&delay, NULL);
    }
    return -1;
}

static pid_t broker_pid(const char *socket_path)
{
    char lock_path[4096];
    FILE *file;
    long pid;

    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", socket_path) >=
        (int)sizeof(lock_path)) {
        return -1;
    }
    file = fopen(lock_path, "r");
    if (!file) {
        return -1;
    }
    if (fscanf(file, "%ld", &pid) != 1) {
        pid = -1;
    }
    fclose(file);
    return (pid_t)pid;
}

static pid_t start_shell(const char *session, int ready, int done)
{
    pid_t child = fork();
    char byte;

    if (child != 0) {
        return child;
    }
    if (setenv("ACE_SESSION", session, 1) != 0 ||
        native_broker_ensure() != 0 || native_broker_attach() != 0) {
        _exit(2);
    }
    if (write(ready, "R", 1) != 1 || read(done, &byte, 1) != 1) {
        _exit(3);
    }
    _exit(0);
}

/* Lets a shell go and waits for it, so that the broker has seen the close
   before anything is asserted about the broker. */
static int stop_shell(pid_t shell, int done)
{
    int status;

    if (write(done, "D", 1) != 1) {
        return -1;
    }
    if (waitpid(shell, &status, 0) != shell || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

int main(void)
{
    const char *socket_path = getenv("ACE_BROKER_SOCKET");
    int first_ready[2], first_done[2], second_ready[2], second_done[2];
    int third_ready[2], third_done[2];
    pid_t first, second, third, broker;
    char byte;

    if (!socket_path || pipe(first_ready) || pipe(first_done) ||
        pipe(second_ready) || pipe(second_done) || pipe(third_ready) ||
        pipe(third_done)) {
        return 1;
    }
    {
        char seconds[16];

        snprintf(seconds, sizeof(seconds), "%d", IDLE_SECONDS);
        if (setenv("ACE_BROKER_IDLE_SECONDS", seconds, 1) != 0) {
            return 1;
        }
    }

    first = start_shell("broker-shell-lifetime-first", first_ready[1],
                        first_done[0]);
    if (first < 0 || read(first_ready[0], &byte, 1) != 1) {
        return 1;
    }
    broker = broker_pid(socket_path);
    if (broker < 1 || wait_for_pid(broker, 1, 2) != 0) {
        return 1;
    }

    second = start_shell("broker-shell-lifetime-second", second_ready[1],
                         second_done[0]);
    if (second < 0 || read(second_ready[0], &byte, 1) != 1) {
        return 1;
    }
    /* One shell of two leaving decides nothing. */
    if (stop_shell(first, first_done[1]) != 0 ||
        wait_for_pid(broker, 1, 2) != 0) {
        return 1;
    }

    /* The last one leaving starts the window rather than ending the broker,
       and a shell that arrives inside the window gets that same broker. */
    if (stop_shell(second, second_done[1]) != 0) {
        return 1;
    }
    if (kill(broker, 0) != 0 && errno != EPERM) {
        fprintf(stderr, "broker exited the moment its last shell did\n");
        return 1;
    }
    third = start_shell("broker-shell-lifetime-third", third_ready[1],
                        third_done[0]);
    if (third < 0 || read(third_ready[0], &byte, 1) != 1) {
        return 1;
    }
    if (broker_pid(socket_path) != broker) {
        fprintf(stderr, "a shell inside the idle window did not reuse the "
                        "waiting broker\n");
        return 1;
    }

    /* And the window, once it does pass unattended, is the end of it. */
    if (stop_shell(third, third_done[1]) != 0) {
        return 1;
    }
    if (wait_for_pid(broker, 0, IDLE_SECONDS + 25) != 0) {
        fprintf(stderr, "broker stayed alive after its idle window passed "
                        "with no shell attached\n");
        return 1;
    }
    puts("broker shell lifetime test: ok");
    return 0;
}
