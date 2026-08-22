#ifndef ACE_SHELL_BREAK_H
#define ACE_SHELL_BREAK_H

#include <sys/types.h>

enum ace_shell_foreground_kind {
    ACE_SHELL_FOREGROUND_NONE = 0,
    ACE_SHELL_FOREGROUND_ACE,
    ACE_SHELL_FOREGROUND_LNX,
};

/* SIGUSR1 is ACE's private console-to-shell Ctrl-C notification. */
void ace_shell_break_init(void);
void ace_shell_break_set_foreground(
    pid_t child, enum ace_shell_foreground_kind kind);

#endif
