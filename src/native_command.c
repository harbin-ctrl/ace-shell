#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <dos/dos.h>

#include "broker_client.h"
#include "broker_protocol.h"
#include "ace_shell_break.h"
#include "aros_exec_runtime.h"
#include "lnx_pty.h"
#include "native_host.h"

int ace_dos_handle_descriptor(BPTR handle);

struct ace_command_segment {
    char path[PATH_MAX];
};

static volatile sig_atomic_t foreground_child;
static int break_pipe[2] = {-1, -1};
static pthread_t break_thread;

static void *broker_break_dispatch(void *unused)
{
    unsigned char byte;

    (void)unused;
    while (read(break_pipe[0], &byte, sizeof(byte)) > 0) {
        pid_t child = (pid_t)foreground_child;
        ULONG signals = byte == 'C' ? SIGBREAKF_CTRL_C :
                        byte == 'D' ? SIGBREAKF_CTRL_D :
                        byte == 'E' ? SIGBREAKF_CTRL_E :
                        byte == 'F' ? SIGBREAKF_CTRL_F : 0;
        int host_signal = byte == 'C' ? SIGUSR1 :
                          byte == 'D' ? SIGUSR2 :
                          byte == 'E' ? SIGRTMIN :
                          byte == 'F' ? SIGRTMIN + 1 : 0;

        if (child > 0 && signals &&
            native_broker_task_break_foreground(signals) != 0)
            (void)kill(child, host_signal); /* broker outage: retain break */
    }
    return NULL;
}

static void shell_break_handler(int signal_number)
{
    unsigned char event = signal_number == SIGUSR1 ? 'C' :
                          signal_number == SIGUSR2 ? 'D' :
                          signal_number == SIGRTMIN ? 'E' :
                          signal_number == SIGRTMIN + 1 ? 'F' : 0;

    if (foreground_child > 0 && break_pipe[1] >= 0)
        (void)write(break_pipe[1], &event, 1);
    else if (signal_number == SIGUSR1)
        ace_aros_runtime_raise_from_host(SIGBREAKF_CTRL_C);
    else if (signal_number == SIGRTMIN)
        ace_aros_runtime_raise_from_host(SIGBREAKF_CTRL_E);
    else if (signal_number == SIGRTMIN + 1)
        ace_aros_runtime_raise_from_host(SIGBREAKF_CTRL_F);
}

static void shell_script_break_handler(int signal_number)
{
    (void)signal_number;
    /* Ctrl-D belongs to the CLI.  Shell.c checks this after the foreground
       command returns and stops the remaining script at that boundary. */
    ace_aros_runtime_raise_from_host(SIGBREAKF_CTRL_D);
}

void ace_shell_break_init(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = shell_break_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (pipe(break_pipe) == 0 &&
        pthread_create(&break_thread, NULL, broker_break_dispatch, NULL) == 0)
    {
        int flags = fcntl(break_pipe[1], F_GETFL);

        if (flags >= 0)
            (void)fcntl(break_pipe[1], F_SETFL, flags | O_NONBLOCK);
        (void)pthread_detach(break_thread);
    }
    else {
        if (break_pipe[0] >= 0)
            close(break_pipe[0]);
        if (break_pipe[1] >= 0)
            close(break_pipe[1]);
        break_pipe[0] = break_pipe[1] = -1;
    }
    (void)sigaction(SIGUSR1, &action, NULL);
    (void)sigaction(SIGRTMIN, &action, NULL);
    (void)sigaction(SIGRTMIN + 1, &action, NULL);
    action.sa_handler = shell_script_break_handler;
    (void)sigaction(SIGUSR2, &action, NULL);
}

void ace_shell_break_set_foreground(pid_t child)
{
    foreground_child = (sig_atomic_t)child;
    (void)native_broker_task_set_foreground_pid(child);
}

/* A directory carries the execute bit too, meaning searchable rather than
   runnable, so being executable is not on its own enough to be a command. */
static int executable_file(const char *path)
{
    struct stat information;

    return access(path, X_OK) == 0 && stat(path, &information) == 0 &&
           S_ISREG(information.st_mode);
}

