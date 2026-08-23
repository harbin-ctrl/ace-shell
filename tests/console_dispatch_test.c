#include "console_dispatch.h"

#include <assert.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct dispatch_test {
    GMainLoop *loop;
    int sockets[2];
    int output_callbacks;
    int control_callbacks;
    int frame_callbacks;
    pid_t controller_pid;
};

static gboolean output_ready(GIOChannel *channel, GIOCondition condition,
                             gpointer data)
{
    struct dispatch_test *test = data;
    unsigned char byte;

    (void)channel;
    (void)condition;
    test->output_callbacks++;
    assert(read(test->sockets[0], &byte, 1) == 1);
    assert(write(test->sockets[1], &byte, 1) == 1);
    return G_SOURCE_CONTINUE;
}

static gboolean control_ready(gpointer data)
{
    struct dispatch_test *test = data;

    test->control_callbacks++;
    assert(ace_console_dispatch_control(test->controller_pid, 3) == 0);
    g_main_loop_quit(test->loop);
    return G_SOURCE_REMOVE;
}

static gboolean frame_ready(gpointer data)
{
    struct dispatch_test *test = data;

    test->frame_callbacks++;
    g_main_loop_quit(test->loop);
    return G_SOURCE_REMOVE;
}

int main(void)
{
    struct dispatch_test test = {0};
    guint output_source;
    guint frame_source;
    unsigned char byte = 'x';
    int status;
    sigset_t mask;
    sigset_t previous_mask;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, test.sockets) == 0);
    assert(write(test.sockets[1], &byte, 1) == 1);
    test.loop = g_main_loop_new(NULL, FALSE);
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    assert(sigprocmask(SIG_BLOCK, &mask, &previous_mask) == 0);
    test.controller_pid = fork();
    assert(test.controller_pid >= 0);
    if (test.controller_pid == 0) {
        int received;

        assert(sigwait(&mask, &received) == 0);
        _exit(received == SIGUSR1 ? 0 : 1);
    }
    assert(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0);

    /* A continuously readable output source must not run ahead of a default
       priority control/key event. */
    output_source = ace_console_dispatch_add_output_watch(
        test.sockets[0], output_ready, &test);
    assert(output_source != 0);
    assert(g_idle_add_full(G_PRIORITY_DEFAULT, control_ready, &test, NULL) != 0);
    g_main_loop_run(test.loop);
    assert(test.control_callbacks == 1);
    assert(test.output_callbacks == 0);
    assert(waitpid(test.controller_pid, &status, 0) == test.controller_pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(g_source_remove(output_source));

    /* Many pages becoming dirty before the next frame still schedule one
       presentation only. */
    frame_source = ace_console_dispatch_schedule_frame(0, frame_ready, &test);
    assert(frame_source != 0);
    for (int page = 0; page < 100; page++)
        assert(ace_console_dispatch_schedule_frame(
                   frame_source, frame_ready, &test) == frame_source);
    g_main_loop_run(test.loop);
    assert(test.frame_callbacks == 1);

    g_main_loop_unref(test.loop);
    close(test.sockets[0]);
    close(test.sockets[1]);
    return 0;
}
