#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <dos/dos.h>

#include "ace_modes.h"

static int resolve_executable_path(const char *argv0, char *path,
                                   size_t path_size)
{
    ssize_t length;

    if (realpath(argv0, path))
        return 0;
    length = readlink("/proc/self/exe", path, path_size - 1);
    if (length < 0 || (size_t)length >= path_size - 1)
        return -1;
    path[length] = '\0';
    return 0;
}

int main(int argc, char **argv)
{
    struct ace_mode_options modes;
    char executable[PATH_MAX];
    char console_path[PATH_MAX];
    char *slash;
    const char *session = getenv("ACE_SESSION");
    int desktop = 0;
    int output = 1;
    pid_t child;

    /* A desktop launcher must remain attached to the real GUI process.  Keep
       this switch private to ace-shell rather than teaching every ACE mode
       consumer about it. */
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--desktop") == 0)
            desktop = 1;
        else
            argv[output++] = argv[index];
    }
    argv[output] = NULL;
    argc = output;

    if (ace_mode_parse(&argc, argv, &modes) != 0) {
        fprintf(stderr, "usage: %s [--root] [--desktop]\n", argv[0]);
        return RETURN_FAIL;
    }
    if (ace_mode_configure(&modes) != 0) {
        /* Being root is the one failure worth explaining rather than
           reporting: the user did something reasonable and needs to know
           that ACE gets its privilege elsewhere. */
        if (errno == EPERM)
            fprintf(stderr,
                    "ace-shell: ACE must be started as a normal user; privileged "
                    "operations\nare provided by the ACE fmm.\n");
        else
            fprintf(stderr, "ace-shell: requested mode is unavailable: %s\n",
                    strerror(errno));
        return RETURN_FAIL;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--root] [--desktop]\n", argv[0]);
        return RETURN_FAIL;
    }
    if (!session || !*session)
        session = "default";
    if (resolve_executable_path(argv[0], executable, sizeof(executable)) != 0) {
        fputs("ace-shell: console unavailable\n", stderr);
        return RETURN_FAIL;
    }
    slash = strrchr(executable, '/');
    if (!slash) {
        fputs("ace-shell: console unavailable\n", stderr);
        return RETURN_FAIL;
    }
    *slash = '\0';
    if (snprintf(console_path, sizeof(console_path), "%s/ace-console",
                 executable) >= (int)sizeof(console_path)) {
        fputs("ace-shell: console unavailable\n", stderr);
        return RETURN_FAIL;
    }

    if (desktop) {
        /* The panel's GAppInfo launch must track this long-lived process, not
           a short-lived fork parent which exits before GTK maps the window. */
        execl(console_path, console_path, "--session", session,
              (char *)NULL);
        fputs("ace-shell: console unavailable\n", stderr);
        return RETURN_FAIL;
    }

    child = fork();
    if (child < 0) {
        fputs("ace-shell: console unavailable\n", stderr);
        return RETURN_FAIL;
    }
    if (child == 0) {
        (void)setsid();
        execl(console_path, console_path, "--session", session,
              (char *)NULL);
        _exit(RETURN_FAIL);
    }
    return RETURN_OK;
}