static int directory_command(const char *directory, const char *name,
                             char *result, size_t result_size)
{
    char amiga_directory[PATH_MAX];
    char amiga_candidate[PATH_MAX];
    char resolved[PATH_MAX];
    DIR *stream = opendir(directory);
    struct dirent *entry;
    char best[PATH_MAX];
    int found = 0;
    int written;

    /* Path resolution belongs to the broker: besides normal AmigaDOS case
       folding it understands the visible ^ spellings used for Linux names
       which collide only by case. */
    if (native_broker_name_from_host(directory, amiga_directory,
                                     sizeof(amiga_directory)) == 0) {
        written = snprintf(amiga_candidate, sizeof(amiga_candidate), "%s/%s",
                           amiga_directory, name);
        if (written >= 0 && (size_t)written < sizeof(amiga_candidate) &&
            native_broker_resolve_path(amiga_candidate, resolved,
                                       sizeof(resolved)) == 0 &&
            executable_file(resolved)) {
            if (strlen(resolved) >= result_size)
                return -1;
            strcpy(result, resolved);
            return 0;
        }
    }

    if (!stream)
        return -1;
    while ((entry = readdir(stream)) != NULL) {
        int written;

        if (strcasecmp(entry->d_name, name) != 0)
            continue;
        written = snprintf(result, result_size, "%s/%s", directory,
                           entry->d_name);
        if (written >= 0 && (size_t)written < sizeof(best) &&
            executable_file(result) &&
            (!found || strcmp(result, best) < 0)) {
            strcpy(best, result);
            found = 1;
        }
    }
    closedir(stream);
    if (!found || strlen(best) >= result_size)
        return -1;
    strcpy(result, best);
    return 0;
}

static int companion_command(const char *path)
{
    char executable[PATH_MAX];
    char candidate[PATH_MAX];
    char *slash;
    ssize_t length;

    length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0 || (size_t)length >= sizeof(executable) - 1)
        return 0;
    executable[length] = '\0';
    slash = strrchr(executable, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    if (!realpath(path, candidate))
        return 0;
    {
        size_t directory_length = strlen(executable);

        return strlen(candidate) > directory_length &&
               strncmp(candidate, executable, directory_length) == 0 &&
               candidate[directory_length] == '/';
    }
}

/* Named through the command drawer, which is where commands are: what
   loadCommand() falls back to when the current directory and the path list
   have nothing, and what it hands to LoadSeg() spelled exactly like this. */
static int named_through_command_drawer(const char *name)
{
    return strncasecmp(name, "C:", 2) == 0;
}

int native_command_path(const char *name, char *result, size_t result_size)
{
    char resolved[PATH_MAX];
    char paths[AMIGA_BROKER_MAX_PAYLOAD];
    char *save = NULL;
    char *path;
    char executable[PATH_MAX];
    char amiga_command[PATH_MAX];
    char *slash;
    ssize_t length;
    int written;

    if (!name || !*name || !result || result_size == 0)
        return -1;
    if (strchr(name, '/') || strchr(name, ':')) {
        /*
         * Two ways to be a command. Through C:, which is the Amiga answer --
         * the assign says where commands are, and the broker resolves the
         * name within it, so nothing outside it can be reached this way. Or
         * beside the running binary, which is how ACE recognised its own
         * before it had a C: to ask, and which still covers an uninstalled
         * build tree. Anything else is an arbitrary path on the host, and
         * arbitrary host programs go through LNX, deliberately.
         */
        if (native_broker_resolve_path(name, resolved, sizeof(resolved)) == 0 &&
            (named_through_command_drawer(name) ||
             companion_command(resolved)) &&
            executable_file(resolved) && strlen(resolved) < result_size) {
            strcpy(result, resolved);
            return 0;
        }
        return -1;
    }
    if (native_broker_resolve_path(name, resolved, sizeof(resolved)) == 0 &&
        executable_file(resolved) && strlen(resolved) < result_size) {
        strcpy(result, resolved);
        return 0;
    }

    /* C: is the AmigaDOS command drawer, not merely a path that happens to
       be present in a shell's mutable Path list.  Fresh broker sessions --
       especially the root/device-view session -- start with no user-added
       path entries, so keep the standard command drawer as an explicit
       fallback for an unqualified command. */
    written = snprintf(amiga_command, sizeof(amiga_command), "C:%s", name);
    if (written >= 0 && (size_t)written < sizeof(amiga_command) &&
        native_broker_resolve_path(amiga_command, resolved,
                                    sizeof(resolved)) == 0 &&
        executable_file(resolved) && strlen(resolved) < result_size) {
        strcpy(result, resolved);
        return 0;
    }

    if (native_broker_listpath(paths, sizeof(paths)) == 0) {
        path = strtok_r(paths, "\n", &save);
        while (path) {
            if (directory_command(path, name, result, result_size) == 0)
                return 0;
            path = strtok_r(NULL, "\n", &save);
        }
    }

    length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length >= 0 && (size_t)length < sizeof(executable) - 1) {
        executable[length] = '\0';
        slash = strrchr(executable, '/');
        if (slash) {
            *slash = '\0';
            if (directory_command(executable, name, result, result_size) == 0)
                return 0;
        }
    }

    /* The host PATH is deliberately not an AmigaDOS command path. Linux
       programs must be invoked explicitly through LNX. */
    return -1;
}

