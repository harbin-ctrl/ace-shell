#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include <dos/stdio.h>
#include <proto/dos.h>
#include <proto/exec.h>

#ifdef LINE_MAX
#undef LINE_MAX
#endif
#include "broker_client.h"
#include "Shell.h"

#undef DOSBase
#undef SysBase

#define HOST_MAX_WORDS 128

static int shell_done;
static char shell_directory[PATH_MAX];

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

static int split_words(char *line, char **words, size_t capacity)
{
    char *read = line;
    size_t count = 0;

    while (*read) {
        char *write;
        BOOL quoted = FALSE;

        while (isspace((unsigned char)*read))
            read++;
        if (!*read)
            break;
        if (count + 1 >= capacity)
            return -1;
        words[count++] = write = read;
        while (*read) {
            if (*read == '"') {
                quoted = !quoted;
                read++;
            } else if (*read == '\\' && read[1]) {
                *write++ = *++read;
                read++;
            } else if (!quoted && isspace((unsigned char)*read)) {
                read++;
                break;
            } else {
                *write++ = *read++;
            }
        }
        if (quoted)
            return -1;
        *write = '\0';
    }
    words[count] = NULL;
    return (int)count;
}

static int find_in_directory(const char *directory, const char *command,
                             char *result, size_t result_size)
{
    DIR *listing;
    struct dirent *entry;
    char path[PATH_MAX];
    const char *name = command;

    listing = opendir(directory);
    if (!listing)
        return -1;
    while ((entry = readdir(listing)) != NULL) {
        if (strcasecmp(entry->d_name, command) == 0) {
            name = entry->d_name;
            break;
        }
    }
    if (!entry || snprintf(path, sizeof(path), "%s/%s", directory, name) < 0 ||
        strlen(path) >= result_size || access(path, X_OK) != 0) {
        closedir(listing);
        return -1;
    }
    closedir(listing);
    strcpy(result, path);
    return 0;
}

static int find_command(const char *command, char *result, size_t result_size)
{
    if (strchr(command, '/')) {
        if (access(command, X_OK) != 0 || strlen(command) >= result_size)
            return -1;
        strcpy(result, command);
        return 0;
    }
    if (shell_directory[0] &&
        find_in_directory(shell_directory, command, result, result_size) == 0)
        return 0;
    /* Linux commands are available only through the explicit Linux command. */
    return -1;
}

static BOOL line_is_blank(const Buffer *line)
{
    for (LONG index = 0; index < line->len; index++) {
        unsigned char character = (unsigned char)line->buf[index];

        if (character != '\n' && character != '\r' &&
            !isspace(character))
            return FALSE;
    }
    return TRUE;
}

static LONG executeLine(ShellState *ss, STRPTR command_args)
{
    char command_path[PATH_MAX];
    char line[4096];
    char *words[HOST_MAX_WORDS] = {0};
    int count;
    pid_t child;
    int status;
    const char *command = ss->command + 2;

    if (!strcasecmp(command, "ENDCLI") || !strcasecmp(command, "ENDSHELL")) {
        shell_done = 1;
        return 0;
    }
    if (snprintf(line, sizeof(line), "%s %s", command,
                 command_args ? command_args : "") >= (int)sizeof(line))
        return ERROR_LINE_TOO_LONG;
    count = split_words(line, words, HOST_MAX_WORDS);
    if (count <= 0)
        return ERROR_OBJECT_NOT_FOUND;

    SetProgramName(command);
    child = fork();
    if (child < 0)
        return ERROR_NO_FREE_STORE;
    if (child == 0) {
        char cwd[PATH_MAX];
        FILE *input = (FILE *)Input();
        FILE *output = (FILE *)Output();

        native_broker_reset_after_fork();
        if (input)
            dup2(fileno(input), STDIN_FILENO);
        if (output)
            dup2(fileno(output), STDOUT_FILENO);
        if (native_broker_getcwd(cwd, sizeof(cwd)) == 0)
            (void)chdir(cwd);
        if (find_command(words[0], command_path, sizeof(command_path)) == 0) {
            words[0] = command_path;
            execv(command_path, words);
        }
        _exit(RETURN_FAIL);
    }
    if (waitpid(child, &status, 0) < 0)
        return ERROR_OBJECT_NOT_FOUND;
    if (WIFEXITED(status))
        return WEXITSTATUS(status) == 0 ? 0 : WEXITSTATUS(status);
    return RETURN_FAIL;
}

LONG readLine(ShellState *ss, struct CommandLineInterface *cli,
              Buffer *out, BOOL *more_left)
{
    char line[LINE_MAX];
    LONG character, length = 0;
    BOOL comment = FALSE, quoted = FALSE, escaped = FALSE;
    (void)ss;

    while (length < LINE_MAX - 1) {
        character = FGetC(cli->cli_CurrentInput);
        if (character == ENDSTREAMCH)
            break;
        if (character == '"' && !escaped)
            quoted = !quoted;
        if (character == '*' && !escaped)
            escaped = TRUE;
        else
            escaped = FALSE;
        if (!quoted && character == ';') {
            comment = TRUE;
            continue;
        }
        if (character == '\n')
            comment = FALSE;
        else if (comment)
            continue;
        line[length++] = (char)character;
        if (character == '\n')
            break;
    }
    if (length >= LINE_MAX - 1) {
        line[0] = '\0';
        return ERROR_LINE_TOO_LONG;
    }
    line[length] = '\0';
    out->len = 0;
    out->cur = 0;
    if (length && bufferAppend(line, length, out, ss) != 0)
        return ERROR_NO_FREE_STORE;
    *more_left = character != ENDSTREAMCH;
    return 0;
}

