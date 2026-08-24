#ifndef AMIGA_SHELL_BROKER_CLIENT_H
#define AMIGA_SHELL_BROKER_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int native_broker_ensure(void);
int native_broker_attach(void);

/*
 * Call this, and only this, in a freshly fork()ed child before it makes any
 * broker request of its own -- e.g. to resolve its starting directory before
 * exec().  fork() duplicates the connection's file descriptor, not the
 * connection: parent and child would otherwise share one socket underneath
 * them, each independently writing a request and reading a response on it
 * with no coordination at all, since they are separate processes from the
 * kernel's point of view the instant fork() returns.  Whichever one loses
 * the race for the other's response bytes desyncs the protocol -- the next
 * thing it reads is the tail of somebody else's answer, not the header it
 * expects.
 *
 * This drops the child's reference to that inherited connection (closing it
 * costs the parent nothing; each fd is its own reference to the shared
 * kernel object) so the next broker call the child makes opens one of its
 * own instead of reusing something another process is also using.
 */
void native_broker_reset_after_fork(void);
int native_broker_resolve_path(const char *path, char *result, size_t result_size);

/* The same, for a caller about to open the result.  Answers ENXIO for an
   object that is not a file to be read -- a device node, a socket, a FIFO --
   so that nothing has to discover it by opening one. */
int native_broker_resolve_path_for_open(const char *path, char *result,
                                        size_t result_size);
int native_broker_resolve_beneath(const char *base, const char *relative,
                                  char *result, size_t result_size);
int native_broker_name_from_host(const char *path, char *result,
                                 size_t result_size);
int native_broker_getcwd(char *result, size_t result_size);
int native_broker_setcwd(const char *path);
int native_broker_assign(const char *name, const char *path);
int native_broker_assign_ex(const char *name, const char *path,
                            uint32_t flags);
int native_broker_listassigns(char *result, size_t result_size);
int native_broker_getvar(const char *name, uint32_t flags,
                         char *result, size_t result_size);
int native_broker_setvar(const char *name, const char *value, uint32_t flags);
int native_broker_deletevar(const char *name, uint32_t flags);
int native_broker_listvars(uint32_t flags, char *result, size_t result_size);
int native_broker_getcli(char *result, size_t result_size);
int native_broker_setfaillevel(int32_t fail_level);
int native_broker_setprompt(const char *prompt);
int native_broker_clone_session(const char *child_session);
int native_broker_getresult(char *result, size_t result_size);
int native_broker_setresult(int32_t return_code, int32_t result2);
int native_broker_listdos(char *result, size_t result_size);
int native_broker_status(char *result, size_t result_size);
int native_broker_relabel(const char *drive, const char *name);
int native_broker_listpath(char *result, size_t result_size);
int native_broker_path(const char *path, uint32_t flags);

typedef void (*native_broker_task_signal_handler)(uint32_t signals,
                                                  void *context);
int native_broker_task_attach(const char *name,
                              native_broker_task_signal_handler handler,
                              void *context, uint64_t *task_id);
int native_broker_task_find(const char *name, uint64_t *task_id);
/* This process's message-delivery channel. Attached lazily on first port
   use, by whichever call gets there first, and idempotent so that the others
   can call it too. The handler runs on the channel's own reader thread, so
   it must be safe to call while this process is blocked in WaitPort() -- that
   being the whole reason the channel exists.

   payload is NUL-terminated for convenience and its length given separately,
   because a serialised message may legitimately contain NUL bytes.  It is
   valid only for the duration of the call.

   stdin_fd and stdout_fd are the sender's own streams when the message
   carried them, or -1. They are handed to the handler, which owns them from
   that moment and must close them; a handler that ignores them leaks them.
   See AMIGA_BROKER_PORT_ATTACH and struct amiga_broker_port_record in
   broker_protocol.h. */
typedef void (*native_broker_port_record_handler)(uint32_t operation,
                                                  uint64_t message_id,
                                                  uint64_t port_id,
                                                  const char *payload,
                                                  size_t payload_length,
                                                  int stdin_fd, int stdout_fd,
                                                  void *context);
int native_broker_port_attach(native_broker_port_record_handler handler,
                              void *context, uint64_t *channel_id);
/* Add a second consumer to the process's delivery channel. This is used by
   private protocol users such as the GUI requestor alongside the ARexx port
   bridge. The handler remains installed for the life of the process. */
int native_broker_port_add_handler(native_broker_port_record_handler handler,
                                   void *context);
/* Send a message to a named port, and answer one that arrived on this
   process's channel. The payload is counted bytes, never inspected in
   transit, and may contain anything including NULs -- it is an ARexx message,
   not a string.

   Both need native_broker_port_attach() to have been called first, senders
   included: a reply comes back down the channel, so a process with no channel
   would wait for something that could never arrive. port_put fails with
   ENOTCONN rather than sending into that trap.

   port_put reports ESRCH, and only ESRCH, when no process has registered the
   name. Callers are expected to say so usefully: nothing starts RexxMast
   automatically, so "no such port" is the ordinary first experience rather
   than an exceptional one. */
int native_broker_port_put(const char *name, const void *message,
                           size_t length, int stdin_fd, int stdout_fd,
                           uint64_t *message_id);
int native_broker_port_reply(uint64_t message_id, const void *reply,
                             size_t length);
/* Push an opaque event to every attached port channel. The broker does not
   inspect or correlate the payload; recipients decide whether it applies to
   them. */
int native_broker_port_broadcast(const void *event, size_t length);
/* Public message ports: claim a name, give it up, or resolve one another
   process claimed. See AMIGA_BROKER_PORT_ADD in broker_protocol.h. */
int native_broker_port_add(const char *name, uint64_t *port_id);
int native_broker_port_remove(uint64_t port_id);
int native_broker_port_find(const char *name, uint64_t *port_id);
int native_broker_task_signal(uint64_t task_id, uint32_t signals);
int native_broker_task_set_foreground_pid(pid_t pid);
int native_broker_task_break_foreground(uint32_t signals);
int native_broker_task_list(char *result, size_t result_size);

/*
 * One privileged file operation, performed by the FMM/CRM service's CRM on
 * this process's behalf.  privop names the FMM/CRM service operation; second is the
 * destination of a rename and NULL otherwise; received_fd takes the
 * descriptor an opening operation produced.
 *
 * Paths are absolute host paths.  Which resolution domain they end up in is
 * the broker's decision, because that is where path translation lives.
 */
/* 0 when the operation was performed, 1 when the CRM answered and refused,
   -1 when the exchange did not complete.  errno carries the refusal in the
   middle case and the transport failure in the last. */
int native_broker_privop(uint32_t privop, const char *path, const char *second,
                         uint32_t flags, uint32_t mode, int64_t when,
                         int *received_fd);

/* Where the FMM/CRM service put the device roots, or empty for a session without a
   device view.  Paths beneath it exist only in the FMM/CRM service's namespace. */
/* The session's count of operations that needed the CRM, and whether the
   shell is reporting it.  `what` is one of AMIGA_BROKER_TALLY_*. */
int native_broker_tally(uint32_t what, char *result, size_t result_size);

/* Start (or retain) one shared Piper voice. Its process belongs to the
 * broker, not to the short-lived command process that requested it. */
int native_broker_say_warm(const char *voice);
int native_broker_say_status(char *result, size_t result_size);

int native_broker_view_root(char *result, size_t result_size);

#endif