BPTR LoadSeg(CONST_STRPTR name)
{
    struct ace_command_segment *segment;

    segment = calloc(1, sizeof(*segment));
    if (!segment) {
        SetIoErr(ERROR_NO_FREE_STORE);
        return BNULL;
    }
    if (native_command_path(name, segment->path, sizeof(segment->path)) != 0) {
        free(segment);
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return BNULL;
    }
    return segment;
}

void UnLoadSeg(BPTR value)
{
    free(value);
}

struct Segment *FindSegment(CONST_STRPTR name, struct Segment *last,
                            BOOL system)
{
    (void)name;
    (void)last;
    (void)system;
    return NULL;
}

static int split_arguments(const char *input, size_t length, char *storage,
                           size_t storage_size, char **argv, size_t capacity)
{
    size_t input_index = 0;
    size_t storage_index = 0;
    size_t argument_count = 0;

    while (input_index < length) {
        int quoted = 0;

        while (input_index < length &&
               (input[input_index] == ' ' || input[input_index] == '\t' ||
                input[input_index] == '\r' || input[input_index] == '\n'))
            input_index++;
        if (input_index == length)
            break;
        if (argument_count + 1 >= capacity)
            return -1;
        argv[++argument_count] = storage + storage_index;
        while (input_index < length) {
            char character = input[input_index++];

            if (character == '"') {
                quoted = !quoted;
                continue;
            }
            if (!quoted && (character == ' ' || character == '\t' ||
                            character == '\r' || character == '\n'))
                break;
            if ((character == '*' || character == '\\') &&
                input_index < length)
                character = input[input_index++];
            if (storage_index + 1 >= storage_size)
                return -1;
            storage[storage_index++] = character;
        }
        if (storage_index + 1 >= storage_size)
            return -1;
        storage[storage_index++] = '\0';
    }
    argv[argument_count + 1] = NULL;
    return (int)argument_count;
}

/* Keep the original spelling for the child command. Its AROS ReadArgs()
   parser remains the authority for quotes, switches, and /F arguments. */
static const char *command_tail(const char *input)
{
    const char *read = input;
    int quoted = 0;

    while (*read == ' ' || *read == '\t' || *read == '\r' || *read == '\n')
        read++;
    while (*read) {
        if (*read == '"') {
            quoted = !quoted;
        } else if (!quoted && (*read == ' ' || *read == '\t' ||
                               *read == '\r' || *read == '\n')) {
            break;
        } else if ((*read == '*' || *read == '\\') && read[1]) {
            read++;
        }
        read++;
    }
    while (*read == ' ' || *read == '\t' || *read == '\r' || *read == '\n')
        read++;
    return read;
}

