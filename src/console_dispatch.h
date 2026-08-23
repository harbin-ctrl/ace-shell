#ifndef ACE_CONSOLE_DISPATCH_H
#define ACE_CONSOLE_DISPATCH_H

#include <glib.h>
#include <sys/types.h>

/* Console output is bulk presentation work. Keyboard/window sources use
 * GLib's default priority and must be allowed to interrupt a continuously
 * readable child socket. */
guint ace_console_dispatch_add_output_watch(int fd, GIOFunc callback,
                                             gpointer data);

/* Collapse any number of output updates into the next display frame. */
guint ace_console_dispatch_schedule_frame(guint existing_source,
                                           GSourceFunc callback,
                                           gpointer data);

/* Deliver an Amiga Ctrl-C/D/E/F chord to the shell which owns the console. */
int ace_console_dispatch_control(pid_t controller_pid, int character);

#endif
