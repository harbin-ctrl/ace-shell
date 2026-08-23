#define _POSIX_C_SOURCE 200809L

#include "broker_client.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static void ignore_signal(int signal_number)
{
    (void)signal_number;
}

int main(void)
{
    struct sigaction action = {0};

    action.sa_handler = ignore_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGUSR1, &action, NULL) != 0 ||
        native_broker_task_set_foreground_pid(getpid()) != 0)
        return 1;
    puts("foreground-spoof-ready");
    fflush(stdout);
    for (;;)
        pause();
}