static void native_console_title(const char *title)
{
    char sequence[PATH_MAX + 16];
    const char *interactive = getenv("ACE_CONSOLE_INTERACTIVE");
    int length;

    /* Piped shells are still valid AROS environments.  GUI title protocol
       bytes belong only on the live ACE console stream. */
    if (!interactive || strcmp(interactive, "1") != 0)
        return;
    length = snprintf(sequence, sizeof(sequence), "\033]2;%s\007",
                      title ? title : "ACE Shell");
    if (length > 0 && (size_t)length < sizeof(sequence)) {
        (void)Write(Output(), sequence, length);
        (void)Flush(Output());
    }
}

static void native_command_title(const char *path)
{
    const char *slash = strrchr(path, '/');

    native_console_title(slash ? slash + 1 : path);
}

static int native_command_has_basename(const struct ace_command_segment *segment,
                                       const char *name)
{
    const char *basename;

    if (!segment)
        return 0;
    basename = strrchr(segment->path, '/');
    basename = basename ? basename + 1 : segment->path;
    /* segment->path is the resolved executable returned by LoadSeg(), not
       user argument text. ACE commands are deliberately loaded from C: or
       the executable's companion drawer, so a well-known resolved basename
       is the narrow identity needed at this command-runner boundary. */
    return strcasecmp(basename, name) == 0;
}

static int native_command_is_lnx(const struct ace_command_segment *segment)
{
    return native_command_has_basename(segment, "LNX");
}

static int native_command_is_endcli(const struct ace_command_segment *segment)
{
    return native_command_has_basename(segment, "EndCLI");
}

static int native_console_session_active(void)
{
    const char *interactive = getenv("ACE_CONSOLE_INTERACTIVE");

    return interactive && strcmp(interactive, "1") == 0;
}

/*
 * Execute.
 *
 * AROS's own Execute.c does this by handing the opened script to
 * cli_CurrentInput, or, when the shell is already reading a script, by
 * writing the new script and the unread remainder of the old one into a
 * temporary file and pointing cli_CurrentInput at that. Both work because
 * the command and the shell are one process sharing one CLI. An ACE command
 * is its own Linux process and cannot redirect the shell that started it, so
 * running AROS's Execute unchanged would be worse than useless: copying the
 * remainder would consume the shell's script to the end, and the shell would
 * come back to a stream with nothing left in it.
 *
 * What ACE has instead of a shared CLI is a shared file description. When
 * the caller is running a script, the script is a private temporary file and
 * the command holds a descriptor onto it at the shell's own read position --
 * so writing the new script in at that position, ahead of what has not been
 * read yet, puts the commands where the shell is about to read. That is the
 * same splice AROS makes, made in the file rather than in the CLI.
 */
static int splice_into_script(FILE *script, BPTR source)
{
    off_t position;
    char *remainder = NULL;
    size_t remainder_length = 0;
    struct stat information;
    int descriptor = fileno(script);
    int result = RETURN_FAIL;
    LONG count;
    char buffer[4096];

    if (descriptor < 0)
        return RETURN_FAIL;
    position = lseek(descriptor, 0, SEEK_CUR);
    if (position < 0 || fstat(descriptor, &information) != 0)
        return RETURN_FAIL;

    /* Everything the shell has not read yet, held aside while the script
       goes in front of it. */
    if (information.st_size > position) {
        remainder_length = (size_t)(information.st_size - position);
        remainder = malloc(remainder_length);
        if (!remainder)
            return RETURN_FAIL;
        if (pread(descriptor, remainder, remainder_length, position) !=
            (ssize_t)remainder_length) {
            free(remainder);
            return RETURN_FAIL;
        }
    }

    if (lseek(descriptor, position, SEEK_SET) < 0)
        goto done;
    while ((count = Read(source, buffer, (LONG)sizeof(buffer))) > 0) {
        if (write(descriptor, buffer, (size_t)count) != (ssize_t)count)
            goto done;
    }
    if (count < 0)
        goto done;
    /* A script that does not end in a newline would otherwise run its last
       line into the caller's next one. */
    if (write(descriptor, "\n", 1) != 1)
        goto done;
    if (remainder_length &&
        write(descriptor, remainder, remainder_length) !=
        (ssize_t)remainder_length)
        goto done;
    if (ftruncate(descriptor, lseek(descriptor, 0, SEEK_CUR)) != 0)
        goto done;
    /* Back to where the shell was: the next thing it reads is the script. */
    if (lseek(descriptor, position, SEEK_SET) < 0)
        goto done;
    result = RETURN_OK;

done:
    free(remainder);
    return result;
}

