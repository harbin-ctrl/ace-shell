#ifndef ACE_NATIVE_HOST_H
#define ACE_NATIVE_HOST_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include <exec/types.h>

#define NATIVE_ENDCLI_STATUS 201

/* The descriptor a shell running a script hands to the commands it starts,
   so a command can find cli_CurrentInput -- the stream AmigaDOS would have
   let it read straight out of the shared CLI. Named in the environment for
   the same reason ACE_COMMAND_ARGUMENTS is: it is the only channel a fork
   and an exec leave open between an ACE shell and an ACE command. */
#define ACE_SCRIPT_INPUT_VARIABLE "ACE_SCRIPT_INPUT"

/* The script a shell was started to run instead of the usual startup set,
   and the marker that says it was started to run only that. */
#define ACE_STARTUP_SCRIPT_VARIABLE "ACE_STARTUP_SCRIPT"

void native_cli_set_script_input(FILE *file);
FILE *native_cli_script_input(void);
void native_set_interactive(int interactive);
void native_publish_shell_result(void);
int native_execute_script(const char *name);
int native_quit_script(void);

BPTR native_console_open(const char *specification);
int native_console_is_handle(BPTR handle);
int native_console_is_instance(BPTR handle);
const char *native_console_specification(BPTR handle);
const char *native_console_session(BPTR handle);
int native_console_dup_input(BPTR handle);
int native_console_dup_output(BPTR handle);
void native_console_close(BPTR handle);
int native_console_is_raw_mode(BPTR handle);
/* True only when both handles identify the same concrete ACE console
   endpoint.  It intentionally does not make abstract CON: handles
   exportable as POSIX descriptors. */
int native_console_same_endpoint(BPTR first, BPTR second);
int native_console_geometry(BPTR handle, int *rows, int *cols);
unsigned long native_console_resize_generation(BPTR handle);
void native_console_notify_resize(int rows, int cols);
int native_console_take_resize(BPTR handle);
BPTR native_lock_host_path(const char *path);
int native_command_path(const char *name, char *result, size_t result_size);
int native_run_background(const char *command, uint64_t *task_id);
void native_request_endcli(void);

#endif
