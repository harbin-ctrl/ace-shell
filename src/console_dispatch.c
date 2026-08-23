#include "console_dispatch.h"

#include <errno.h>
#include <signal.h>

#define ACE_CONSOLE_FRAME_MILLISECONDS 16

guint ace_console_dispatch_add_output_watch(int fd, GIOFunc callback,
                                             gpointer data)
{
    GIOChannel *channel = g_io_channel_unix_new(fd);
    guint source;

    if (!channel)
        return 0;
    source = g_io_add_watch_full(channel, G_PRIORITY_LOW,
                                 G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                 callback, data, NULL);
    /* The source owns its reference after g_io_add_watch_full(). */
    g_io_channel_unref(channel);
    return source;
}

guint ace_console_dispatch_schedule_frame(guint existing_source,
                                           GSourceFunc callback,
                                           gpointer data)
{
    if (existing_source)
        return existing_source;
    return g_timeout_add_full(G_PRIORITY_LOW,
                              ACE_CONSOLE_FRAME_MILLISECONDS,
                              callback, data, NULL);
}

int ace_console_dispatch_control(pid_t controller_pid, int character)
{
    int signal_number = character == 3 ? SIGUSR1 :
                        character == 4 ? SIGUSR2 :
                        character == 5 ? SIGRTMIN :
                        character == 6 ? SIGRTMIN + 1 : 0;

    if (controller_pid <= 0 || !signal_number) {
        errno = EINVAL;
        return -1;
    }
    return kill(controller_pid, signal_number);
}