/* No script to splice into, so the script gets a shell of its own. It shares
   this session, so the directory it changes and the variables it sets are
   still there afterwards, which is the part of AmigaOS's behaviour a caller
   can actually observe. */
static int execute_in_nested_shell(const char *name)
{
    char shell_path[PATH_MAX];
    char executable[PATH_MAX];
    char *slash;
    ssize_t length;
    pid_t child;
    int status;

    length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0 || (size_t)length >= sizeof(executable) - 1)
        return RETURN_FAIL;
    executable[length] = '\0';
    slash = strrchr(executable, '/');
    if (!slash)
        return RETURN_FAIL;
    *slash = '\0';
    if (snprintf(shell_path, sizeof(shell_path), "%s/ace-user-shell",
                 executable) >= (int)sizeof(shell_path))
        return RETURN_FAIL;

    child = fork();
    if (child < 0)
        return RETURN_FAIL;
    if (child == 0) {
        int null_fd = open("/dev/null", O_RDONLY);

        /* The script's own commands read from the script, and the shell
           stops when it ends rather than turning to a standard input that
           belongs to whoever called Execute. */
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        (void)unsetenv(ACE_SCRIPT_INPUT_VARIABLE);
        (void)unsetenv("ACE_COMMAND_ARGUMENTS");
        if (setenv(ACE_STARTUP_SCRIPT_VARIABLE, name, 1) != 0)
            _exit(RETURN_FAIL);
        execl(shell_path, shell_path, (char *)NULL);
        _exit(RETURN_FAIL);
    }
    if (waitpid(child, &status, 0) < 0)
        return RETURN_FAIL;
    return WIFEXITED(status) ? WEXITSTATUS(status) : RETURN_FAIL;
}

int native_execute_script(const char *name)
{
    FILE *script;
    BPTR source;
    int result;

    if (!name || !*name) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return RETURN_FAIL;
    }
    script = native_cli_script_input();
    if (!script)
        return execute_in_nested_shell(name);
    source = Open(name, MODE_OLDFILE);
    if (!source)
        return RETURN_FAIL;
    result = splice_into_script(script, source);
    Close(source);
    if (result != RETURN_OK)
        SetIoErr(ERROR_OBJECT_IN_USE);
    return result;
}

/* Run must report the process it created.  The broker assigns that ID as the
   new program starts, so wait briefly for the task registration made by its
   command entry point. */
static uint64_t native_background_task_id(pid_t pid)
{
    char listing[AMIGA_BROKER_MAX_PAYLOAD];
    struct timespec pause = {0, 10000000L};

    for (int attempt = 0; attempt < 100; attempt++) {
        char *line;

        if (native_broker_task_list(listing, sizeof(listing)) == 0) {
            for (line = strtok(listing, "\n"); line;
                 line = strtok(NULL, "\n")) {
                char *separator = strchr(line, '\t');
                char *end;
                long listed_pid;

                if (!separator)
                    continue;
                listed_pid = strtol(separator + 1, &end, 10);
                if (end == separator + 1 || *end != '\t' ||
                    listed_pid != (long)pid)
                    continue;
                return strtoull(line, NULL, 10);
            }
        }
        (void)nanosleep(&pause, NULL);
    }
    return 0;
}