BOOL setInteractive(struct CommandLineInterface *cli, ShellState *ss)
{
    (void)ss;
    cli->cli_Interactive = cli->cli_Background ? DOSFALSE : DOSTRUE;
    return cli->cli_Interactive;
}

LONG checkLine(ShellState *ss, Buffer *in, Buffer *out, BOOL echo)
{
    struct CommandLineInterface *cli = Cli();
    BOOL have_command = FALSE;
    BOOL executed = FALSE;
    LONG result = convertLine(ss, in, out, &have_command);

    if (!result && have_command) {
        executed = TRUE;
        if (echo)
            cliEcho(ss, out->buf);
        result = executeLine(ss, out->buf);
        cli->cli_ReturnCode = result;
        cli->cli_Result2 = result ? IoErr() : 0;
    }
    SelectInput(cli->cli_StandardInput);
    SelectOutput(cli->cli_StandardOutput);
    cliVarNum(ss, "RC", cli->cli_ReturnCode);
    cliVarNum(ss, "Result2", cli->cli_Result2);
    if (result && !executed && result != RETURN_FAIL)
        PrintFault(result, NULL);
    return result;
}

LONG interact(ShellState *ss)
{
    struct CommandLineInterface *cli = Cli();
    Buffer in = {0}, out = {0};
    BOOL more_left = FALSE;
    LONG error = bufferAppend("?", 1, &in, ss);

    setInteractive(cli, ss);
    while (!error && !shell_done) {
        Redirection_init(ss);
        cliPrompt(ss);
        bufferReset(&in);
        bufferReset(&out);
        error = readLine(ss, cli, &in, &more_left);
        if (!error && in.len && !line_is_blank(&in))
            (void)checkLine(ss, &in, &out, TRUE);
        Redirection_release(ss);
        if (!more_left)
            break;
    }
    bufferFree(&in, ss);
    bufferFree(&out, ss);
    return error;
}

static int launch_console(const char *argv0)
{
    char executable[PATH_MAX];
    char console_path[PATH_MAX];
    char *slash;
    const char *session = getenv("ACE_SESSION");
    pid_t child;

    if (!session || !*session)
        session = "default";
    if (resolve_executable_path(argv0, executable, sizeof(executable)) != 0) {
        fputs("ace-shell: console unavailable\n", stderr);
        return RETURN_FAIL;
    }
    slash = strrchr(executable, '/');
    if (!slash || slash == executable ||
        (size_t)(slash - executable) >= sizeof(executable)) {
        fputs("ace-shell: console unavailable\n", stderr);
        return RETURN_FAIL;
    }
    *slash = '\0';
    if (snprintf(console_path, sizeof(console_path), "%s/ace-console",
                 executable) >= (int)sizeof(console_path)) {
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

int main(int argc, char **argv)
{
    struct Process *process;
    struct CommandLineInterface *cli;
    ShellState *state;
    struct Library *dos_library;
    char executable[PATH_MAX];

    if (argc == 1)
        return launch_console(argv[0]);
    if (argc != 2 || strcmp(argv[1], "--console-child") != 0) {
        fprintf(stderr, "usage: %s [--console-child]\n", argv[0]);
        return RETURN_FAIL;
    }
    if (resolve_executable_path(argv[0], executable, sizeof(executable)) == 0) {
        char *slash = strrchr(executable, '/');
        if (slash) {
            *slash = '\0';
            snprintf(shell_directory, sizeof(shell_directory), "%s", executable);
        }
    }
    if (native_broker_ensure() != 0) {
        fputs("ace-shell: broker unavailable\n", stderr);
        return RETURN_FAIL;
    }
    /*
     * This process is the shell, so this process's session belongs to it and
     * ends with it. Commands deliberately do not do this: they are separate
     * host processes using a session they do not own, and a session that
     * outlives them is the whole reason the broker exists.
     *
     * A failure here is not fatal. The session simply stays ownerless, which
     * is how every session behaved before, so the shell still runs.
     */
    (void)native_broker_attach();
    dos_library = OpenLibrary("dos.library", 36);
    if (!dos_library)
        return RETURN_FAIL;
    state = AllocMem(sizeof(*state), MEMF_CLEAR);
    process = (struct Process *)FindTask(NULL);
    cli = Cli();
    if (!state || !cli)
        return RETURN_FAIL;
    state->ss_DOSBase = DOSBase;
    state->ss_SysBase = SysBase;
    state->cliNumber = process->pr_TaskNum ? process->pr_TaskNum : 1;
    cli->cli_StandardInput = Input();
    cli->cli_CurrentInput = cli->cli_StandardInput;
    cli->cli_StandardOutput = Output();
    cli->cli_CurrentOutput = cli->cli_StandardOutput;
    cli->cli_Background = DOSFALSE;
    initDefaultInterpreterState(state);
    cliVarNum(state, "process", state->cliNumber);
    if (interact(state) != 0)
        cli->cli_ReturnCode = RETURN_FAIL;
    popInterpreterState(state);
    if (state->arg_rd)
        FreeDosObject(DOS_RDARGS, state->arg_rd);
    FreeMem(state, sizeof(*state));
    CloseLibrary(dos_library);
    return cli->cli_ReturnCode;
}