int native_run_background(const char *command, uint64_t *task_id)
{
    char storage[4096];
    char *argv[128] = {0};
    char command_path[PATH_MAX];
    char cwd[PATH_MAX] = {0};
    const char *tail;
    int argument_count;
    pid_t child;
    pid_t background_pid = 0;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int outcome;

    if (task_id)
        *task_id = 0;

    if (!command || !*command) {
        SetIoErr(ERROR_BAD_TEMPLATE);
        return -1;
    }
    argument_count = split_arguments(command, strlen(command), storage,
                                     sizeof(storage), argv,
                                     sizeof(argv) / sizeof(argv[0]));
    if (argument_count < 1 ||
        native_command_path(argv[1], command_path, sizeof(command_path)) != 0 ||
        native_broker_getcwd(cwd, sizeof(cwd)) != 0) {
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return -1;
    }
    tail = command_tail(command);
    if (setenv("ACE_COMMAND_ARGUMENTS", tail, 1) != 0 ||
        posix_spawn_file_actions_init(&actions) != 0 ||
        posix_spawnattr_init(&attributes) != 0) {
        SetIoErr(ERROR_NO_FREE_STORE);
        return -1;
    }
    (void)posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                            "/dev/null", O_RDONLY, 0);
    if (cwd[0] != '\0')
        (void)posix_spawn_file_actions_addchdir_np(&actions, cwd);
    (void)posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    argv[0] = command_path;
    for (int index = 1; index < argument_count; index++)
        argv[index] = argv[index + 1];
    argv[argument_count] = NULL;
    outcome = posix_spawn(&child, command_path, &actions, &attributes, argv,
                          environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (outcome != 0) {
        errno = outcome;
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return -1;
    }
    background_pid = child;
    if (task_id)
        *task_id = native_background_task_id(background_pid);
    return 0;
}

LONG RunCommand(BPTR value, ULONG stack, STRPTR arguments, LONG length)
{
    struct ace_command_segment *segment = value;
    char storage[4096];
    char *argv[128] = {0};
    char cwd[PATH_MAX];
    char script_name[16];
    FILE *script;
    int script_fd = -1;
    pid_t child;
    int status;
    int input_descriptor;
    int output_descriptor;
    int lnx_command;
    int lnx_pty_mode;
    BPTR selected_input;
    BPTR selected_output;
    sigset_t break_mask;
    sigset_t previous_mask;

    (void)stack;
    if (!segment || !arguments || length < 0 ||
        (size_t)length >= sizeof(storage) ||
        split_arguments(arguments, (size_t)length, storage, sizeof(storage),
                         argv, sizeof(argv) / sizeof(argv[0])) < 0) {
        SetIoErr(ERROR_BAD_TEMPLATE);
        return RETURN_FAIL;
    }
    argv[0] = segment->path;
    native_command_title(segment->path);
    /* Redirection has already selected Input()/Output().  A real ACE console
       contributes the same endpoint on both sides; files, pipes, separate
       consoles, and abstract CON: handles do not satisfy this complete
       predicate.  The descriptor checks are intentionally before fork so a
       PTY is chosen only when LNX can inherit both sides of this bridge. */
    selected_input = Input();
    selected_output = Output();
    input_descriptor = ace_dos_handle_descriptor(selected_input);
    output_descriptor = ace_dos_handle_descriptor(selected_output);
    lnx_command = native_command_is_lnx(segment);
    lnx_pty_mode = native_console_session_active() && lnx_command &&
                   input_descriptor >= 0 && output_descriptor >= 0 &&
                   native_console_same_endpoint(selected_input, selected_output);
    script = native_cli_script_input();
    if (script) {
        script_fd = fileno(script);
        if (script_fd >= 0) {
            /* Not the file, the file description: the child gets the same
               offset, so a command that reads the script -- If skipping
               forward to its EndIf -- moves the shell's own position with
               it, which is how the block ends up skipped. */
            (void)fcntl(script_fd, F_SETFD,
                        fcntl(script_fd, F_GETFD) & ~FD_CLOEXEC);
            snprintf(script_name, sizeof(script_name), "%d", script_fd);
        } else {
            script = NULL;
        }
    }
    sigemptyset(&break_mask);
    sigaddset(&break_mask, SIGUSR1);
    (void)sigprocmask(SIG_BLOCK, &break_mask, &previous_mask);
    child = fork();
    if (child < 0) {
        (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);
        native_console_title("ACE Shell");
        SetIoErr(ERROR_NO_FREE_STORE);
        return RETURN_FAIL;
    }
    if (child == 0) {
        (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);
        native_broker_reset_after_fork();
        if (native_broker_getcwd(cwd, sizeof(cwd)) == 0)
            (void)chdir(cwd);
        if (setenv("ACE_COMMAND_ARGUMENTS", arguments, 1) != 0)
            _exit(RETURN_FAIL);
        /* Shell.c's AmigaDOS 3.1 redirection parser has already selected
           these streams with SelectInput()/SelectOutput().  They are FILE *
           state in this process, whereas execv() gives the command only its
           POSIX descriptors, so carry the selected streams across the exec
           boundary.  The argument line remains in ACE_COMMAND_ARGUMENTS;
           native_dos.c supplies that prefix before redirected stdin. */
        if ((input_descriptor >= 0 &&
             dup2(input_descriptor, STDIN_FILENO) < 0) ||
            (output_descriptor >= 0 &&
             dup2(output_descriptor, STDOUT_FILENO) < 0))
            _exit(RETURN_FAIL);
        if (lnx_command) {
            if (setenv(ACE_LNX_TARGET_TERM_VARIABLE, ACE_LNX_PTY_VALUE, 1) !=
                0)
                _exit(RETURN_FAIL);
            if (lnx_pty_mode &&
                setenv(ACE_LNX_PTY_VARIABLE, ACE_LNX_PTY_VALUE, 1) != 0)
                _exit(RETURN_FAIL);
            if (!lnx_pty_mode)
                (void)unsetenv(ACE_LNX_PTY_VARIABLE);
        } else {
            (void)unsetenv(ACE_LNX_PTY_VARIABLE);
            (void)unsetenv(ACE_LNX_TARGET_TERM_VARIABLE);
        }
        if (script && setenv(ACE_SCRIPT_INPUT_VARIABLE, script_name, 1) != 0)
            _exit(RETURN_FAIL);
        if (!script)
            (void)unsetenv(ACE_SCRIPT_INPUT_VARIABLE);
        execv(segment->path, argv);
        _exit(RETURN_FAIL);
    }
    ace_shell_break_set_foreground(child);
    (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);
    if (waitpid(child, &status, 0) < 0) {
        ace_shell_break_set_foreground(0);
        native_console_title("ACE Shell");
        SetIoErr(ERROR_OBJECT_NOT_FOUND);
        return RETURN_FAIL;
    }
    ace_shell_break_set_foreground(0);
    if (script) {
        /* Whatever the command read is gone from the script for good. The
           descriptor knows that; this stream does not until it is told. */
        off_t position = lseek(script_fd, 0, SEEK_CUR);

        if (position >= 0)
            (void)fseeko(script, position, SEEK_SET);
    }
    native_console_title("ACE Shell");
    if (WIFEXITED(status) && WEXITSTATUS(status) == NATIVE_ENDCLI_STATUS) {
        native_request_endcli();
        return RETURN_OK;
    }
    /* EndCLI normally propagates this through native_publish_result(), but
       its AROS entry can also return RETURN_OK after setting the CLI flag in
       the short-lived command child.  The resolved command identity gives
       the parent the same unambiguous request without depending on that
       child-local state. */
    if (WIFEXITED(status) && WEXITSTATUS(status) == RETURN_OK &&
        native_command_is_endcli(segment)) {
        native_request_endcli();
        return RETURN_OK;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == RETURN_OK) {
        char requested[8];

        if (native_broker_getvar("__ACE_ENDCLI", AMIGA_BROKER_VAR_LOCAL,
                                 requested, sizeof(requested)) == 0) {
            (void)native_broker_deletevar("__ACE_ENDCLI",
                                          AMIGA_BROKER_VAR_LOCAL);
            native_request_endcli();
        }
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return RETURN_FAIL;
}
