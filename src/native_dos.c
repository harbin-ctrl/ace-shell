#define _GNU_SOURCE

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <sys/socket.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

#include <dirent.h>
#include <fcntl.h>

#include <dos/dos.h>
#include <dos/datetime.h>
#include <dos/dosextens.h>
#include <dos/exall.h>
#include <dos/stdio.h>
#include <dos/var.h>
#include <exec/lists.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <utility/utility.h>
#include "ace_crm_retry.h"
#include "ace_modes.h"
#include "broker_client.h"
#include "native_host.h"
#include "broker_protocol.h"
#include "aros_dos_path.h"
#include "aros_console_editor.h"
#include "aros_exec_runtime.h"
#include "clipboard_bridge.h"
#include "console_channel.h"
#include "console_device.h"
#include "native_console_endpoint.h"

struct CommandLineInterface *Cli(void);

static struct Process native_process;
/*
 * Which struct Process the calling thread is.  NULL means the one this Linux
 * process was started as, which is every thread except those CreateNewProc()
 * made: an AmigaDOS process created with NP_Entry runs in this address space,
 * so a second struct Process has to exist and FindTask(NULL) has to tell them
 * apart.  Thread-local rather than a field, because the whole point is that
 * two threads asking the same question get different answers.
 *
 * Main-thread behaviour is unchanged: the accessor falls back to
 * native_process, so nothing that ran before CreateNewProc() existed can
 * observe the difference.
 */
static _Thread_local struct Process *native_current_process;

struct Process *native_this_process(void)
{
    return native_current_process ? native_current_process : &native_process;
}

void native_set_this_process(struct Process *process)
{
    native_current_process = process;
}
/* IoErr() is the process's pr_Result2, not a variable beside it. The AROS
   DOS sources ACE compiles set it that way -- rom/dos/readargs.c ends with
   "me->pr_Result2 = error;" and never calls SetIoErr() -- so a separate
   store would leave every ReadArgs() failure reporting no reason at all:
   the command's PrintFault(IoErr(), name) printed nothing and the shell saw
   a bare failure. Keeping one storage location is what makes ACE's stubs and
   upstream's own code agree about the error. */
#define native_ioerr (native_this_process()->pr_Result2)

static struct ExecBase native_exec_base;
static struct UtilityBase native_utility_base;
static struct Library native_iffparse_base;
/* rn_Flags left at 0: '*' is a literal character, not also a wildcard for
   '#?', matching modern AmigaDOS's default (RNF_WILDSTAR off). Read by the
   real pattern-matching engine (rom/dos/patternmatching.c) this seam
   compiles unmodified. */
static struct RootNode native_root_node;
static struct DosLibrary native_dos_base = { &native_root_node };
/* Weak, because an AROS command may define these itself. A command that
   sets SH_GLOBAL_SYSBASE/SH_GLOBAL_DOSBASE before including
   <aros/shcommands.h> -- Dir.c does both -- gets a file-scope definition
   from the macro expansion, which on a real AROS build is the whole
   program's only copy. Here ACE's runtime is linked in beside it, so
   without weak linkage the two collide. Yielding keeps the command's own
   copy authoritative, exactly as AROS intends; ace_shcommand_start() hands
   the entry point the real base to put in it, rather than reading it back
   out of a symbol the command may not have filled in yet. */
struct ExecBase *SysBase __attribute__((weak)) = &native_exec_base;
struct DosLibrary *DOSBase __attribute__((weak)) = &native_dos_base;

struct ExecBase *native_exec_base_pointer(void)
{
    return &native_exec_base;
}
static struct CommandLineInterface native_cli;
static char native_cli_prompt[PATH_MAX];
static char native_cli_set_name[PATH_MAX];
static int native_cli_loaded;
static int native_shell_result_published;
static int native_endcli_requested;
static int native_task_registered;
static int native_task_broker_registered;
static uint64_t native_task_broker_id;
static char native_task_name[64];

#define NATIVE_REMOTE_TASK_LIMIT 32
struct native_remote_task {
    struct Task task;
    uint64_t broker_id;
    char name[64];
};
static struct native_remote_task native_remote_tasks[NATIVE_REMOTE_TASK_LIMIT];

#define NATIVE_LOCAL_VAR_LIMIT 128
#define NATIVE_LOCAL_VAR_NAME 64

static struct LocalVar native_local_vars[NATIVE_LOCAL_VAR_LIMIT];
static char native_local_var_names[NATIVE_LOCAL_VAR_LIMIT][NATIVE_LOCAL_VAR_NAME];
static char native_local_var_values[NATIVE_LOCAL_VAR_LIMIT][AMIGA_BROKER_MAX_PAYLOAD];
static FILE *native_input = NULL;
static FILE *native_output = NULL;
static char native_input_prefix[4096];
static size_t native_input_prefix_length;
static size_t native_input_prefix_position;
static int native_input_prefix_loaded;
static struct ace_aros_console_editor *native_console_editor;
static int native_console_editor_attempted;
static struct ace_console_channel native_current_console_channel;
static struct native_console_endpoint *native_current_console_endpoint;
static struct native_console_endpoint *native_selected_input_endpoint;
static struct native_console_endpoint *native_selected_output_endpoint;
/*
 * A shell started to run one script and then stop is not interactive, even
 * though its standard input is the same kind of handle an interactive one
 * has. Shell.c recomputes cli_Background from IsInteractive() when a script
 * ends, and without this it would decide the nested shell should now start
 * prompting -- on a standard input that belongs to somebody else.
 */
static int native_interactive = 1;
static unsigned char native_editor_line[8194];
static size_t native_editor_line_length;
static size_t native_editor_line_position;
static BPTR native_program_dir;
static char native_program_name[PATH_MAX];

struct native_stdio_handle {
    struct FileHandle amiga;
    FILE *stream;
};

static struct native_stdio_handle native_stdin_handle;
static struct native_stdio_handle native_stdout_handle;
static struct native_stdio_handle native_stderr_handle;
static int native_stdio_initialized;

static void set_native_broker_error(void);
static FILE *selected_output(void);
static void native_forget_script_input(FILE *file);

static void native_task_signal_from_broker(uint32_t signals, void *context)
{
    (void)context;
    /* A host process can have its bootstrap Process state and an implicit
       Exec task state used by unmodified command code.  Its control socket
       represents the host process, so deliver to each local Exec task. */
    ace_aros_runtime_signal_local_tasks(signals);
}

static void native_task_name_init(void)
{
    const char *name;
    char executable[PATH_MAX];
    ssize_t length;

    if (native_task_name[0])
        return;
    length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length > 0) {
        executable[length] = '\0';
        name = strrchr(executable, '/');
        name = name ? name + 1 : executable;
    } else {
        name = "ACE-task";
    }
    strncpy(native_task_name, name, sizeof(native_task_name) - 1);
    native_task_name[sizeof(native_task_name) - 1] = '\0';
    native_process.pr_Task.tc_Node.ln_Name = native_task_name;
}

static void native_unregister_task(void)
{
    if (native_task_registered) {
        ace_aros_runtime_unregister_task(&native_process.pr_Task);
        native_task_registered = 0;
    }
}

static void native_activate_task(void)
{
    /* A CreateNewProc() thread registered its own task and set its own
       identity before calling its entry point, and the state below is this
       Linux process's, not that thread's.  Running it here would publish the
       parent's task as the caller's, which is precisely the question
       FindTask(NULL) is being asked. */
    if (native_current_process)
        return;
    native_task_name_init();
    ace_aros_runtime_set_current_task(&native_process.pr_Task);
    if (!native_task_registered &&
        ace_aros_runtime_register_task(&native_process.pr_Task) == 0) {
        native_task_registered = 1;
        (void)atexit(native_unregister_task);
    }
    if (!native_task_broker_registered && native_broker_ensure() == 0 &&
        native_broker_task_attach(native_task_name,
                                  native_task_signal_from_broker, NULL,
                                  &native_task_broker_id) == 0)
        native_task_broker_registered = 1;
}

static void native_init_stdio_handles(void)
{
    native_activate_task();
    if (native_stdio_initialized)
        return;
    /* RunCommand() injects a command's arguments ahead of the existing
       Input() stream.  Keep the host stream unbuffered so the shell cannot
       read ahead into the next command before a child handles ReadArgs('?').
       The real AROS input handle has the same packet-level property. */
    (void)setvbuf(stdin, NULL, _IONBF, 0);
    native_stdin_handle.amiga.fh_Pos = 0;
    native_stdin_handle.amiga.fh_End = INT_MAX / 2;
    native_stdin_handle.stream = stdin;
    native_stdout_handle.amiga.fh_Pos = 0;
    native_stdout_handle.amiga.fh_End = INT_MAX / 2;
    native_stdout_handle.stream = stdout;
    native_stderr_handle.amiga.fh_Pos = 0;
    native_stderr_handle.amiga.fh_End = INT_MAX / 2;
    native_stderr_handle.stream = stderr;
    ace_console_channel_attach(&native_current_console_channel,
                               fileno(stdin), fileno(stdout));
    native_stdio_initialized = 1;
    native_current_console_endpoint = native_console_endpoint_open(
        &native_current_console_channel);
}

static bool native_input_is_raw(void)
{
    native_init_stdio_handles();
    return ace_console_channel_is_raw(&native_current_console_channel);
}

static void native_load_input_prefix(void)
{
    const char *arguments;

    if (native_input_prefix_loaded)
        return;
    native_input_prefix_loaded = 1;
    arguments = getenv("ACE_COMMAND_ARGUMENTS");
    if (!arguments)
        return;
    /* Set but empty is a command invoked with no arguments at all, and it is
       not the same as unset. AmigaDOS leaves the argument line in the input
       stream for ReadArgs() to read, so a command with no arguments finds an
       empty line there and reports a missing /A argument. Returning here
       instead would leave ReadArgs() reading whatever the process's real
       standard input holds -- in a shell, the next command. */
    native_input_prefix_length = strlen(arguments);
    if (native_input_prefix_length >= sizeof(native_input_prefix) - 1)
        native_input_prefix_length = sizeof(native_input_prefix) - 1;
    memcpy(native_input_prefix, arguments, native_input_prefix_length);
    if (native_input_prefix_length == 0 ||
        native_input_prefix[native_input_prefix_length - 1] != '\n')
        native_input_prefix[native_input_prefix_length++] = '\n';
}

static int native_console_session_enabled(void)
{
    const char *enabled = getenv("ACE_CONSOLE_INTERACTIVE");

    return enabled && strcmp(enabled, "1") == 0;
}

static void native_console_editor_output(void)
{
    unsigned char output[4096];
    size_t length;
    BPTR handle = Output();

    if (!native_console_editor)
        return;
    do {
        length = ace_aros_console_editor_take_output(
            native_console_editor, output, sizeof(output));
        if (length != 0) {
            if (Write(handle, output, (LONG)length) != (LONG)length)
                return;
            /* Keystroke echo has to reach the GUI before the next byte is
               read. The normal shell output path may remain buffered. */
            (void)Flush(handle);
        }
    } while (length != 0);
}

static int native_console_editor_start(void)
{
    if (native_console_editor_attempted)
        return native_console_editor != NULL;
    native_console_editor_attempted = 1;
    if (!native_console_session_enabled())
        return 0;
    native_console_editor = ace_aros_console_editor_open();
    return native_console_editor != NULL;
}

static int native_editor_next_char(FILE *file)
{
    unsigned char input[32];
    size_t input_length;
    int character;

    if (native_editor_line_position < native_editor_line_length)
        return native_editor_line[native_editor_line_position++];
    native_editor_line_position = 0;
    native_editor_line_length = 0;
    if (!native_console_editor_start())
        return fgetc(file);

    for (;;) {
        character = fgetc(file);
        if (character == EOF)
            return EOF;
        input[0] = (unsigned char)character;
        input_length = 1;
        /* support.c's CSI matcher receives a buffer, not a stream: give it
           the complete key sequence so an arrow is interpreted as history
           navigation rather than as an unknown CSI followed by a literal
           'A'. The AROS console sequences all terminate in a final letter or
           '~'; the bound keeps malformed input from consuming indefinitely. */
        if (input[0] == 0x9b) {
            while (input_length < sizeof(input)) {
                character = fgetc(file);
                if (character == EOF)
                    break;
                input[input_length++] = (unsigned char)character;
                if ((character >= 'A' && character <= 'Z') ||
                    (character >= 'a' && character <= 'z') ||
                    character == '~' || character == '@')
                    break;
            }
        }
        if (ace_aros_console_editor_feed(native_console_editor, input,
                                          input_length) != 0)
            return input[0];
        native_console_editor_output();
        native_editor_line_length = ace_aros_console_editor_take_line(
            native_console_editor, native_editor_line,
            sizeof(native_editor_line));
        if (native_editor_line_length != 0)
            return native_editor_line[native_editor_line_position++];
    }
}

static int native_input_getc(FILE *file)
{
    native_load_input_prefix();
    /* ACE_COMMAND_ARGUMENTS supplies the command line that AmigaDOS
       ReadArgs() expects to find on a child CLI's cooked Input() stream.
       A raw terminal program such as unchanged Vim must see the real
       console bytes immediately; otherwise the synthetic empty argument
       line's newline is consumed as Vim's first terminal response byte. */
    if (file == stdin && !native_input_is_raw() && native_input_prefix_position <
        native_input_prefix_length)
        return (unsigned char)native_input_prefix[
            native_input_prefix_position++];
    if (file == stdin && !native_input_is_raw())
        return native_editor_next_char(file);
    return fgetc(file);
}

static void native_refresh_local_vars(void)
{
    char listing[AMIGA_BROKER_MAX_PAYLOAD];
    char *line;
    size_t count = 0;

    NEWLIST(&native_process.pr_LocalVars);
    memset(native_local_vars, 0, sizeof(native_local_vars));
    memset(native_local_var_names, 0, sizeof(native_local_var_names));
    memset(native_local_var_values, 0, sizeof(native_local_var_values));
    if (native_broker_listvars(AMIGA_BROKER_VAR_LOCAL |
                               AMIGA_BROKER_VAR_ANY,
                               listing, sizeof(listing)) != 0)
        return;

    line = listing;
    while (*line && count < NATIVE_LOCAL_VAR_LIMIT) {
        char *tab = strchr(line, '\t');
        char *end = strchr(line, '\n');
        size_t name_length;
        struct LocalVar *variable;

        if (!tab || !end || tab > end)
            break;
        name_length = (size_t)(end - tab - 1);
        if (name_length == 0 || name_length >= NATIVE_LOCAL_VAR_NAME) {
            line = end + 1;
            continue;
        }
        memcpy(native_local_var_names[count], tab + 1, name_length);
        native_local_var_names[count][name_length] = '\0';
        variable = &native_local_vars[count++];
        variable->lv_Node.ln_Type = (line[0] == '1') ? LV_ALIAS : LV_VAR;
        variable->lv_Node.ln_Name = native_local_var_names[count - 1];
        if (native_broker_getvar(variable->lv_Node.ln_Name,
                                 AMIGA_BROKER_VAR_LOCAL |
                                 (variable->lv_Node.ln_Type == LV_ALIAS ?
                                  AMIGA_BROKER_VAR_ALIAS :
                                  AMIGA_BROKER_VAR_VARIABLE),
                                 native_local_var_values[count - 1],
                                 sizeof(native_local_var_values[count - 1])) == 0) {
            variable->lv_Value = (UBYTE *)native_local_var_values[count - 1];
            variable->lv_Len = strlen((char *)variable->lv_Value);
        }
        variable->lv_Node.ln_Succ = (struct Node *)&native_process.pr_LocalVars.lh_Tail;
        variable->lv_Node.ln_Pred = native_process.pr_LocalVars.lh_TailPred;
        native_process.pr_LocalVars.lh_TailPred->ln_Succ = &variable->lv_Node;
        native_process.pr_LocalVars.lh_TailPred = &variable->lv_Node;
        line = end + 1;
    }
}

struct native_lock {
    /* Keep the public prefix compatible with AROS's FileLock.  The imported
       GetDeviceProc() code reads fl_Task when it advances a multi-assign;
       the host bridge uses the lock pointer itself as its handler marker. */
    BPTR fl_Link;
    IPTR fl_Key;
    LONG fl_Access;
    struct MsgPort *fl_Task;
    BPTR fl_Volume;
    char path[PATH_MAX];
    /* Opened by Examine() when the lock is a directory; consumed by
       ExNext(). Real AmigaDOS ties this scan position to the filesystem
       handler process behind the lock (fl_Task) -- ACE's Lock() has no
       handler process at all (see the OpenDevice("console.device",...)-
       style comment on Examine() below), so the scan lives directly on the
       lock instead. */
    DIR *scan;
    /* ExAll() uses the FileInfoBlock disk key as a progress marker between
       calls.  The host scan itself stays open across those calls, so this
       only needs to be a non-zero, monotonically increasing token. */
    IPTR scan_key;
    /* telldir() position immediately before the readdir() call that
       produced the entry currently reflected by scan_key.  AROS's ExAll()
       (rom/dos/exall.c) rolls fib_DiskKey back by one entry when its
       buffer fills up mid-scan, then calls ExNext() again on the same fib,
       expecting that same entry back so it can retry it in the next
       batch.  A real filesystem handler can reconstruct any entry from its
       disk key directly; readdir() is forward-only, so ExNext() detects
       the rollback and seeks back here to replay it. */
    long scan_prev_pos;
};

/* CurrentDir() in AmigaDOS only swaps the process's lock pointer; it does not
   manufacture a new lock for every query.  Keep the initial process lock
   separately from caller-owned Lock()/DupLock() results.  This matters to
   the real AROS MatchNext() implementation, which saves and restores the
   current directory on every iteration without freeing either returned
   value. */
static struct native_lock native_initial_dir;
static BPTR native_current_dir;

static void native_init_lock(struct native_lock *lock, const char *path,
                              LONG access)
{
    memset(lock, 0, sizeof(*lock));
    snprintf(lock->path, sizeof(lock->path), "%s", path);
    lock->fl_Access = access;
    lock->fl_Task = (struct MsgPort *)lock;
}

/* Commands run as separate host processes, but ACE deliberately gives them
   one broker session so an Amiga command such as CD can update the shell's
   directory.  The shell process therefore has to refresh its cached native
   lock before AROS code uses it.  Without this, Shell.c's command loader
   restores the shell's pre-CD lock after every command and silently undoes
   the child's successful CurrentDir(). */
static int native_sync_current_dir(void)
{
    char current[PATH_MAX];
    struct native_lock *current_lock = native_current_dir;

    if (native_broker_getcwd(current, sizeof(current)) != 0)
        return -1;
    if (!native_current_dir) {
        native_init_lock(&native_initial_dir, current, SHARED_LOCK);
        native_current_dir = &native_initial_dir;
        return 0;
    }
    if (strcmp(current_lock->path, current) != 0) {
        if (current_lock->scan) {
            closedir(current_lock->scan);
            current_lock->scan = NULL;
        }
        current_lock->scan_key = 0;
        snprintf(current_lock->path, sizeof(current_lock->path),
                 "%s", current);
    }
    return 0;
}

/* Unix epoch (1970-01-01) to Amiga epoch (1978-01-01): 2922 days, including
   the two leap years (1972, 1976) in between. Amiga DateStamp cannot
   represent an earlier date, so those clamp to the epoch itself. */
#define NATIVE_AMIGA_EPOCH_DAYS 2922

static void native_datestamp_from_unix(time_t when, struct DateStamp *stamp)
{
    long long days = (long long)when / 86400 - NATIVE_AMIGA_EPOCH_DAYS;
    long long seconds_of_day = (long long)when % 86400;

    if (seconds_of_day < 0) {
        seconds_of_day += 86400;
        days--;
    }
    if (days < 0) {
        days = 0;
        seconds_of_day = 0;
    }
    stamp->ds_Days = (LONG)days;
    stamp->ds_Minute = (LONG)(seconds_of_day / 60);
    stamp->ds_Tick = (LONG)((seconds_of_day % 60) * 50);
}

static int native_unix_from_datestamp(const struct DateStamp *stamp,
                                      time_t *result)
{
    int64_t seconds;

    if (!stamp || stamp->ds_Days < 0 || stamp->ds_Minute < 0 ||
        stamp->ds_Minute >= 24 * 60 || stamp->ds_Tick < 0 ||
        stamp->ds_Tick >= 60 * 50)
        return -1;
    seconds = ((int64_t)stamp->ds_Days + NATIVE_AMIGA_EPOCH_DAYS) * 86400;
    seconds += (int64_t)stamp->ds_Minute * 60;
    seconds += stamp->ds_Tick / 50;
    *result = (time_t)seconds;
    return (int64_t)*result == seconds ? 0 : -1;
}

struct DateStamp *DateStamp(struct DateStamp *date)
{
    if (date)
        native_datestamp_from_unix(time(NULL), date);
    return date;
}

BOOL SetFileDate(CONST_STRPTR name, const struct DateStamp *date)
{
    char resolved[PATH_MAX];
    struct stat information;
    struct utimbuf times;

    if (!name || !date) {
        errno = EINVAL;
        set_native_broker_error();
        return DOSFALSE;
    }
    if (native_unix_from_datestamp(date, &times.modtime) != 0) {
        errno = EINVAL;
        set_native_broker_error();
        return DOSFALSE;
    }
    if (native_broker_resolve_path(name, resolved, sizeof(resolved)) != 0 ||
        ace_crm_retry_stat(resolved, &information, 1) != 0) {
        set_native_broker_error();
        return DOSFALSE;
    }
    times.actime = information.st_atime;
    if (ace_crm_retry_utime(resolved, &times) != 0) {
        set_native_broker_error();
        return DOSFALSE;
    }
    native_ioerr = 0;
    return DOSTRUE;
}

BOOL DateToStr(struct DateTime *datetime)
{
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    static const char *const weekdays[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"
    };
    time_t seconds;
    struct tm broken_down;

    if (!datetime || native_unix_from_datestamp(&datetime->dat_Stamp,
                                                &seconds) != 0 ||
        gmtime_r(&seconds, &broken_down) == NULL)
        return DOSFALSE;
    if (datetime->dat_StrDay)
        strcpy((char *)datetime->dat_StrDay, weekdays[broken_down.tm_wday]);
    if (datetime->dat_StrDate) {
        switch (datetime->dat_Format) {
        case FORMAT_INT:
            sprintf((char *)datetime->dat_StrDate,
                    "%02d-%s-%02d", (broken_down.tm_year + 1900) % 100,
                    months[broken_down.tm_mon], broken_down.tm_mday);
            break;
        case FORMAT_USA:
            sprintf((char *)datetime->dat_StrDate,
                    "%02d-%02d-%02d", broken_down.tm_mon + 1,
                    broken_down.tm_mday, (broken_down.tm_year + 1900) % 100);
            break;
        case FORMAT_CDN:
            sprintf((char *)datetime->dat_StrDate,
                    "%02d-%02d-%02d", broken_down.tm_mday,
                    broken_down.tm_mon + 1, (broken_down.tm_year + 1900) % 100);
            break;
        default:
            sprintf((char *)datetime->dat_StrDate,
                    "%02d-%s-%02d", broken_down.tm_mday,
                    months[broken_down.tm_mon], (broken_down.tm_year + 1900) % 100);
            break;
        }
    }
    if (datetime->dat_StrTime)
        sprintf((char *)datetime->dat_StrTime, "%02d:%02d:%02d",
                broken_down.tm_hour, broken_down.tm_min,
                (datetime->dat_Stamp.ds_Tick / 50) % 60);
    return DOSTRUE;
}

LONG CompareDates(const struct DateStamp *first, const struct DateStamp *second)
{
    if (first->ds_Days != second->ds_Days)
        return first->ds_Days < second->ds_Days ? -1 : 1;
    if (first->ds_Minute != second->ds_Minute)
        return first->ds_Minute < second->ds_Minute ? -1 : 1;
    if (first->ds_Tick != second->ds_Tick)
        return first->ds_Tick < second->ds_Tick ? -1 : 1;
    return 0;
}

BOOL StrToDate(struct DateTime *datetime)
{
    struct tm broken_down = { 0 };
    time_t seconds;
    char *end;
    const char *date_format;

    if (!datetime || !datetime->dat_StrDate)
        return DOSFALSE;
    date_format = datetime->dat_Format == FORMAT_INT ? "%y-%b-%d" :
                  datetime->dat_Format == FORMAT_USA ? "%m-%d-%y" :
                  datetime->dat_Format == FORMAT_CDN ? "%d-%m-%y" :
                  "%d-%b-%y";
    end = strptime((const char *)datetime->dat_StrDate, date_format,
                   &broken_down);
    if (!end || *end != '\0')
        return DOSFALSE;
    if (datetime->dat_StrTime) {
        end = strptime((const char *)datetime->dat_StrTime, "%H:%M:%S",
                       &broken_down);
        if (!end || *end != '\0')
            return DOSFALSE;
    }
    broken_down.tm_isdst = -1;
    seconds = timegm(&broken_down);
    if (seconds == (time_t)-1 || seconds < (time_t)0)
        return DOSFALSE;
    native_datestamp_from_unix(seconds, &datetime->dat_Stamp);
    return DOSTRUE;
}

/* FIBF_{READ,WRITE,EXECUTE,DELETE} are historically inverted: the bit is
   SET to mean the permission is DENIED. A Unix file that is owner-writable
   and owner-executable therefore maps to protection 0 (everything
   permitted); denied permissions set their bit. */
static LONG native_protection_from_stat(const struct stat *information)
{
    LONG protection = 0;

    if (!(information->st_mode & S_IRUSR))
        protection |= FIBF_READ;
    if (!(information->st_mode & S_IWUSR))
        protection |= FIBF_WRITE | FIBF_DELETE;
    if (!(information->st_mode & S_IXUSR))
        protection |= FIBF_EXECUTE;
    return protection;
}

/* The inverse, for SetProtection(). AmigaDOS's delete bit has no separate
   Unix permission -- on Unix it is the containing directory that governs
   removal -- so it shares the owner write bit with FIBF_WRITE, which is the
   pairing the read direction above already fixed. Either bit therefore
   withdraws write permission, and Delete FORCE (which clears the whole
   protection mask before unlinking) restores it. Bits ACE does not model,
   including the archive/pure/script trio, are left alone rather than
   guessed at. */
static mode_t native_mode_from_protection(mode_t mode, LONG protection)
{
    mode = (protection & FIBF_READ) ? (mode & ~(mode_t)S_IRUSR) :
                                      (mode | S_IRUSR);
    mode = (protection & (FIBF_WRITE | FIBF_DELETE)) ?
           (mode & ~(mode_t)S_IWUSR) : (mode | S_IWUSR);
    mode = (protection & FIBF_EXECUTE) ? (mode & ~(mode_t)S_IXUSR) :
                                         (mode | S_IXUSR);
    return mode;
}

/* An AmigaDOS file comment has no Unix permission or timestamp to live in,
   so it is kept in an extended attribute on the file itself. That puts it on
   the inode, which is what makes it survive a rename or a move within a
   filesystem without ACE doing anything.

   The name is deliberately generic, and neither half of that is an
   oversight. It is not ACE's, because a comment on a file is not an ACE
   concept -- ACE is only what happens to be writing this one, and anything
   else that wants to read or write one should not have to know that. It
   does not carry a vendor's name either, because unlike a source filename
   an extended attribute propagates: onto the user's own files, and through
   every backup and copy that preserves xattrs. Everything ACE puts on disk
   is named for ACE or for nothing -- ace.conf, ace-broker.sock -- and this
   is the "for nothing" case.

   The cost of a shared name is that a foreign writer is not bound by
   AmigaDOS's 79-character limit, which is why the read path truncates
   instead of trusting what it finds. */
#define NATIVE_COMMENT_ATTRIBUTE "user.comment"

/* Reads a file comment into a fib, which was zeroed by the caller: no
   attribute, an empty one, or a filesystem with no extended attributes at
   all all leave the fib's empty string in place, since none of those is an
   error in an object that simply has no comment. */
static void native_fill_fib_comment(const char *path,
                                    struct FileInfoBlock *fib)
{
    char stored[1024];
    ssize_t size = getxattr(path, NATIVE_COMMENT_ATTRIBUTE, stored,
                            sizeof(stored));

    if (size <= 0)
        return;
    if ((size_t)size >= sizeof(fib->fib_Comment))
        size = (ssize_t)sizeof(fib->fib_Comment) - 1;
    memcpy(fib->fib_Comment, stored, (size_t)size);
    fib->fib_Comment[size] = '\0';
}

/* Fills fib for a resolved host path already known to exist, shared by
   Examine() (the locked object itself) and ExNext() (its next directory
   child). name overrides the basename ACE would otherwise take from path,
   since a directory child's fib_FileName is the entry's own name, not the
   parent directory's.

   follow says which of the two objects at a symbolic link is being
   described. A directory scan reports the link itself (follow == 0), so a
   listing still shows a softlink as a softlink, and shows one whose target
   has gone away at all. A lock describes what Lock() actually resolved
   (follow != 0), which followed the link -- so Examine() on it reports the
   target's type, the way AmigaDOS does. That distinction is what keeps a
   drawer of symlinked commands from listing as drawers: SYS:C is exactly
   that, and Dir.c re-Lock()s each softlink to ask what it really is. */
static int native_fill_fib(const char *path, const char *name, int follow,
                           struct FileInfoBlock *fib)
{
    struct stat information;
    const char *fib_name = name;
    char mapped_path[PATH_MAX];

    /* A Linux symbolic link maps directly to AmigaDOS's ST_SOFTLINK. */
    if (ace_crm_retry_stat(path, &information, follow) != 0) {
        native_ioerr = errno;
        return -1;
    }
    /* The broker also maps ordinary-looking names that collide under
       AmigaDOS case folding, so every directory entry goes through it. */
    {
        const char *last;

        if (native_broker_name_from_host(path, mapped_path,
                                         sizeof(mapped_path)) != 0) {
            /* Translated rather than passed through: a name with no AmigaDOS
               spelling reaches the user as ERROR_INVALID_COMPONENT_NAME,
               which is what it is, instead of a raw Linux errno printed as
               "Error 36". */
            set_native_broker_error();
            return -1;
        }
        /*
         * What comes back is volume-qualified -- "rootfs:home/pi/x" -- and
         * what belongs in a FileInfoBlock is the entry's own name.  The colon
         * separates the volume from the path exactly as a slash separates one
         * component from the next, so both are separators when looking for
         * the last component; taking only the slash left every entry that had
         * no slash after the colon wearing a volume name.
         *
         * Two of those, and the second is the one that matters:
         *
         *   - an entry directly inside a volume root listed as "RAM:dbus",
         *     because "RAM:dbus" has no slash to split on;
         *   - an entry with a filesystem mounted on it listed as that
         *     filesystem, "DEVTMPFS:", because asking which volume owns a
         *     mountpoint's path answers with the mounted one.
         *
         * The second answer is a mountpoint wearing a volume's name, and
         * AmigaDOS has no word for a mountpoint at all.  A volume is one
         * filesystem and every name on it belongs to that filesystem: a
         * directory with something mounted over it is just a directory on the
         * volume that holds it, named by the entry that volume actually has.
         * That name is what the caller already knows and passes in -- from
         * readdir() for a scan, from the path for a lock -- and it is the only
         * one that is a fact about this volume rather than about another one.
         * Whatever is mounted there is a separate volume, reached by its own
         * name and by no other route.
         */
        last = strrchr(mapped_path, '/');
        if (!last)
            last = strchr(mapped_path, ':');
        fib_name = last ? last + 1 : mapped_path;
        if (!*fib_name && name && *name)
            fib_name = name;
        if (!*fib_name) {
            /* The volume's own root.  Examine names it by the volume, without
               the colon that would make it a path rather than a name. */
            char *colon = strchr(mapped_path, ':');

            if (colon)
                *colon = '\0';
            fib_name = mapped_path;
        }
    }
    memset(fib, 0, sizeof(*fib));
    /*
     * A pipe is not foreign: ST_PIPEFILE is AmigaDOS's own word for one, and
     * saying so costs nothing, because the convention every reader follows is
     * that a negative type is a file of some kind.  A program that knows the
     * type learns something true; one that does not still sees a file.
     *
     * Character and block devices and Unix sockets get no such answer,
     * because AmigaDOS has none to give.  It has devices in plenty -- NIL:,
     * SER:, PRT:, CON: -- but a device there is an entry in the DOS device
     * list, opened by its own name, never a name sitting inside a drawer.  A
     * node is a filesystem entry impersonating a device, which is a category
     * AmigaDOS does not have, so it is listed as the file it superficially
     * resembles and refused when something tries to open it.  See Open().
     */
    fib->fib_DirEntryType = S_ISLNK(information.st_mode) ? ST_SOFTLINK :
                            S_ISDIR(information.st_mode) ? ST_USERDIR :
                            S_ISFIFO(information.st_mode) ? ST_PIPEFILE :
                            ST_FILE;
    fib->fib_EntryType = fib->fib_DirEntryType;
    if (strlen(fib_name) >= sizeof(fib->fib_FileName)) {
        native_ioerr = ERROR_LINE_TOO_LONG;
        return -1;
    }
    strcpy((char *)fib->fib_FileName, fib_name);
    fib->fib_Protection = native_protection_from_stat(&information);
    fib->fib_Size = (LONG)information.st_size;
    fib->fib_NumBlocks = (LONG)information.st_blocks;
    native_datestamp_from_unix(information.st_mtime, &fib->fib_Date);
    native_fill_fib_comment(path, fib);
    /* A successful DOS lookup replaces any error left by an earlier
       operation.  Shell.c uses IoErr() after its failed command lookup when
       it falls back to treating the command text as a directory name. */
    native_ioerr = 0;
    return 0;
}

struct native_console_handle {
    uint64_t magic;
    FILE *input;
    FILE *output;
    struct ace_console_channel *channel;
    struct native_console_endpoint *endpoint;
    struct native_console_instance *instance;
    char specification[PATH_MAX];
};

/* A parameterised CON: name owns a different byte stream from the CLI that
 * opened it.  The GUI process owns one end of this socket; the DOS handle
 * owns the other, and NEWCLI can duplicate that end into the shell it starts.
 * CONSOLE: and * deliberately have no instance and continue to name the
 * caller's current console. */
struct native_console_instance {
    struct ace_console_channel channel;
    int fd;
    pid_t window_pid;
    char session[128];
};

#define NATIVE_CONSOLE_MAGIC UINT64_C(0x414345434f4e3031)

static struct native_console_handle *
native_console_pointer(BPTR handle)
{
    struct native_console_handle *candidate = handle;

    /* These are ordinary stdio handles, not heap-allocated CON: objects.
       Check them before looking at the larger console structure so optimized
       builds cannot treat a small stdio wrapper as a native console handle. */
    if (handle == (BPTR)stdin || handle == (BPTR)stdout ||
        handle == (BPTR)stderr ||
        handle == (BPTR)&native_stdin_handle.amiga ||
        handle == (BPTR)&native_stdout_handle.amiga ||
        handle == (BPTR)&native_stderr_handle.amiga)
        return NULL;
    return candidate && candidate->magic == NATIVE_CONSOLE_MAGIC
        ? candidate : NULL;
}

static int native_console_current_specification(const char *specification)
{
    return specification &&
        (strcasecmp(specification, "CONSOLE:") == 0 ||
         strcmp(specification, "*") == 0);
}

static int native_console_executable_directory(char *directory,
                                                size_t directory_size)
{
    char executable[PATH_MAX];
    char *slash;
    ssize_t length;

    length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0 || (size_t)length >= sizeof(executable) - 1)
        return -1;
    executable[length] = '\0';
    slash = strrchr(executable, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    if (strlen(executable) >= directory_size)
        return -1;
    strcpy(directory, executable);
    return 0;
}

static int native_console_child_session(char *session, size_t session_size)
{
    const char *parent = getenv("ACE_SESSION");
    struct timespec now;

    if (!parent || !*parent)
        parent = "default";
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return snprintf(session, session_size, "%s-con-%ld-%ld", parent,
                    (long)getpid(), (long)now.tv_nsec) >= (int)session_size
        ? -1 : 0;
}

static struct native_console_instance *
native_console_create_instance(const char *specification)
{
    struct native_console_instance *instance;
    char directory[PATH_MAX];
    char console_path[PATH_MAX];
    char descriptor[32];
    int sockets[2];
    pid_t child;

    if (native_console_executable_directory(directory, sizeof(directory)) != 0 ||
        snprintf(console_path, sizeof(console_path), "%s/ace-console",
                 directory) >= (int)sizeof(console_path) ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        native_ioerr = errno ? errno : ERROR_OBJECT_NOT_FOUND;
        return NULL;
    }
    instance = calloc(1, sizeof(*instance));
    if (!instance) {
        close(sockets[0]);
        close(sockets[1]);
        native_ioerr = ERROR_NO_FREE_STORE;
        return NULL;
    }
    if (native_console_child_session(instance->session,
                                     sizeof(instance->session)) != 0 ||
        native_broker_ensure() != 0 ||
        native_broker_clone_session(instance->session) != 0) {
        close(sockets[0]);
        close(sockets[1]);
        free(instance);
        native_ioerr = ERROR_OBJECT_NOT_FOUND;
        return NULL;
    }
    child = fork();
    if (child < 0) {
        close(sockets[0]);
        close(sockets[1]);
        free(instance);
        native_ioerr = ERROR_NO_FREE_STORE;
        return NULL;
    }
    if (child == 0) {
        close(sockets[0]);
        if (dup2(sockets[1], 3) < 0)
            _exit(RETURN_FAIL);
        if (sockets[1] != 3)
            close(sockets[1]);
        snprintf(descriptor, sizeof(descriptor), "%d", 3);
        execl(console_path, console_path, "--session", instance->session,
              "--fd", descriptor, "--spec", specification, (char *)NULL);
        _exit(RETURN_FAIL);
    }
    close(sockets[1]);
    instance->fd = sockets[0];
    instance->window_pid = child;
    (void)fcntl(instance->fd, F_SETFD, FD_CLOEXEC);
    ace_console_channel_attach(&instance->channel, instance->fd, instance->fd);
    return instance;
}

BPTR native_console_open(const char *specification)
{
    struct native_console_handle *handle;
    struct native_console_instance *instance = NULL;
    int input_fd = -1;
    int output_fd = -1;

    native_init_stdio_handles();
    handle = calloc(1, sizeof(*handle));
    if (!handle) {
        native_ioerr = ERROR_NO_FREE_STORE;
        return BNULL;
    }
    handle->magic = NATIVE_CONSOLE_MAGIC;
    snprintf(handle->specification, sizeof(handle->specification), "%s",
             specification ? specification : "CON:");
    if (native_console_current_specification(handle->specification)) {
        handle->input = stdin;
        handle->output = stdout;
        handle->channel = &native_current_console_channel;
        handle->endpoint = native_current_console_endpoint;
        return handle;
    }
    instance = native_console_create_instance(handle->specification);
    if (!instance) {
        free(handle);
        return BNULL;
    }
    handle->instance = instance;
    handle->channel = &instance->channel;
    handle->endpoint = native_console_endpoint_open(handle->channel);
    if (!handle->endpoint) {
        close(instance->fd);
        ace_console_channel_close(&instance->channel);
        free(instance);
        free(handle);
        native_ioerr = ERROR_NO_FREE_STORE;
        return BNULL;
    }
    /* Each descriptor is held until fdopen() has taken it: fdopen() adopts
       the descriptor only when it succeeds, so a failure leaves the dup to
       be closed here rather than leaked. */
    input_fd = dup(instance->fd);
    output_fd = dup(instance->fd);
    handle->input = input_fd >= 0 ? fdopen(input_fd, "rb") : NULL;
    if (handle->input)
        input_fd = -1;
    handle->output = output_fd >= 0 ? fdopen(output_fd, "wb") : NULL;
    if (handle->output)
        output_fd = -1;
    if (!handle->input || !handle->output) {
        if (input_fd >= 0)
            close(input_fd);
        if (output_fd >= 0)
            close(output_fd);
        if (handle->input)
            fclose(handle->input);
        if (handle->output)
            fclose(handle->output);
        native_console_endpoint_close(handle->endpoint);
        close(instance->fd);
        ace_console_channel_close(&instance->channel);
        free(instance);
        free(handle);
        native_ioerr = errno;
        return BNULL;
    }
    (void)setvbuf(handle->input, NULL, _IONBF, 0);
    (void)setvbuf(handle->output, NULL, _IONBF, 0);
    return handle;
}

int native_console_is_handle(BPTR handle)
{
    return native_console_pointer(handle) != NULL;
}

int native_console_is_instance(BPTR handle)
{
    struct native_console_handle *console = native_console_pointer(handle);

    return console && console->instance != NULL;
}

const char *native_console_specification(BPTR handle)
{
    struct native_console_handle *console = native_console_pointer(handle);

    return console ? console->specification : NULL;
}

const char *native_console_session(BPTR handle)
{
    struct native_console_handle *console = native_console_pointer(handle);

    return console && console->instance ? console->instance->session : NULL;
}

static int native_console_duplicate(BPTR handle, int output)
{
    struct native_console_handle *console = native_console_pointer(handle);

    if (!console) {
        errno = EBADF;
        return -1;
    }
    return dup(fileno(output ? console->output : console->input));
}

int native_console_dup_input(BPTR handle)
{
    return native_console_duplicate(handle, 0);
}

int native_console_dup_output(BPTR handle)
{
    return native_console_duplicate(handle, 1);
}

void native_console_close(BPTR handle)
{
    struct native_console_handle *console = native_console_pointer(handle);

    if (!console)
        return;
    if (console->instance) {
        struct native_console_instance *instance = console->instance;

        fclose(console->input);
        fclose(console->output);
        native_console_endpoint_close(console->endpoint);
        close(instance->fd);
        ace_console_channel_close(&instance->channel);
        free(instance);
    }
    free(console);
}

static struct ace_console_channel *native_channel_for_handle(BPTR handle)
{
    struct native_console_handle *console = native_console_pointer(handle);

    native_init_stdio_handles();
    if (console)
        return console->channel;
    if (handle == (BPTR)stdin || handle == (BPTR)stdout ||
        handle == (BPTR)&native_stdin_handle.amiga ||
        handle == (BPTR)&native_stdout_handle.amiga)
        return &native_current_console_channel;
    return NULL;
}

static struct native_console_endpoint *native_endpoint_for_handle(BPTR handle)
{
    struct native_console_handle *console = native_console_pointer(handle);

    native_init_stdio_handles();
    if (console)
        return console->endpoint;
    if (handle == (BPTR)stdin || handle == (BPTR)stdout ||
        handle == (BPTR)stderr || handle == (BPTR)&native_stdin_handle.amiga ||
        handle == (BPTR)&native_stdout_handle.amiga ||
        handle == (BPTR)&native_stderr_handle.amiga)
        return native_current_console_endpoint;
    if (native_selected_input_endpoint &&
        native_input && handle == (BPTR)native_input)
        return native_selected_input_endpoint;
    if (native_selected_output_endpoint &&
        native_output && handle == (BPTR)native_output)
        return native_selected_output_endpoint;
    return NULL;
}

int native_console_same_endpoint(BPTR first, BPTR second)
{
    struct native_console_endpoint *first_endpoint =
        native_endpoint_for_handle(first);
    struct native_console_endpoint *second_endpoint =
        native_endpoint_for_handle(second);

    return first_endpoint && first_endpoint == second_endpoint;
}

static struct native_console_endpoint *native_endpoint_for_file(FILE *file)
{
    native_init_stdio_handles();
    if (file == stdin || file == stdout || file == stderr)
        return native_current_console_endpoint;
    if (native_selected_input_endpoint && file == native_input)
        return native_selected_input_endpoint;
    if (native_selected_output_endpoint && file == native_output)
        return native_selected_output_endpoint;
    return NULL;
}

int native_console_is_raw_mode(BPTR handle)
{
    struct ace_console_channel *channel = native_channel_for_handle(handle);

    return channel ? ace_console_channel_is_raw(channel) : -1;
}

int native_console_geometry(BPTR handle, int *rows, int *cols)
{
    struct ace_console_channel *channel = native_channel_for_handle(handle);

    if (!channel)
        return -1;
    if (rows)
        *rows = ace_console_channel_rows(channel);
    if (cols)
        *cols = ace_console_channel_cols(channel);
    return 0;
}

unsigned long native_console_resize_generation(BPTR handle)
{
    struct ace_console_channel *channel = native_channel_for_handle(handle);

    return channel ? ace_console_channel_resize_generation(channel) : 0;
}

void native_console_notify_resize(int rows, int cols)
{
    native_init_stdio_handles();
    ace_console_channel_notify_resize(&native_current_console_channel,
                                      rows, cols);
}

int native_console_take_resize(BPTR handle)
{
    struct ace_console_channel *channel = native_channel_for_handle(handle);

    return channel ? ace_console_channel_take_resize(channel) : 0;
}

static FILE *as_file(BPTR handle)
{
    struct native_console_handle *console = native_console_pointer(handle);

    native_init_stdio_handles();
    if (console)
        return console->input;
    if (handle == (BPTR)&native_stdin_handle.amiga)
        return native_stdin_handle.stream;
    if (handle == (BPTR)&native_stdout_handle.amiga)
        return native_stdout_handle.stream;
    if (handle == (BPTR)&native_stderr_handle.amiga)
        return native_stderr_handle.stream;
    return (FILE *)handle;
}

extern struct Library *ace_rexxsyslib_library_base(void) __attribute__((weak));
extern struct Library *ace_rexxsupport_library_base(void) __attribute__((weak));

struct Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    (void)version;
    if (name && strcasecmp(name, "dos.library") == 0) {
        native_dos_base.dl_Root = &native_root_node;
        DOSBase = &native_dos_base;
        return (struct Library *)&native_dos_base;
    }
    if (name && strcasecmp(name, "utility.library") == 0)
        return (struct Library *)&native_utility_base;
    if (name && strcasecmp(name, "iffparse.library") == 0)
        return &native_iffparse_base;
    /*
     * Weak, because src/rexxsyslib.c is linked into the ARexx programs and
     * not into every command, while this function is in all of them. Where it
     * is linked the name resolves to a real base; where it is not, this
     * answers NULL for it as it always did.
     */
    if (name && strcasecmp(name, "rexxsyslib.library") == 0 &&
        ace_rexxsyslib_library_base)
        return ace_rexxsyslib_library_base();
    if (name && strcasecmp(name, "rexxsupport.library") == 0 &&
        ace_rexxsupport_library_base)
        return ace_rexxsupport_library_base();
    return NULL;
}

struct Task *FindTask(CONST_STRPTR name)
{
    struct Task *local;

    SysBase = &native_exec_base;
    native_activate_task();
    native_refresh_local_vars();
    /* Copy.c is an older AROS process entry point. It checks pr_CLI directly
       before it ever calls a DOS routine that would normally initialize the
       CLI, so make the host process look like the CLI-backed process it is. */
    if (!native_cli_loaded)
        (void)Cli();
    if (!name)
        return (struct Task *)native_this_process();
    local = ace_aros_runtime_find_task(name);
    if (local)
        return local;
    {
        uint64_t task_id;

        if (native_broker_task_find(name, &task_id) == 0) {
            for (size_t index = 0; index < NATIVE_REMOTE_TASK_LIMIT; index++) {
                struct native_remote_task *remote = &native_remote_tasks[index];

                if (remote->broker_id == task_id)
                    return &remote->task;
                if (!remote->broker_id) {
                    remote->broker_id = task_id;
                    strncpy(remote->name, name, sizeof(remote->name) - 1);
                    remote->name[sizeof(remote->name) - 1] = '\0';
                    remote->task.tc_Node.ln_Name = remote->name;
                    return &remote->task;
                }
            }
        }
    }
    return NULL;
}

/*
 * Send a message back to whoever sent it, per rom/exec/replymsg.c. This used
 * to do nothing at all, which is invisible until something waits for the
 * reply: an ARexx client PutMsg()s a RexxMsg and then WaitPort()s its reply
 * port, and with no reply ever arriving it waits there for ever.
 *
 * A message with no reply port is not an error -- it is a message nobody is
 * waiting on -- and is marked NT_FREEMSG so its sender can tell it is no
 * longer in flight.
 */
/* See rexx_port_bridge.c. Weak, because most commands have no ARexx in them
   and should not drag it in; when it is absent every message is local, which
   is what it was before. */
extern int ace_rexx_port_reply(struct Message *message) __attribute__((weak));

void ReplyMsg(struct Message *message)
{
    struct MsgPort *port;

    if (!message)
        return;
    /* A message that arrived from another process is answered through the
       broker, not by putting it on a reply port that does not exist here. */
    if (ace_rexx_port_reply && ace_rexx_port_reply(message))
        return;
    port = message->mn_ReplyPort;
    if (!port) {
        message->mn_Node.ln_Type = NT_FREEMSG;
        return;
    }
    message->mn_Node.ln_Type = NT_REPLYMSG;
    PutMsg(port, message);
}

struct LocalVar *FindVar(CONST_STRPTR name, LONG type)
{
    struct Node *node;

    if (!name)
        return NULL;
    native_refresh_local_vars();
    ForeachNode(&native_process.pr_LocalVars, node) {
        struct LocalVar *variable = (struct LocalVar *)node;
        if (strcasecmp(node->ln_Name, name) == 0 &&
            (type == 0 || variable->lv_Node.ln_Type == type))
            return variable;
    }
    return NULL;
}

void CloseLibrary(struct Library *library)
{
    (void)library;
}

/*
 * AROS's own findarg.c and strtolong.c are already compiled into the DOS
 * runtime, under renamed symbols so that readargs.c reaches them without
 * ACE having to publish them. If and Else are the first callers from
 * outside, and they call them by their real names, which is what these are.
 */
LONG ace_aros_FindArg(CONST_STRPTR keywords, CONST_STRPTR argument);
LONG ace_aros_StrToLong(CONST_STRPTR string, LONG *value);

LONG FindArg(CONST_STRPTR keywords, CONST_STRPTR argument)
{
    return ace_aros_FindArg(keywords, argument);
}

LONG StrToLong(CONST_STRPTR string, LONG *value)
{
    return ace_aros_StrToLong(string, value);
}

LONG Stricmp(CONST_STRPTR left, CONST_STRPTR right)
{
    return strcasecmp(left, right);
}

LONG Strnicmp(CONST_STRPTR left, CONST_STRPTR right, LONG length)
{
    return strncasecmp(left, right, (size_t)length);
}

LONG SplitName(CONST_STRPTR path, LONG separator, STRPTR buffer,
               LONG buffer_position, LONG buffer_size)
{
    const char *end;
    size_t length;

    if (!path || !buffer || buffer_position < 0 || buffer_size <= 0)
        return 0;
    end = strchr(path, (int)separator);
    if (!end)
        return 0;
    length = (size_t)(end - path);
    if (length > (size_t)buffer_size - 1)
        length = (size_t)buffer_size - 1;
    memcpy(buffer + buffer_position, path, length);
    buffer[buffer_position + length] = '\0';
    return (LONG)(length + 1);
}

BOOL ErrorReport(LONG error, ULONG type, IPTR object, APTR requester)
{
    (void)type;
    (void)object;
    (void)requester;
    SetIoErr(error);
    /*
     * The return value is which button the user chose, not whether anything
     * was reported: DOSFALSE is "Retry", DOSTRUE is "Cancel". AmigaDOS also
     * specifies DOSTRUE for an error that could not be reported at all,
     * which is permanently ACE's case -- there is no requester at this
     * layer.
     *
     * Every caller of this in rom/dos is a retry loop that only exits on
     * DOSTRUE, so answering "Retry" to an error nothing will ever fix spins
     * forever. getdeviceproc.c's `do { ... } while (dl == NULL)` is the one
     * a bad device name reaches -- "CD A:" burned a core there -- and
     * internalseek.c, startnotify.c and getdeviceproc.c's volume path hang
     * the same way.
     */
    return DOSTRUE;
}

UBYTE ToUpper(ULONG character)
{
    return (UBYTE)toupper((int)character);
}

APTR AllocVec(ULONG size, ULONG flags)
{
    (void)flags;
    return calloc(1, size);
}

APTR AllocMem(ULONG size, ULONG flags)
{
    return (flags & 1u) ? calloc(1, size) : malloc(size);
}

STRPTR StrDup(CONST_STRPTR string)
{
    return string ? strdup(string) : NULL;
}

void __sprintf(UBYTE *buffer, const UBYTE *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsprintf((char *)buffer, (const char *)format, arguments);
    va_end(arguments);
}

void FreeMem(APTR memory, ULONG size)
{
    (void)size;
    free(memory);
}

ULONG AvailMem(ULONG flags)
{
    (void)flags;
    return (ULONG)-1;
}

void FreeVec(APTR memory)
{
    free(memory);
}

void CopyMemQuick(CONST_APTR source, APTR destination, ULONG length)
{
    memmove(destination, source, length);
}

void CopyMem(CONST_APTR source, APTR destination, ULONG length)
{
    memmove(destination, source, length);
}

STRPTR FilePart(CONST_STRPTR path)
{
    const char *cursor;

    if (!path)
        return NULL;
    if (!*path)
        return (STRPTR)path;

    cursor = path + strlen(path) - 1;
    while (*cursor != ':' && *cursor != '/' && cursor != path)
        cursor--;
    if (*cursor == ':' || *cursor == '/')
        cursor++;
    return (STRPTR)cursor;
}

STRPTR PathPart(CONST_STRPTR path)
{
    const char *cursor;

    if (!path)
        return NULL;
    while (*path == '/')
        path++;
    cursor = path;
    while (*cursor) {
        if (*cursor == '/')
            path = cursor;
        else if (*cursor == ':')
            path = cursor + 1;
        cursor++;
    }
    return (STRPTR)path;
}

BOOL AddPart(STRPTR dirname, CONST_STRPTR filename, ULONG size)
{
    char *position;
    size_t dirname_length;
    size_t filename_length;
    int add_slash = 0;

    if (!dirname || !filename || size == 0) {
        native_ioerr = ERROR_LINE_TOO_LONG;
        return DOSFALSE;
    }

    position = strchr(filename, ':');
    if (position) {
        if (position == filename) {
            position = strchr(dirname, ':');
            if (!position)
                position = dirname;
        } else {
            position = dirname;
        }
    } else {
        dirname_length = strlen(dirname);
        position = dirname + dirname_length;
        if (dirname_length > 0 && position[-1] != ':' && position[-1] != '/')
            add_slash = 1;
    }

    dirname_length = (size_t)(position - dirname);
    filename_length = strlen(filename);
    if (dirname_length + filename_length + 1 + (size_t)add_slash > size) {
        native_ioerr = ERROR_LINE_TOO_LONG;
        return DOSFALSE;
    }
    if (add_slash)
        *position++ = '/';
    strcpy(position, filename);
    return DOSTRUE;
}

STRPTR stccpy(STRPTR destination, CONST_STRPTR source, LONG length)
{
    size_t source_length;
    size_t copied;

    if (!destination || !source || length <= 0)
        return destination;
    source_length = strlen(source);
    copied = source_length < (size_t)length - 1 ? source_length :
             (size_t)length - 1;
    memcpy(destination, source, copied);
    destination[copied] = '\0';
    return destination;
}

BPTR AllocDosObject(LONG type, APTR tags)
{
    (void)tags;
    if (type == DOS_FIB)
        return calloc(1, sizeof(struct FileInfoBlock));
    if (type == DOS_RDARGS)
        return calloc(1, sizeof(struct RDArgs));
    if (type == DOS_EXALLCONTROL)
        return calloc(1, sizeof(struct InternalExAllControl));
    return NULL;
}

static void native_free_rdargs_values(struct RDArgs *arguments)
{
    if (!arguments)
        return;
    for (ULONG i = 0; i < arguments->RDA_ArgumentCount && i < 32; i++) {
        if (arguments->RDA_Multiple[i] && arguments->RDA_Values[i]) {
            char **values = arguments->RDA_Values[i];
            for (size_t j = 0; values[j]; j++)
                free(values[j]);
        }
        free(arguments->RDA_Values[i]);
        arguments->RDA_Values[i] = NULL;
        arguments->RDA_Multiple[i] = 0;
    }
    if (arguments->RDA_Arguments) {
        for (ULONG i = 0; i < arguments->RDA_ArgumentCount; i++)
            arguments->RDA_Arguments[i] = 0;
    }
    arguments->RDA_ArgumentCount = 0;
}

void FreeDosObject(LONG type, APTR object)
{
    struct RDArgs *arguments = object;

    if (type == DOS_RDARGS)
        native_free_rdargs_values(arguments);
    if (type == DOS_EXALLCONTROL && object) {
        /* exall.c's real ExAll() emulation allocates this internally (see
           compat/include/dos/exall.h) the first time it is called on a
           lock, and documents it as freed by FreeDosObject(). */
        struct InternalExAllControl *control = object;

        free(control->fib);
    }
    free(object);
}

static int native_named_device_path(CONST_STRPTR name)
{
    const char *colon;

    if (!name || strncasecmp(name, "PROGDIR:", 8) == 0 ||
        strncasecmp(name, "NIL:", 4) == 0)
        return 0;
    colon = strchr(name, ':');
    return colon && colon != name;
}

/* Resolve a name that the user's broker-side path walker cannot enter.
 *
 * The ordinary resolver intentionally walks as the session user, which is
 * the right answer for normal names and for deciding whether a typo exists.
 * Once the shell has deliberately entered a directory such as /root, though,
 * resolving ".bashrc" fails before ace_crm_retry_fopen() gets to try the
 * crm.  The broker already gave us the trusted host spelling of the
 * current directory; combine that with the relative name and let the access
 * seam make the permission decision on the complete object.
 */
static int native_resolve_path_for_access(CONST_STRPTR name, char *result,
                                          size_t result_size)
{
    char current[PATH_MAX];
    int failure;

    if (native_broker_resolve_path(name, result, result_size) == 0)
        return 0;
    failure = errno;
    if (!ace_mode_is_root() || (failure != EACCES && failure != EPERM) ||
        !name || !*name || name[0] == '/' || native_named_device_path(name)) {
        errno = failure;
        return -1;
    }
    if (native_broker_getcwd(current, sizeof(current)) != 0) {
        errno = failure;
        return -1;
    }
    if (snprintf(result, result_size, "%s%s%s", current,
                 current[0] && current[strlen(current) - 1] == '/' ? "" : "/",
                 name) >= (int)result_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/*
 * Resolution by a caller that is about to open the result.
 *
 * Shares the fallback below with the plain form and differs only in telling
 * the broker what it means to do, so that an object which cannot be read as a
 * file is refused before anything tries.
 */
static int native_resolve_path_for_open(CONST_STRPTR name, char *result,
                                        size_t result_size)
{
    if (native_broker_resolve_path_for_open(name, result, result_size) == 0)
        return 0;
    if (errno == ENXIO)
        return -1;
    return native_resolve_path_for_access(name, result, result_size);
}

static int native_device_base(struct DevProc *device, char *result,
                              size_t result_size)
{
    struct native_lock *lock = device ? device->dvp_Lock : NULL;

    if (!lock && device && device->dvp_DevNode)
        lock = device->dvp_DevNode->dol_Lock;
    if (!lock)
        return -1;
    if (snprintf(result, result_size, "%s", lock->path) >=
        (int)result_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int native_device_relative(CONST_STRPTR name, char *result,
                                  size_t result_size)
{
    const char *colon = strchr(name, ':');
    const char *cursor = colon + 1;

    /* Keep AmigaDOS's leading slashes intact. ACE once rewrote each slash as
       "../" before asking the broker to resolve it, which required the
       resolver to invent Linux-style dot syntax for every caller. */
    if (strlen(cursor) >= result_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(result, cursor);
    return 0;
}

static int native_union_candidate(CONST_STRPTR name, struct DevProc *device,
                                  char *result, size_t result_size)
{
    char base[PATH_MAX];
    char relative[PATH_MAX * 2];

    /* CLIPS: presents unit numbers as virtual entries (0..255), while its
       backing directory stores clip0..clip255.  The ordinary assign path
       resolver deliberately does not know that presentation rule when it
       resolves a path beneath an assign, so let the broker handle the
       complete spelling here too. */
    if (name && strncasecmp(name, "CLIPS:", 6) == 0)
        return native_broker_resolve_path(name, result, result_size);

    if (native_device_base(device, base, sizeof(base)) != 0 ||
        native_device_relative(name, relative, sizeof(relative)) != 0)
        return -1;
    {
        return native_broker_resolve_beneath(base, relative, result,
                                             result_size);
    }
}

static int native_union_existing(CONST_STRPTR name, char *result,
                                 size_t result_size)
{
    struct stat direct_information;
    struct DevProc *device = ace_aros_GetDeviceProc(name, NULL);
    /*
     * A NULL on the very first call means no handler claims this name at
     * all -- an unmounted device, or an assign that was never made -- and
     * GetDeviceProc() has already recorded the specific reason, normally
     * ERROR_DEVICE_NOT_MOUNTED. Keep it. The generic not-found below is
     * only right once a handler does exist and it is the object within it
     * that is missing, which is the distinction "CD A:" and "CD DH0:junk"
     * turn on.
     */
    int saved_error = device ? ERROR_OBJECT_NOT_FOUND : (int)IoErr();

    /* The broker already knows how to resolve the complete logical assign
       name.  Use that first for the common single-target case; it avoids
       asking the device-view resolver to re-resolve a host-backed C: assign
       beneath a base path, which is a different operation and can be refused
       in a fresh root session.  If the object is absent, retain the iterator
       below for true multi-assigns. */
    if (native_broker_resolve_path(name, result, result_size) == 0 &&
        ace_crm_retry_stat(result, &direct_information, 1) == 0) {
        if (device)
            ace_aros_FreeDeviceProc(device);
        return 0;
    }

    while (device) {
        struct stat information;

        if (native_union_candidate(name, device, result, result_size) == 0) {
            if (ace_crm_retry_stat(result, &information, 1) == 0) {
                ace_aros_FreeDeviceProc(device);
                return 0;
            }
            saved_error = errno;
            if (saved_error != ENOENT && saved_error != ENOTDIR) {
                ace_aros_FreeDeviceProc(device);
                errno = saved_error;
                return -1;
            }
        } else {
            saved_error = errno;
        }
        device = ace_aros_GetDeviceProc(name, device);
    }
    errno = saved_error;
    return -1;
}

BPTR Lock(CONST_STRPTR name, LONG mode)
{
    char resolved[PATH_MAX];
    struct stat information;
    struct native_lock *lock;

    if ((native_named_device_path(name) ?
         native_union_existing(name, resolved, sizeof(resolved)) :
         native_resolve_path_for_access(name, resolved, sizeof(resolved))) != 0 ||
        ace_crm_retry_stat(resolved, &information, 1) != 0) {
        if (native_named_device_path(name) &&
            (errno == ENOENT || errno == ENOTDIR))
            native_ioerr = ERROR_OBJECT_NOT_FOUND;
        else
            set_native_broker_error();
        return NULL;
    }
    lock = calloc(1, sizeof(*lock));
    if (!lock) {
        native_ioerr = ERROR_NO_FREE_STORE;
        return NULL;
    }
    native_init_lock(lock, resolved, mode);
    /* Match AmigaDOS Lock(): a successful locate has no pending error. */
    native_ioerr = 0;
    return lock;
}

/* The broker stores assignment targets as canonical host paths.  DOS Lock()
   deliberately interprets leading '/' with Amiga parent-directory meaning,
   so the DosList compatibility layer needs this explicit host-path seam when
   it materializes a broker assignment for Assign LIST/EXISTS. */
BPTR native_lock_host_path(const char *path)
{
    struct stat information;
    struct native_lock *lock;

    if (!path || ace_crm_retry_stat(path, &information, 1) != 0) {
        native_ioerr = errno;
        return NULL;
    }
    lock = calloc(1, sizeof(*lock));
    if (!lock) {
        native_ioerr = ERROR_NO_FREE_STORE;
        return NULL;
    }
    native_init_lock(lock, path, SHARED_LOCK);
    native_ioerr = 0;
    return lock;
}

static struct native_lock *native_info_lock(BPTR handle)
{
    if (handle)
        return handle;
    if (!native_current_dir && native_sync_current_dir() != 0)
        return NULL;
    return native_current_dir;
}

static LONG native_info_disk_type(const char *path)
{
    struct statfs filesystem;

    if (statfs(path, &filesystem) != 0)
        return ID_NOT_REALLY_DOS;
    /* Linux filesystem magic values are deliberately kept here rather than
       exposed through the public DOS structure.  The AROS IDs are the useful
       compatibility description for the two families ACE currently mounts. */
    switch ((unsigned long)filesystem.f_type) {
    case 0xEF53: /* ext2/ext3/ext4 */
        return ID_EXT2_DISK;
    case 0x0000EF51: /* old VFAT/MS-DOS magic */
    case 0x4d44:     /* MSDOS filesystem */
        return ID_FAT_DISK;
    default:
        return ID_NOT_REALLY_DOS;
    }
}

static LONG native_fill_info64(BPTR handle, struct InfoData64 *info)
{
    struct native_lock *lock = native_info_lock(handle);
    struct statvfs statistics;
    uint64_t block_size;
    uint64_t free_blocks;

    if (!lock || !info || statvfs(lock->path, &statistics) != 0) {
        native_ioerr = errno ? errno : ERROR_INVALID_LOCK;
        return DOSFALSE;
    }
    memset(info, 0, sizeof(*info));
    block_size = statistics.f_frsize ? statistics.f_frsize :
                 (statistics.f_bsize ? statistics.f_bsize : 1);
    free_blocks = (uint64_t)statistics.f_bfree;
    info->id_NumSoftErrors = 0;
    info->id_UnitNumber = 0;
    info->id_DiskState = (statistics.f_flag & ST_RDONLY) ?
                         ID_WRITE_PROTECTED : ID_VALIDATED;
    info->id_NumBlocks = (uint64_t)statistics.f_blocks;
    info->id_NumBlocksUsed = info->id_NumBlocks >= free_blocks ?
                             info->id_NumBlocks - free_blocks : 0;
    info->id_BytesPerBlock = block_size > INT32_MAX ? INT32_MAX :
                             (LONG)block_size;
    info->id_DiskType = native_info_disk_type(lock->path);
    info->id_VolumeNode = NULL;
    info->id_InUse = 1;
    native_ioerr = 0;
    return DOSTRUE;
}

LONG Info64(BPTR handle, struct InfoData64 *parameter_block)
{
    return native_fill_info64(handle, parameter_block);
}

LONG Info(BPTR handle, struct InfoData *parameter_block)
{
    struct InfoData64 wide;

    if (!parameter_block) {
        native_ioerr = ERROR_INVALID_LOCK;
        return DOSFALSE;
    }
    if (native_fill_info64(handle, &wide) == DOSFALSE)
        return DOSFALSE;
    parameter_block->id_NumSoftErrors = wide.id_NumSoftErrors;
    parameter_block->id_UnitNumber = wide.id_UnitNumber;
    parameter_block->id_DiskState = wide.id_DiskState;
    /* This is the same narrowing AROS performs when Info64() falls back to
       a classic handler: preserve the low 32 bits for AmigaDOS callers. */
    parameter_block->id_NumBlocks = (LONG)(ULONG)wide.id_NumBlocks;
    parameter_block->id_NumBlocksUsed = (LONG)(ULONG)wide.id_NumBlocksUsed;
    parameter_block->id_BytesPerBlock = wide.id_BytesPerBlock;
    parameter_block->id_DiskType = wide.id_DiskType;
    parameter_block->id_VolumeNode = wide.id_VolumeNode;
    parameter_block->id_InUse = wide.id_InUse;
    return DOSTRUE;
}

/*
 * Release one command-directory list.
 *
 * The locks in it are not only this list's.  Shell.c walks the list to search
 * the path and installs each one as the current directory as it goes
 * (loadCommand(), Shell.c:402), and ACE's CurrentDir() keeps the caller's
 * pointer rather than a copy of it -- so while that search is running the
 * process is standing on a lock this list owns.
 *
 * Normally the Shell puts the previous directory back before anything
 * rebuilds the list, and the window closes.  It cannot when that previous
 * directory has been deleted: CurrentDir() fails at the broker and returns
 * without restoring anything, so the process is left standing on the list's
 * lock.  The next Cli() then frees the list underneath it, and
 * native_sync_current_dir() reads a freed lock and eventually frees it a
 * second time -- which is an abort, from a shell that had done nothing worse
 * than delete the drawer it was sitting in.
 *
 * Dropping the reference sends native_sync_current_dir() back to the broker
 * for the answer, which is where the current directory actually lives; it
 * re-establishes it in native_initial_dir, which is static and never freed.
 */
static void free_cli_path_list(BPTR head)
{
    BPTR *entry = (BPTR *)BADDR(head);

    while (entry) {
        BPTR *next = (BPTR *)BADDR(entry[0]);

        if (native_current_dir == entry[1])
            native_current_dir = BNULL;
        UnLock(entry[1]);
        free(entry);
        entry = next;
    }
}

/* The real Shell walks cli_CommandDir after its current directory. ACE's
 * commands are separate processes, so Path cannot mutate the shell's local
 * list directly. Rebuild the small local view from the broker each time Cli()
 * is requested; the locks remain ordinary native locks, exactly what the
 * unmodified Shell expects to pass to CurrentDir(). */
static void refresh_cli_path_list(void)
{
    char paths[AMIGA_BROKER_MAX_PAYLOAD];
    char command_drawer[PATH_MAX];
    char *save = NULL;
    char *path;
    BPTR head = BNULL;
    BPTR *tail = NULL;

    if (native_broker_listpath(paths, sizeof(paths)) != 0)
        return;
    path = strtok_r(paths, "\n", &save);
    while (path) {
        BPTR *entry;
        BPTR lock;

        if (!*path) {
            path = strtok_r(NULL, "\n", &save);
            continue;
        }
        lock = native_lock_host_path(path);
        entry = lock ? calloc(2, sizeof(*entry)) : NULL;
        if (!entry) {
            if (lock)
                UnLock(lock);
            free_cli_path_list(head);
            return;
        }
        entry[1] = lock;
        if (tail)
            tail[0] = MKBADDR(entry);
        else
            head = MKBADDR(entry);
        tail = entry;
        path = strtok_r(NULL, "\n", &save);
    }

    /* The Shell's source has a separate C: fallback, but that fallback goes
       through GetDeviceProc().  Keep C: in the local command-dir list too:
       it is the canonical command drawer, and this also makes a fresh
       root/device-view session behave like an established session whose
       mutable Path list already contains it. */
    if (native_broker_resolve_path("C:", command_drawer,
                                   sizeof(command_drawer)) == 0) {
        BPTR *entry = NULL;
        BPTR lock = native_lock_host_path(command_drawer);

        if (lock)
            entry = calloc(2, sizeof(*entry));
        if (entry) {
            entry[1] = lock;
            if (tail)
                tail[0] = MKBADDR(entry);
            else
                head = MKBADDR(entry);
            tail = entry;
        } else if (lock) {
            UnLock(lock);
        }
    }
    free_cli_path_list(native_cli.cli_CommandDir);
    native_cli.cli_CommandDir = head;
}

LONG UnLock(BPTR handle)
{
    struct native_lock *lock = handle;

    /* The initial current-directory lock is process state, not a heap lock
       returned by Lock().  Commands such as AROS CD quite reasonably call
       UnLock() on the old value returned by CurrentDir(). */
    if (lock == &native_initial_dir)
        return DOSTRUE;
    if (lock && lock->scan)
        closedir(lock->scan);
    free(handle);
    return DOSTRUE;
}

BPTR CreateDir(CONST_STRPTR name)
{
    char resolved[PATH_MAX];
    struct native_lock *lock;

    if (!name || native_broker_resolve_path(name, resolved, sizeof(resolved)) != 0) {
        native_ioerr = ERROR_OBJECT_NOT_FOUND;
        return BNULL;
    }
    if (ace_crm_retry_mkdir(resolved, 0777) != 0) {
        if (errno == EEXIST)
            native_ioerr = ERROR_OBJECT_EXISTS;
        else
            set_native_broker_error();
        return BNULL;
    }
    lock = calloc(1, sizeof(*lock));
    if (!lock) {
        native_ioerr = ERROR_NO_FREE_STORE;
        return BNULL;
    }
    native_init_lock(lock, resolved, EXCLUSIVE_LOCK);
    return lock;
}

LONG ChangeMode(LONG type, BPTR object, LONG mode)
{
    (void)type;
    (void)object;
    (void)mode;
    return DOSTRUE;
}

LONG WriteChars(CONST_STRPTR buf, ULONG buflen)
{
    BPTR out = Output();
    if (!out) return 0;
    return Write(out, buf, buflen);
}

LONG Examine(BPTR handle, struct FileInfoBlock *fib)
{
    struct native_lock *lock = handle;
    const char *name;

    if (!lock || !fib) {
        native_ioerr = ERROR_INVALID_COMPONENT_NAME;
        return DOSFALSE;
    }
    name = strrchr(lock->path, '/');
    name = name ? name + 1 : lock->path;
    if (native_fill_fib(lock->path, name, 1, fib) != 0)
        return DOSFALSE;

    /* A real Examine() on a directory lock (re)starts the scan ExNext()
       continues; AROS's own ExAll() relies on exactly this to restart
       enumeration by setting eac_LastKey back to 0 and calling Examine()
       again. */
    if (lock->scan)
        closedir(lock->scan);
    lock->scan = fib->fib_DirEntryType > 0 ? ace_crm_retry_opendir(lock->path)
                                           : NULL;
    lock->scan_key = 0;
    lock->scan_prev_pos = lock->scan ? telldir(lock->scan) : 0;
    return DOSTRUE;
}

LONG ExNext(BPTR handle, struct FileInfoBlock *fib)
{
    struct native_lock *lock = handle;
    struct dirent *entry;
    char child[PATH_MAX];
    long pos;

    if (!lock || !fib || !lock->scan) {
        native_ioerr = ERROR_NO_MORE_ENTRIES;
        return DOSFALSE;
    }

    /* ExAll() calls ExNext() once for every entry.  Dir checks for breaks
       after ExAll returns, which is too late for a large host directory: the
       scan also does an lstat() and broker name translation for each entry.
       Poll here so a pending Ctrl-C stops the scan at the next entry rather
       than waiting for the whole ExAll batch/tree to finish.  Leave the bit
       set; Dir's existing SetSignal() path consumes it after ExAll unwinds. */
    native_activate_task();
    if (ace_aros_runtime_check_signal(SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
        native_ioerr = ERROR_BREAK;
        return DOSFALSE;
    }

    /* Replay the entry ExAll() rolled back to via fib_DiskKey (see the
       scan_prev_pos comment on struct native_lock) before scanning on. */
    if (lock->scan_key > 0 && fib->fib_DiskKey == lock->scan_key - 1) {
        seekdir(lock->scan, lock->scan_prev_pos);
        lock->scan_key--;
    }

    for (;;) {
        for (;;) {
            pos = telldir(lock->scan);
            entry = readdir(lock->scan);
            if (!entry) {
                closedir(lock->scan);
                lock->scan = NULL;
                native_ioerr = ERROR_NO_MORE_ENTRIES;
                return DOSFALSE;
            }
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
                break;
        }
        if (snprintf(child, sizeof(child), "%s/%s", lock->path, entry->d_name) >=
            (int)sizeof(child)) {
            native_ioerr = ERROR_LINE_TOO_LONG;
            return DOSFALSE;
        }
        if (native_fill_fib(child, entry->d_name, 0, fib) != 0) {
            /* A name too long to spell, compressed or not, is not this
             * entry's fault and not this scan's failure: real AmigaDOS
             * never meets one, since a real filesystem enforces its own
             * naming rules at creation time, so by the time a directory is
             * being listed every name in it already fits.  ACE is exposing
             * an arbitrary Linux directory that gives no such guarantee, so
             * this is the one place that gap is actually visible, and
             * failing the whole scan over it is far worse than the gap
             * itself: AROS's own ExAll() (rom/dos/exall.c) treats any
             * ExNext() failure other than ERROR_NO_MORE_ENTRIES as a real
             * error and discards every entry the caller had already
             * collected, and Dir.c's recursive descent (ALL) propagates
             * that failure back up through every parent directory on the
             * way to the root -- so one unrepresentable name anywhere in a
             * tree could blank out a listing of thousands of ordinary
             * ones.  Skip it and keep scanning instead: the entry is
             * genuinely missing from what ACE can show, which is the
             * honest limit already documented where component_needs_mapping()
             * gives up, but it costs that one entry, not everything after
             * it.
             *
             * Only these two errors mean "the name itself is the problem";
             * anything else (a stat() failure, disk I/O) is not something
             * skipping the entry would fix, and must still fail the scan
             * the way it always has. */
            if (native_ioerr == ERROR_INVALID_COMPONENT_NAME ||
                native_ioerr == ERROR_LINE_TOO_LONG)
                continue;
            return DOSFALSE;
        }
        lock->scan_prev_pos = pos;
        fib->fib_DiskKey = ++lock->scan_key;
        return DOSTRUE;
    }
}

BPTR DupLock(BPTR handle)
{
    struct native_lock *lock = handle;
    struct native_lock *copy;

    if (!lock)
        return BNULL;
    copy = malloc(sizeof(*copy));
    if (!copy) {
        native_ioerr = ERROR_NO_FREE_STORE;
        return BNULL;
    }
    memcpy(copy, lock, sizeof(*copy));
    copy->fl_Task = (struct MsgPort *)copy;
    copy->fl_Link = NULL;
    copy->fl_Volume = lock->fl_Volume;
    copy->scan = NULL;
    copy->scan_key = 0;
    return copy;
}

BPTR CurrentDir(BPTR handle)
{
    BPTR old;

    if (native_sync_current_dir() != 0) {
        native_ioerr = errno;
        return NULL;
    }

    old = native_current_dir;
    if (handle) {
        struct native_lock *new_dir = handle;
        if (native_broker_setcwd(new_dir->path) != 0) {
            native_ioerr = errno;
            return NULL;
        }
        native_current_dir = handle;
    }
    native_ioerr = 0;
    return old;
}

LONG NameFromLock(BPTR handle, STRPTR buffer, LONG length)
{
    struct native_lock *lock = handle;
    char amiga_name[PATH_MAX];

    if (!lock) {
        native_ioerr = ERROR_INVALID_LOCK;
        return DOSFALSE;
    }
    if (!buffer || length <= 0) {
        native_ioerr = ERROR_LINE_TOO_LONG;
        return DOSFALSE;
    }
    if (native_broker_name_from_host(lock->path, amiga_name,
                                     sizeof(amiga_name)) != 0) {
        native_ioerr = errno;
        return DOSFALSE;
    }
    if (strlen(amiga_name) >= (size_t)length) {
        native_ioerr = ERROR_LINE_TOO_LONG;
        return DOSFALSE;
    }
    strcpy(buffer, amiga_name);
    return DOSTRUE;
}

void SetCurrentDirName(CONST_STRPTR name)
{
    if (!name)
        name = "";
    snprintf(native_cli_set_name, sizeof(native_cli_set_name), "%s", name);
    native_cli.cli_SetName = native_cli_set_name;
}

static FILE *selected_input(void)
{
    native_init_stdio_handles();
    return native_input ? native_input : stdin;
}

static FILE *selected_output(void)
{
    native_init_stdio_handles();
    return native_output ? native_output : stdout;
}

static BPTR handle_for_file(FILE *file)
{
    native_init_stdio_handles();
    if (file == stdin)
        return (BPTR)&native_stdin_handle.amiga;
    if (file == stdout)
        return (BPTR)&native_stdout_handle.amiga;
    if (file == stderr)
        return (BPTR)&native_stderr_handle.amiga;
    return (BPTR)file;
}

/*
 * The host descriptor behind a DOS handle, or -1.
 *
 * For passing a process's own streams to another process with SCM_RIGHTS, so
 * that a script sent to an ARexx port writes on the console of whoever sent
 * it -- rm_Stdin and rm_Stdout in a RexxMsg. On AmigaOS this is free, because
 * a BPTR FileHandle is valid in any task; here the descriptor has to travel.
 *
 * A console handle has no single descriptor to give: it is a channel to the
 * console process, not a file. That case answers -1, and the message travels
 * without that stream, which is the same as it not having been set. It is
 * also the uncommon case -- a command's Output() is normally the standard
 * stream it inherited, whose descriptor is exactly what is wanted.
 *
 * The descriptor is not duplicated: SCM_RIGHTS gives the receiver a
 * descriptor of its own, so the sender keeps using this one unaffected.
 */
int ace_dos_handle_descriptor(BPTR handle)
{
    FILE *file = NULL;

    if (!handle)
        return -1;
    native_init_stdio_handles();
    if (native_console_pointer(handle))
        return -1;
    if (handle == (BPTR)&native_stdin_handle.amiga)
        file = native_stdin_handle.stream;
    else if (handle == (BPTR)&native_stdout_handle.amiga)
        file = native_stdout_handle.stream;
    else if (handle == (BPTR)&native_stderr_handle.amiga)
        file = native_stderr_handle.stream;
    else
        file = (FILE *)handle;
    if (!file)
        return -1;
    return fileno(file);
}

BPTR Output(void)
{
    return handle_for_file(selected_output());
}

BPTR Input(void)
{
    return handle_for_file(selected_input());
}

BPTR OpenFromLock(BPTR handle)
{
    struct native_lock *lock = handle;
    if (!lock)
        return BNULL;
    
    FILE *file = ace_crm_retry_fopen(lock->path, "rb");
    if (!file) {
        native_ioerr = errno == ENOENT ? ERROR_OBJECT_NOT_FOUND : errno;
        return BNULL;
    }

    struct stat information;
    int descriptor = fileno(file);
    if (descriptor >= 0 && fstat(descriptor, &information) == 0 &&
        S_ISDIR(information.st_mode)) {
        fclose(file);
        native_ioerr = ERROR_OBJECT_WRONG_TYPE;
        return BNULL;
    }
    
    native_ioerr = 0;
    return (BPTR)file;
}

BPTR DupLockFromFH(BPTR fh)
{
    if (!fh)
        return BNULL;

    int fd = fileno((FILE *)fh);
    if (fd < 0)
        return BNULL;

    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);

    char link_path[PATH_MAX];
    ssize_t link_len = readlink(proc_path, link_path, sizeof(link_path) - 1);
    if (link_len <= 0)
        return BNULL;
    link_path[link_len] = '\0';

    return native_lock_host_path(link_path);
}

LONG NameFromFH(BPTR fh, STRPTR buffer, LONG length)
{
    if (!fh || !buffer || length <= 0)
        return 0;

    int fd = fileno((FILE *)fh);
    if (fd < 0)
        return 0;

    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);

    char link_path[PATH_MAX];
    ssize_t link_len = readlink(proc_path, link_path, sizeof(link_path) - 1);
    if (link_len <= 0)
        return 0;
    link_path[link_len] = '\0';

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir) {
        char pipe_prefix[PATH_MAX];
        snprintf(pipe_prefix, sizeof(pipe_prefix), "%s/ace-pipes/", runtime_dir);
        size_t prefix_len = strlen(pipe_prefix);
        if (strncmp(link_path, pipe_prefix, prefix_len) == 0) {
            snprintf((char *)buffer, length, "PIPE:%s", link_path + prefix_len);
            return DOSTRUE;
        }
    }
    return 0;
}

static BPTR native_pipe_open(CONST_STRPTR name, LONG mode)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir) {
        native_ioerr = ERROR_OBJECT_NOT_FOUND;
        return BNULL;
    }

    char pipe_dir[PATH_MAX];
    snprintf(pipe_dir, sizeof(pipe_dir), "%s/ace-pipes", runtime_dir);
    mkdir(pipe_dir, 0700);

    char pipe_path[PATH_MAX];
    if (strcmp((const char *)name, "PIPE:") == 0 || strcmp((const char *)name, "PIPE:*") == 0) {
        static int pipe_counter = 1;
        snprintf(pipe_path, sizeof(pipe_path), "%.4000s/pipe-%d-%d", pipe_dir, getpid(), pipe_counter++);
    } else {
        snprintf(pipe_path, sizeof(pipe_path), "%.4000s/%.50s", pipe_dir, name + 5);
    }

    mkfifo(pipe_path, 0600);

    int fd = open(pipe_path, O_RDWR);
    if (fd < 0) {
        native_ioerr = errno;
        return BNULL;
    }

    const char *access = mode == MODE_NEWFILE ? "wb" :
                         mode == MODE_READWRITE ? "r+b" : "rb";
    FILE *file = fdopen(fd, access);
    if (!file) {
        native_ioerr = errno;
        close(fd);
        return BNULL;
    }

    native_ioerr = 0;
    return (BPTR)file;
}

BPTR Open(CONST_STRPTR name, LONG mode)
{
    const char *access = mode == MODE_NEWFILE ? "wb" :
                         mode == MODE_READWRITE ? "r+b" : "rb";
    char resolved[PATH_MAX];
    FILE *file;
    /* Why the open failed, kept from the moment it failed. Everything between
       here and the report -- freeing a DevProc, asking the broker for the
       next target in a multi-assign -- is entitled to leave errno holding
       something else, and errno is only meaningful immediately after the call
       that set it. */
    int open_errno = 0;

    if (name && strncasecmp((const char *)name, "PIPE:", 5) == 0)
        return native_pipe_open(name, mode);

    if (name && (strncasecmp(name, "CON:", 4) == 0 ||
                 strncasecmp(name, "CONSOLE:", 8) == 0 ||
                 strcmp(name, "*") == 0))
        return native_console_open(name);

    /*
     * NIL:, which AmigaOS had natively alongside CON:, RAW:, SER: and PRT:.
     * Writes to it are thrown away and a read of it is immediately at end of
     * file, which is what /dev/null is, so that is what stands behind it.
     *
     * Handled here, before any resolution, for the same reason CON: and PIPE:
     * are: it is a device, not a name in a drawer.  That is not a technicality
     * -- it is the whole reason the two are different things.  Resolving it as
     * a path is what used to make a drawer called "NIL:" and copy real files
     * into it.
     *
     * Opening /dev/null directly rather than through the seam is deliberate
     * twice over.  The seam would refuse it, correctly: a character device is
     * not a file to be read, and nothing that walks a filesystem should be
     * able to open one.  And the refusal does not apply here, because nothing
     * walked a filesystem to get here -- the user named a device and this is
     * the device, not an entry that happens to resemble it.
     *
     * Anything after the colon is ignored, as it is on an Amiga: NIL: takes no
     * path, because there is nothing there to have a path into.
     */
    if (name && strncasecmp(name, "NIL:", 4) == 0) {
        file = fopen("/dev/null", access);

        if (!file) {
            set_native_broker_error();
            return (BPTR)NULL;
        }
        native_ioerr = 0;
        return (BPTR)file;
    }

    if (native_named_device_path(name)) {
        struct DevProc *device;

        file = NULL;

        /* A broker assignment is a complete logical path even when the
           compatibility DosList has not materialised a packet handler for
           it.  Resolve it directly first.  This is especially important for
           a fresh root/device-view session: Open("C:") must see the drawer
           as a directory so Shell.c can apply its implicit-CD behavior. */
        if (native_broker_resolve_path_for_open(name, resolved,
                                               sizeof(resolved)) == 0) {
            errno = 0;
            file = ace_crm_retry_fopen(resolved, access);
            open_errno = errno;
        } else if (errno == ENXIO) {
            /* Not a file to be read, and the broker said so without opening
               anything.  Nothing further to try: another candidate for the
               same name would be the same object. */
            native_ioerr = ERROR_OBJECT_WRONG_TYPE;
            return NULL;
        }

        device = file ? NULL : ace_aros_GetDeviceProc(name, NULL);
        while (device) {
            if (native_union_candidate(name, device, resolved,
                                       sizeof(resolved)) == 0) {
                errno = 0;
                file = ace_crm_retry_fopen(resolved, access);
                open_errno = errno;
            }
            if (file || mode == MODE_NEWFILE ||
                (file == NULL && open_errno != ENOENT &&
                 open_errno != ENOTDIR))
                break;
            device = ace_aros_GetDeviceProc(name, device);
        }
        if (device)
            ace_aros_FreeDeviceProc(device);
        if (file)
            native_ioerr = 0;
        else
            native_ioerr = open_errno == ENOENT || open_errno == 0 ?
                           ERROR_OBJECT_NOT_FOUND : open_errno;
    } else if (native_resolve_path_for_open(name, resolved,
                                           sizeof(resolved)) != 0) {
        /* Through the shared mapping rather than by hand: a name that failed
           to resolve fails for reasons this function has no opinion about,
           and every one of them already has an AmigaDOS counterpart there.
           Assigning the errno straight to native_ioerr is what printed
           "Error 19" at a shell that had redirected into an unmounted
           device, where AmigaDOS says the device is not mounted. */
        if (errno == 0)
            native_ioerr = ERROR_OBJECT_NOT_FOUND;
        else
            set_native_broker_error();
        return NULL;
    } else {
        errno = 0;
        file = ace_crm_retry_fopen(resolved, access);
        open_errno = errno;
    }
    /* A bare name that is not in the current directory used to be looked up
       as a command here, which is where ACE kept its stand-in for C: before
       it had one. That belongs in the loader, not in Open(): AROS's
       loadCommand() already searches the current directory, then the path
       list, then the C: multiassign, and doing it here as well meant any
       failed open of a data file quietly found a command of that name
       instead of reporting that the file was missing. */
    /*
     * A directory is not a file to be opened. Linux will open one read-only
     * quite happily, but AmigaDOS answers ERROR_OBJECT_WRONG_TYPE, and
     * something is listening for that: the Shell tries to open a typed word
     * as a command, and when the open fails with wrong-type, not-found or
     * invalid-component it locks the name instead and changes directory to
     * it if it is one. That is how typing "C:" at an Amiga prompt moves you
     * there. With the open succeeding, the Shell got a handle on a
     * directory, believed it had found a command, and tried to run it.
     */
    if (file) {
        struct stat information;
        int descriptor = fileno(file);

        if (descriptor >= 0 && fstat(descriptor, &information) == 0 &&
            S_ISDIR(information.st_mode)) {
            fclose(file);
            native_ioerr = ERROR_OBJECT_WRONG_TYPE;
            return NULL;
        }
    }
    if (!file && !native_named_device_path(name))
        native_ioerr = open_errno == ENOENT ? ERROR_OBJECT_NOT_FOUND :
                       open_errno;
    else if (file)
        native_ioerr = 0;
    return (BPTR)file;
}

LONG Close(BPTR handle)
{
    FILE *file;

    native_init_stdio_handles();
    if (handle == (BPTR)&native_stdin_handle.amiga ||
        handle == (BPTR)&native_stdout_handle.amiga ||
        handle == (BPTR)&native_stderr_handle.amiga)
        return DOSTRUE;
    if (native_console_is_handle(handle)) {
        native_console_close(handle);
        return DOSTRUE;
    }
    if (!handle) {
        native_ioerr = errno;
        return DOSFALSE;
    }
    file = as_file(handle);
    /* Before the close, not after.  fclose() disassociates the stream
       whether or not it reports an error, so this seam has to stop holding
       the pointer either way -- and once fclose() has returned, even
       comparing the old pointer is undefined. */
    native_forget_script_input(file);
    if (fclose(file) != 0) {
        native_ioerr = errno;
        return DOSFALSE;
    }
    return DOSTRUE;
}

LONG DeleteFile(CONST_STRPTR name)
{
    char resolved[PATH_MAX];

    if (!name ||
        native_resolve_path_for_access(name, resolved, sizeof(resolved)) != 0) {
        set_native_broker_error();
        return DOSFALSE;
    }
    /* One AmigaDOS call removes either kind of object.  Which system call
       that takes is the seam's business, on both the unprivileged and the
       privileged side; a directory that still has children stays refused,
       which is what makes Delete recurse into it before trying again.
       Retrying here would run unprivileged after a privileged attempt had
       already failed, and would report that second refusal instead of the
       real one. */
    if (ace_crm_retry_unlink(resolved) == 0) {
        ace_clipboard_store_deleted_path(resolved);
        return DOSTRUE;
    }
    set_native_broker_error();
    /* Refused for want of permission, which for a removal is what AmigaDOS
       calls delete protection -- the state Delete FORCE exists to clear.
       Only this operation can read those two errnos that way. */
    if (errno == EACCES || errno == EPERM)
        native_ioerr = ERROR_DELETE_PROTECTED;
    return DOSFALSE;
}

LONG SetComment(CONST_STRPTR name, CONST_STRPTR comment)
{
    char resolved[PATH_MAX];
    size_t length = comment ? strlen(comment) : 0;

    if (!name ||
        native_broker_resolve_path(name, resolved, sizeof(resolved)) != 0) {
        set_native_broker_error();
        return DOSFALSE;
    }
    /* Refused rather than truncated, as on AmigaDOS: a FileInfoBlock cannot
       carry more than this back, so a longer comment would be stored and
       then never readable in full by anything that asked for it. */
    if (length >= sizeof(((struct FileInfoBlock *)0)->fib_Comment)) {
        native_ioerr = ERROR_COMMENT_TOO_BIG;
        return DOSFALSE;
    }
    /* An empty comment is how AmigaDOS clears one, so remove the attribute
       rather than leaving an empty one behind. Nothing to remove is the
       state the caller asked for, not a failure. */
    if (length == 0) {
        if (removexattr(resolved, NATIVE_COMMENT_ATTRIBUTE) == 0 ||
            errno == ENODATA)
            return DOSTRUE;
    } else if (setxattr(resolved, NATIVE_COMMENT_ATTRIBUTE, comment, length,
                        0) == 0) {
        return DOSTRUE;
    }
    set_native_broker_error();
    return DOSFALSE;
}

LONG SetProtection(CONST_STRPTR name, ULONG protection)
{
    char resolved[PATH_MAX];
    struct stat information;

    if (!name ||
        native_broker_resolve_path(name, resolved, sizeof(resolved)) != 0 ||
        ace_crm_retry_stat(resolved, &information, 1) != 0 ||
        ace_crm_retry_chmod(resolved,
                          native_mode_from_protection(information.st_mode,
                                                      (LONG)protection)) != 0) {
        set_native_broker_error();
        return DOSFALSE;
    }
    return DOSTRUE;
}

LONG Rename(CONST_STRPTR old_name, CONST_STRPTR new_name)
{
    char old_path[PATH_MAX];
    char new_path[PATH_MAX];

    if (!old_name || !new_name) {
        native_ioerr = ERROR_REQUIRED_ARG_MISSING;
        return DOSFALSE;
    }

    /* AROS's Rename() selects the filesystem for the source through
       GetDeviceProc(), which matters for multi-assigns: the object may live
       in a later AssignList target. The destination is resolved through the
       broker so component mappings remain directory-specific. */
    if ((native_named_device_path(old_name) ?
         native_union_existing(old_name, old_path, sizeof(old_path)) :
         native_broker_resolve_path(old_name, old_path, sizeof(old_path))) != 0 ||
        native_broker_resolve_path(new_name, new_path, sizeof(new_path)) != 0) {
        set_native_broker_error();
        return DOSFALSE;
    }
    if (ace_crm_retry_rename(old_path, new_path) != 0) {
        native_ioerr = errno == EXDEV ? ERROR_RENAME_ACROSS_DEVICES : errno;
        if (errno == ENOENT)
            native_ioerr = ERROR_OBJECT_NOT_FOUND;
        else if (errno == EEXIST)
            native_ioerr = ERROR_OBJECT_EXISTS;
        return DOSFALSE;
    }
    native_ioerr = 0;
    return DOSTRUE;
}

/*
 * How much of two absolute paths is shared, ending on a component boundary.
 *
 * Boundary matters: "/usr/lib" and "/usr/libexec" share the seven characters
 * of "/usr/li", and treating that as a shared path would produce a relative
 * target that walks into a directory nobody named.
 */
static size_t native_common_directory(const char *first, const char *second)
{
    size_t index = 0;
    size_t boundary = 0;

    while (first[index] && second[index] && first[index] == second[index]) {
        if (first[index] == '/')
            boundary = index;
        index++;
    }
    if ((!first[index] || first[index] == '/') &&
        (!second[index] || second[index] == '/'))
        boundary = index;
    return boundary;
}

/*
 * What a soft link on this volume should say.
 *
 * A link's target is text, and the text has to keep meaning something after
 * ACE has exited -- to the user's own processes, to their file manager, to
 * the machine next boot.  Within one volume a relative target does that: it
 * describes a route between two objects on the same disk, which is true
 * wherever that disk is mounted, and it is what AmigaDOS means by a link
 * inside a volume.  An absolute host path would be true only of this mount,
 * and inside the device view it would be true only of this session -- a link
 * that looked right from inside ACE and was broken for everyone else.
 *
 * Returns -1 when the two are not on one volume, which is not an error: the
 * caller then has a different, equally honest answer to give.
 */
static int native_relative_target_for_link(const char *link_path,
                                           const char *target_path,
                                           char *result, size_t result_size)
{
    char directory[PATH_MAX];
    const char *slash = strrchr(link_path, '/');
    const char *remainder;
    size_t common;
    size_t used = 0;

    if (!slash || slash == link_path || target_path[0] != '/')
        return -1;
    if ((size_t)(slash - link_path) >= sizeof(directory))
        return -1;
    memcpy(directory, link_path, (size_t)(slash - link_path));
    directory[slash - link_path] = '\0';

    common = native_common_directory(directory, target_path);
    /* One step up for each component of the link's own directory that lies
       below the shared part. */
    for (const char *cursor = directory + common; *cursor; cursor++) {
        if (*cursor != '/')
            continue;
        if (used + 3 >= result_size)
            return -1;
        memcpy(result + used, "../", 3);
        used += 3;
    }
    remainder = target_path + common;
    while (*remainder == '/')
        remainder++;
    if (!*remainder) {
        /* The target is a directory the link is already inside. */
        if (!used) {
            if (result_size < 2)
                return -1;
            strcpy(result, ".");
            return 0;
        }
        result[used - 1] = '\0';   /* drop the trailing slash of the last ".." */
        return 0;
    }
    if (used + strlen(remainder) + 1 > result_size)
        return -1;
    strcpy(result + used, remainder);
    return 0;
}

/* Whether a resolved host path lives in the fmm's device view, and so names
   an object only this session can reach. */
static int native_path_is_device_view(const char *path)
{
    char root[PATH_MAX];
    size_t length;

    if (native_broker_view_root(root, sizeof(root)) != 0 || !root[0])
        return 0;
    length = strlen(root);
    return strncmp(path, root, length) == 0 &&
           (path[length] == '/' || path[length] == '\0');
}

/* The volume an ACE name belongs to: the text before its colon. */
static int native_volume_of(const char *host_path, char *result,
                            size_t result_size)
{
    char name[PATH_MAX];
    char *colon;

    if (native_broker_name_from_host(host_path, name, sizeof(name)) != 0)
        return -1;
    colon = strchr(name, ':');
    if (!colon)
        return -1;
    *colon = '\0';
    if (strlen(name) >= result_size)
        return -1;
    strcpy(result, name);
    return 0;
}

LONG MakeLink(CONST_STRPTR name, IPTR destination, LONG soft)
{
    char link_path[PATH_MAX];
    char target_path[PATH_MAX];
    struct native_lock *source;

    if (!name || native_broker_resolve_path(name, link_path,
                                             sizeof(link_path)) != 0) {
        set_native_broker_error();
        return DOSFALSE;
    }
    if (soft) {
        char stored[PATH_MAX];
        char link_volume[PATH_MAX];
        char target_volume[PATH_MAX];

        if (native_broker_resolve_path((const char *)destination, target_path,
                                       sizeof(target_path)) != 0) {
            set_native_broker_error();
            return DOSFALSE;
        }
        /*
         * Same volume: say it relatively, so the link survives this session
         * and this mount.  Different volumes: AmigaDOS allows that -- a soft
         * link is a name, not a location, and is not restricted to one volume
         * -- so the absolute host path stands, which is what ReadLink turns
         * back into a volume-qualified ACE name.
         *
         * The one arrangement with no honest answer is a cross-volume link
         * touching the device view.  That path exists only inside the fmm's
         * namespace and only while this session lasts, so writing it into a
         * filesystem would leave a link that is broken for every process that
         * is not this ACE -- including the user's own, and including ACE
         * tomorrow.  Refusing says so now rather than later.
         */
        if (native_volume_of(link_path, link_volume, sizeof(link_volume)) == 0 &&
            native_volume_of(target_path, target_volume,
                             sizeof(target_volume)) == 0 &&
            strcmp(link_volume, target_volume) == 0 &&
            native_relative_target_for_link(link_path, target_path, stored,
                                            sizeof(stored)) == 0) {
            /* relative target chosen */
        } else if (native_path_is_device_view(link_path) ||
                   native_path_is_device_view(target_path)) {
            native_ioerr = ERROR_OBJECT_WRONG_TYPE;
            return DOSFALSE;
        } else if (strlen(target_path) < sizeof(stored)) {
            strcpy(stored, target_path);
        } else {
            native_ioerr = ERROR_LINE_TOO_LONG;
            return DOSFALSE;
        }
        if (ace_crm_retry_symlink(stored, link_path) != 0) {
            set_native_broker_error();
            return DOSFALSE;
        }
    } else {
        source = (struct native_lock *)destination;
        if (!source) {
            errno = EINVAL;
            set_native_broker_error();
            return DOSFALSE;
        }
        if (ace_crm_retry_link(source->path, link_path) != 0) {
            struct stat information;
            int failure = errno;

            set_native_broker_error();
            /*
             * AmigaDOS hard-links directories; Linux does not, for anyone.
             * A directory with two parents is no longer a tree, and "..",
             * recursive deletion and filesystem checking all rest on it being
             * one, so the kernel refuses it at every privilege level.
             *
             * Reported rather than left as a bare refusal.  The two are not
             * the same sentence: "you may not do this here" invites the user
             * to try again with more privilege, which will fail identically,
             * while "this filesystem cannot do this at all" tells them the
             * thing they need in order to stop.  Asked rather than assumed --
             * if a filesystem ever does support it the link simply succeeds
             * and nothing is claimed about it.
             */
            /* Through the escalating stat, not lstat(): the object may be
               somewhere this process cannot look, which is the case the
               retry existed for and would otherwise be the one case that
               reported the refusal without explaining it. */
            if (failure == EPERM &&
                ace_crm_retry_stat(source->path, &information, 0) == 0 &&
                S_ISDIR(information.st_mode))
                native_ioerr = ERROR_NOT_IMPLEMENTED;
            return DOSFALSE;
        }
    }
    native_ioerr = 0;
    return DOSTRUE;
}

/* Linux stores relative symbolic-link targets with POSIX's dot components;
 * AmigaDOS has no current-directory dot and spells parent traversal with an
 * slash (/ is the parent). Preserve the target's relative meaning
 * without resolving it: a dangling link must still report the exact path it
 * points at. */
static int native_relative_link_target(const char *target, char *result,
                                       size_t result_size)
{
    const char *cursor = target;
    size_t used = 0;
    size_t parents = 0;
    int emitted = 0;

    if (!target || !result || result_size == 0)
        return -1;
    result[0] = '\0';
    while (*cursor) {
        const char *end = strchr(cursor, '/');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);

        if (length == 0 || (length == 1 && cursor[0] == '.')) {
            /* No AmigaDOS equivalent is needed for an empty or current
             * directory component. */
        } else if (length == 2 && cursor[0] == '.' && cursor[1] == '.') {
            parents++;
        } else {
            /* At the start, each slash is a parent traversal. After a
             * component, the first slash is its separator and the remaining
             * slashes are parent traversals. */
            size_t slashes = parents ? parents + (emitted ? 1 : 0) :
                                       (emitted ? 1 : 0);

            if (slashes > result_size - used - 1 ||
                length > result_size - used - slashes - 1)
                return -1;
            memset(result + used, '/', slashes);
            used += slashes;
            memcpy(result + used, cursor, length);
            used += length;
            result[used] = '\0';
            parents = 0;
            emitted = 1;
        }
        if (!end)
            break;
        cursor = end + 1;
    }
    if (parents != 0) {
        size_t slashes = parents + (emitted ? 1 : 0);

        if (slashes > result_size - used - 1)
            return -1;
        memset(result + used, '/', slashes);
        used += slashes;
        result[used] = '\0';
    }
    return 0;
}

LONG ReadLink(struct MsgPort *port, BPTR handle, CONST_STRPTR name,
              STRPTR buffer, LONG size)
{
    struct native_lock *lock = handle;
    char path[PATH_MAX];
    char target[PATH_MAX];
    ssize_t length;

    (void)port;
    if (!lock || !name || !buffer || size <= 0 ||
        native_broker_resolve_beneath(lock->path, name, path,
                                      sizeof(path)) != 0) {
        set_native_broker_error();
        return -1;
    }
    length = ace_crm_retry_readlink(path, target, sizeof(target) - 1);
    if (length < 0) {
        set_native_broker_error();
        return -1;
    }
    if ((size_t)length >= sizeof(target)) {
        native_ioerr = ERROR_BUFFER_OVERFLOW;
        return -2;
    }
    target[length] = '\0';
    if (target[0] == '/') {
        /* An absolute Linux target must name its volume explicitly. The
         * broker chooses the volume that owns the target's lexical host
         * path, including a target which does not exist. */
        if (native_broker_name_from_host(target, buffer, (size_t)size) != 0) {
            set_native_broker_error();
            return -1;
        }
    } else if (native_relative_link_target(target, buffer, (size_t)size) != 0) {
        native_ioerr = ERROR_BUFFER_OVERFLOW;
        return -2;
    }
    native_ioerr = 0;
    return (LONG)strlen(buffer);
}

BOOL SameDevice(BPTR first_handle, BPTR second_handle)
{
    struct native_lock *first = first_handle;
    struct native_lock *second = second_handle;
    struct stat first_information;
    struct stat second_information;

    if (!first || !second || stat(first->path, &first_information) != 0 ||
        stat(second->path, &second_information) != 0)
        return DOSFALSE;
    return first_information.st_dev == second_information.st_dev;
}

BPTR ParentDir(BPTR handle)
{
    struct native_lock *lock = handle;
    struct native_lock *parent;
    char path[PATH_MAX];
    char *slash;

    if (!lock)
        return BNULL;
    snprintf(path, sizeof(path), "%s", lock->path);
    slash = strrchr(path, '/');
    if (!slash || slash == path)
        return BNULL;
    *slash = '\0';
    parent = calloc(1, sizeof(*parent));
    if (!parent) {
        native_ioerr = ERROR_NO_FREE_STORE;
        return BNULL;
    }
    native_init_lock(parent, path, SHARED_LOCK);
    native_ioerr = 0;
    return parent;
}

BOOL IsFileSystem(CONST_STRPTR device_name)
{
    if (!device_name || strcasecmp(device_name, "CON:") == 0 ||
        strcasecmp(device_name, "RAW:") == 0 ||
        strcasecmp(device_name, "CONSOLE:") == 0 ||
        strcasecmp(device_name, "NIL:") == 0 ||
        strcmp(device_name, "*") == 0) {
        native_ioerr = ERROR_OBJECT_NOT_FOUND;
        return DOSFALSE;
    }
    native_ioerr = 0;
    return DOSTRUE;
}

LONG SameLock(BPTR lock1, BPTR lock2)
{
    struct native_lock *first = lock1;
    struct native_lock *second = lock2;
    struct stat first_info;
    struct stat second_info;

    if (!first || !second)
        return LOCK_DIFFERENT;
    if (strcmp(first->path, second->path) == 0)
        return LOCK_SAME;
    if (stat(first->path, &first_info) == 0 &&
        stat(second->path, &second_info) == 0 &&
        first_info.st_dev == second_info.st_dev)
        return LOCK_SAME_VOLUME;
    return LOCK_DIFFERENT;
}

BOOL IsInteractive(BPTR handle)
{
    if (native_console_is_handle(handle))
        return native_interactive ? DOSTRUE : DOSFALSE;
    native_init_stdio_handles();
    if (handle != (BPTR)stdin && handle != (BPTR)stdout &&
        handle != (BPTR)stderr &&
        handle != (BPTR)&native_stdin_handle.amiga &&
        handle != (BPTR)&native_stdout_handle.amiga &&
        handle != (BPTR)&native_stderr_handle.amiga)
        return DOSFALSE;
    return native_interactive ? DOSTRUE : DOSFALSE;
}

void native_set_interactive(int interactive)
{
    native_interactive = interactive;
}

LONG SetMode(BPTR handle, LONG mode)
{
    struct ace_console_channel *channel = native_channel_for_handle(handle);
    struct native_console_endpoint *endpoint =
        native_endpoint_for_handle(handle);

    if (channel && (native_console_is_handle(handle) ||
                    handle == (BPTR)&native_stdin_handle.amiga ||
                    handle == (BPTR)stdin)) {
        if (endpoint && native_console_endpoint_set_raw(endpoint, mode != 0) !=
            AMIGA_IOERR_OK) {
            native_ioerr = ERROR_ACTION_NOT_KNOWN;
            return DOSFALSE;
        }
        ace_console_channel_set_raw(channel, mode != 0);
        return DOSTRUE;
    }
    native_init_stdio_handles();
    native_ioerr = ERROR_ACTION_NOT_KNOWN;
    return DOSFALSE;
}

LONG WaitForChar(BPTR handle, LONG timeout)
{
    struct native_console_handle *console = native_console_pointer(handle);
    int result;

    native_init_stdio_handles();
    if (!console && (!handle ||
                     (handle != (BPTR)&native_stdin_handle.amiga &&
                      handle != (BPTR)stdin)))
        return DOSFALSE;
    native_load_input_prefix();
    /* The synthetic argument line is part of the cooked Input() stream only.
       A raw-mode reader never consumes it, so counting it here would report
       a character forever and never reach the select() below -- and a
       program that polls with WaitForChar(0) to ask whether input is waiting
       right now, as Vim's char_avail() does before every screen update,
       would be told "yes" and then block in Read() until a key arrives.
       Bytes the cooked line editor has already pulled off the descriptor are
       different: those are real console input, and Read() returns them. */
    if ((!native_input_is_raw() &&
         native_input_prefix_position < native_input_prefix_length) ||
        native_editor_line_position < native_editor_line_length)
        return DOSTRUE;
    result = ace_console_channel_wait(console ? console->channel :
                                      &native_current_console_channel,
                                      timeout);
    return result > 0 ? DOSTRUE : DOSFALSE;
}

LONG FPutC(BPTR handle, LONG character)
{
    struct native_console_handle *console = native_console_pointer(handle);
    struct native_console_endpoint *endpoint =
        native_endpoint_for_handle(handle);
    unsigned char byte = (unsigned char)character;
    FILE *file;

    if (console || endpoint) {
        size_t actual = 0;
        int error = native_console_endpoint_write(endpoint, &byte, 1,
                                                  &actual);

        if (error != AMIGA_IOERR_OK || actual != 1) {
            native_ioerr = error != AMIGA_IOERR_OK ? error : ERROR_WRITE_PROTECTED;
            return -1;
        }
        return character;
    }
    file = as_file(handle);
    if (fputc((unsigned char)character, file) == EOF) {
        native_ioerr = errno;
        return -1;
    }
    return character;
}

LONG FPuts(BPTR handle, CONST_STRPTR string)
{
    struct native_console_handle *console = native_console_pointer(handle);
    struct native_console_endpoint *endpoint =
        native_endpoint_for_handle(handle);
    FILE *file;

    if (console || endpoint) {
        size_t actual = 0;
        size_t length = strlen((const char *)string);
        int error = native_console_endpoint_write(endpoint, string, length,
                                                  &actual);

        if (error != AMIGA_IOERR_OK || actual != length) {
            native_ioerr = error != AMIGA_IOERR_OK ? error : ERROR_WRITE_PROTECTED;
            return -1;
        }
        return 0;
    }
    file = as_file(handle);
    if (fputs(string, file) == EOF) {
        native_ioerr = errno;
        return -1;
    }
    return 0;
}

STRPTR FGets(BPTR handle, STRPTR buffer, LONG length)
{
    struct native_console_handle *console = native_console_pointer(handle);
    FILE *file = console ? console->input : as_file(handle);
    struct native_console_endpoint *endpoint = console ? console->endpoint :
        (handle ? native_endpoint_for_handle(handle) :
         native_endpoint_for_file(file));

    if (endpoint && (file != stdin || native_input_is_raw())) {
        if (!buffer || length <= 1)
            return NULL;
        for (LONG index = 0; index < length - 1; index++) {
            unsigned char character;

            if (Read(handle, &character, 1) != 1) {
                if (index == 0)
                    return NULL;
                break;
            }
            buffer[index] = (char)character;
            buffer[index + 1] = '\0';
            if (character == '\n')
                break;
        }
        return buffer;
    }

    if (file != stdin) {
        if (!fgets(buffer, length, file)) {
            native_ioerr = errno;
            return NULL;
        }
        return buffer;
    }
    if (!buffer || length <= 1)
        return NULL;
    for (LONG index = 0; index < length - 1; index++) {
        int character = native_input_getc(file);

        if (character == EOF) {
            if (index == 0) {
                native_ioerr = errno;
                return NULL;
            }
            break;
        }
        buffer[index] = (char)character;
        if (character == '\n') {
            buffer[index + 1] = '\0';
            return buffer;
        }
        buffer[index + 1] = '\0';
    }
    return buffer;
}

LONG Flush(BPTR handle)
{
    struct native_console_handle *console = native_console_pointer(handle);
    struct native_console_endpoint *endpoint =
        native_endpoint_for_handle(handle);
    FILE *file;

    if (console || endpoint)
        return DOSTRUE;
    file = as_file(handle);

    if (fflush(file) != 0) {
        native_ioerr = errno;
        return DOSFALSE;
    }
    return DOSTRUE;
}

LONG IoErr(void)
{
    return native_ioerr;
}

void SetIoErr(LONG error)
{
    native_ioerr = error;
}

void native_request_endcli(void)
{
    native_endcli_requested = 1;
    native_cli.cli_Background = DOSTRUE;
    native_cli.cli_Interactive = DOSFALSE;
    /* Shell.c uses this condition to stop a buffered input script. */
    native_cli.cli_FailLevel = 0;
    (void)native_broker_setfaillevel(0);
}

static void set_native_broker_error(void)
{
    /* Anything left unmapped reaches the user as a raw Linux errno through
       an AmigaDOS command's PrintFault(), which prints "Error 39" where
       AmigaDOS would say the directory is not empty. Only errnos with an
       unambiguous AmigaDOS counterpart are translated here; the ones whose
       meaning depends on the operation are mapped by their caller. */
    switch (errno) {
    case ENOENT:      native_ioerr = ERROR_OBJECT_NOT_FOUND; break;
    case ENOMEM:      native_ioerr = ERROR_NO_FREE_STORE; break;
    case ENOTEMPTY:   native_ioerr = ERROR_DIRECTORY_NOT_EMPTY; break;
    case EEXIST:      native_ioerr = ERROR_OBJECT_EXISTS; break;
    case ENOTDIR:     native_ioerr = ERROR_OBJECT_WRONG_TYPE; break;
    /* Something that is not a file being asked to behave as one: a device
       node, a socket, a FIFO.  Linux answers ENXIO when one of these is
       opened, and the broker answers it in advance rather than letting the
       open happen at all. */
    case ENXIO:       native_ioerr = ERROR_OBJECT_WRONG_TYPE; break;
    /* A device, volume or assign that is not there.  AmigaDOS asks the user
       to insert the volume and, when it does not appear, says this. */
    case ENODEV:      native_ioerr = ERROR_DEVICE_NOT_MOUNTED; break;
    case EROFS:       native_ioerr = ERROR_DISK_WRITE_PROTECTED; break;
    case ENOSPC:      native_ioerr = ERROR_DISK_FULL; break;
    case ENAMETOOLONG: native_ioerr = ERROR_INVALID_COMPONENT_NAME; break;
    /* The filesystem does not implement the operation -- a VFAT volume has
       no extended attributes to keep a file comment in, and ACE mounts VFAT.
       ERROR_ACTION_NOT_KNOWN is AmigaDOS's own answer for a handler that
       does not understand a packet, which is the same statement. */
    case ENOTSUP:     native_ioerr = ERROR_ACTION_NOT_KNOWN; break;
    default:          native_ioerr = (LONG)errno; break;
    }
}

BOOL Relabel(CONST_STRPTR drive, CONST_STRPTR name)
{
    if (!drive || !*drive || !name || !*name) {
        native_ioerr = ERROR_REQUIRED_ARG_MISSING;
        return DOSFALSE;
    }
    if (native_broker_relabel(drive, name) != 0) {
        set_native_broker_error();
        return DOSFALSE;
    }
    native_ioerr = 0;
    return DOSTRUE;
}

/*
 * The script a shell is reading commands from, when it is reading from one.
 * AmigaDOS calls this cli_CurrentInput, and every command that has to know
 * whether it is running inside a script -- If, Else, EndIf -- asks by
 * comparing it against cli_StandardInput.
 *
 * On a real Amiga the shell and its commands share one CLI, so a command can
 * simply read it. An ACE command is its own Linux process, so the shell
 * passes the open descriptor across the fork and names it here. What comes
 * back is a stream over the same file description, which means the same file
 * offset: a command that consumes lines -- If skipping to its EndIf --
 * advances the shell's own position by doing so, exactly as it would on the
 * machine this code was written for.
 *
 * Unbuffered for the same reason. Any read-ahead would consume the shell's
 * next command into a buffer that dies with the command's process.
 */
static FILE *native_script_input;

/* The last character FGetC() handed out, for UnGetC()'s AmigaDOS -1. */
static FILE *native_last_read_file;
static int native_last_read_char = EOF;

/* Set once the script stream has been closed in this process, so the
   descriptor named in the environment is never opened a second time.  It
   belonged to the stream that has just gone; the number is either closed or
   has been handed to something else since. */
static int native_script_input_closed;

static void native_attach_script_input(void)
{
    const char *descriptor = getenv(ACE_SCRIPT_INPUT_VARIABLE);
    char *end;
    long value;

    if (native_script_input || native_script_input_closed ||
        !descriptor || !*descriptor)
        return;
    value = strtol(descriptor, &end, 10);
    if (*end || value < 0 || value > INT_MAX)
        return;
    native_script_input = fdopen((int)value, "r");
    if (native_script_input)
        (void)setvbuf(native_script_input, NULL, _IONBF, 0);
}

void native_cli_set_script_input(FILE *file)
{
    native_script_input = file;
    if (file)
        (void)setvbuf(file, NULL, _IONBF, 0);
    if (native_cli_loaded)
        native_cli.cli_CurrentInput = file ? handle_for_file(file) :
                                      native_cli.cli_StandardInput;
}

FILE *native_cli_script_input(void)
{
    native_attach_script_input();
    return native_script_input;
}

/*
 * The shell owns cli_CurrentInput and closes it when a script ends
 * (workbench/c/Shell/Shell.c, right before it assigns cli_StandardInput in
 * its place).  For an ACE shell that handle is this process's script input,
 * and nothing used to tell this seam that the stream underneath it had gone:
 * the static outlived the FILE, so every later RunCommand() read a freed
 * stream to find the descriptor to pass to its child, and Quit seeked one.
 * Called from Close() for exactly the stream being closed, whoever closes
 * it -- the shell at the end of a script, or a command that was handed the
 * handle.
 */
static void native_forget_script_input(FILE *file)
{
    if (!file || file != native_script_input)
        return;
    native_script_input_closed = 1;
    /* UnGetC()'s record of the last stream read is the same pointer. */
    if (native_last_read_file == file) {
        native_last_read_file = NULL;
        native_last_read_char = EOF;
    }
    /* Puts cli_CurrentInput back on standard input, which is what the shell
       does next anyway; a command that closed the script rather than the
       shell gets the same consistent CLI. */
    native_cli_set_script_input(NULL);
}

/* The AmigaDOS Quit command advances cli_CurrentInput beyond the end of the
 * script. ACE's script input is a shared FILE description, so seeking this
 * stream to EOF gives the parent shell the same observable result after the
 * command process exits. */
int native_quit_script(void)
{
    FILE *script = native_cli_script_input();

    if (!script || fseeko(script, 0, SEEK_END) != 0) {
        native_ioerr = errno ? errno : ERROR_OBJECT_NOT_FOUND;
        return -1;
    }
    clearerr(script);
    native_ioerr = 0;
    return 0;
}


/* Streams and process links, which do not depend on the broker at all. */
static void cli_attach_streams(void)
{
    native_attach_script_input();
    if (native_script_input) {
        /* The two have to stay distinguishable now: If does SelectInput() on
           the script, so Input() is no longer the standard input, and
           Shell.c assigns cli_CurrentInput itself when the script ends.
           Take the script only on the first pass and leave it alone after,
           or the shell could never get back to reading the keyboard. */
        native_cli.cli_StandardInput = handle_for_file(stdin);
        if (!native_cli_loaded)
            native_cli.cli_CurrentInput =
                handle_for_file(native_script_input);
    } else {
        native_cli.cli_StandardInput = Input();
        native_cli.cli_CurrentInput = native_cli.cli_StandardInput;
    }
    native_cli.cli_StandardOutput = Output();
    native_cli.cli_CurrentOutput = native_cli.cli_StandardOutput;
    native_cli.cli_StandardError = handle_for_file(stderr);
    native_cli.cli_DefaultStack = 8192 / CLI_DEFAULTSTACK_UNIT;
    native_process.pr_CES = (BPTR)stderr;
    native_process.pr_CLI = (BPTR)&native_cli;
    native_process.pr_TaskNum = 1;
    native_cli_loaded = 1;
}

/*
 * Real AmigaDOS gives every shell process a CLI, so AROS's own Shell.c and
 * cliPrompt.c dereference Cli() without checking it -- correct for the
 * system they were written against. On ACE the CLI is broker-backed, and the
 * broker is a separate process with a finite session table, so it can fail
 * where a real Amiga's could not. Returning NULL there turned a recoverable
 * broker error into a SIGSEGV inside unmodified AROS source (Shell.c:716,
 * and five more unchecked Cli() calls inside its command loop).
 *
 * So this never returns NULL. It keeps whatever state was last read
 * successfully -- which is what a transient failure wants -- or falls back to
 * the same defaults the broker itself would hand a new session, and says once
 * on stderr what went wrong, so a full session table is a diagnosable
 * degradation rather than a window that vanishes.
 */
static struct CommandLineInterface *cli_without_broker(void)
{
    static int warned;

    if (!warned) {
        warned = 1;
        fprintf(stderr, "ace: broker CLI state unavailable (%s); "
                        "continuing with defaults\n", strerror(errno));
    }
    if (!native_cli_loaded) {
        native_cli.cli_FailLevel = 10; /* the broker's DEFAULT_FAIL_LEVEL */
        native_cli_prompt[0] = '\0';
        native_cli.cli_Prompt = native_cli_prompt;
        native_cli_set_name[0] = '\0';
        native_cli.cli_SetName = native_cli_set_name;
    }
    cli_attach_streams();
    return &native_cli;
}

struct CommandLineInterface *Cli(void)
{
    char state[AMIGA_BROKER_MAX_PAYLOAD];
    char cwd[PATH_MAX];
    char *save = NULL;
    char *return_code;
    char *result2;
    char *fail_level;
    char *prompt;

    /* Shell.c writes CLI-level failures after its last broker refresh. If it
       asks for Cli() again before leaving the shell, publish that state before
       the broker payload replaces the local fields. */
    native_publish_shell_result();
    if (native_broker_ensure() != 0)
        return cli_without_broker();

    if (native_broker_getcli(state, sizeof(state)) != 0)
        return cli_without_broker();
    return_code = strtok_r(state, "\n", &save);
    result2 = strtok_r(NULL, "\n", &save);
    fail_level = strtok_r(NULL, "\n", &save);
    if (!return_code || !result2 || !fail_level)
        return cli_without_broker();
    /*
     * The prompt is what is left, newlines and all, rather than the next
     * token. A prompt may contain them: "*N" is the AmigaDOS star-escape for
     * a newline, ReadItem() converts it, and "Prompt \"*N%S> \"" -- a blank
     * line before each prompt -- is an ordinary thing to ask for. Tokenising
     * here would cut such a prompt at its first newline and leave the rest
     * unprintable. GETCLI puts the prompt last precisely so that it can be
     * taken whole; the three fields before it are numbers and cannot contain
     * a newline of their own.
     */
    prompt = save;

    /* A previously promoted CLI error remains in native_cli until the next
       broker refresh. Once a command has published a clean result, discard
       that old local Result2 so it cannot be promoted again. */
    if (native_shell_result_published &&
        strtol(return_code, NULL, 10) == RETURN_OK &&
        strtol(result2, NULL, 10) == 0)
        native_shell_result_published = 0;

    native_cli.cli_ReturnCode = (LONG)strtol(return_code, NULL, 10);
    native_cli.cli_Result2 = (LONG)strtol(result2, NULL, 10);
    native_cli.cli_FailLevel = (LONG)strtol(fail_level, NULL, 10);
    if (native_endcli_requested) {
        native_cli.cli_Background = DOSTRUE;
        native_cli.cli_Interactive = DOSFALSE;
        native_cli.cli_FailLevel = 0;
    }
    strncpy(native_cli_prompt, prompt, sizeof(native_cli_prompt) - 1);
    native_cli_prompt[sizeof(native_cli_prompt) - 1] = '\0';
    native_cli.cli_Prompt = native_cli_prompt;
    if (native_broker_getcwd(cwd, sizeof(cwd)) == 0 &&
        native_broker_name_from_host(cwd, native_cli_set_name,
                                     sizeof(native_cli_set_name)) == 0) {
        /* The broker returns the AROS spelling, not the Linux cwd. */
    } else {
        native_cli_set_name[0] = '\0';
    }
    native_cli.cli_SetName = native_cli_set_name;
    cli_attach_streams();
    refresh_cli_path_list();
    return &native_cli;
}

void Forbid(void)
{
}

void Permit(void)
{
}

UBYTE ToLower(ULONG character)
{
    if (character >= 'A' && character <= 'Z')
        character += 'a' - 'A';
    return (UBYTE)character;
}

ULONG SetSignal(ULONG set_mask, ULONG clear_mask)
{
    native_activate_task();
    return ace_aros_runtime_set_signal(set_mask, clear_mask);
}

void Signal(struct Task *task, ULONG signal_set)
{
    native_activate_task();
    for (size_t index = 0; index < NATIVE_REMOTE_TASK_LIMIT; index++) {
        if (task == &native_remote_tasks[index].task) {
            (void)native_broker_task_signal(native_remote_tasks[index].broker_id,
                                            signal_set);
            return;
        }
    }
    ace_aros_runtime_signal_task(task, signal_set);
}

void SetMem(APTR destination, ULONG length, UBYTE value)
{
    memset(destination, value, length);
}


ULONG CheckSignal(ULONG mask)
{
    native_activate_task();
    return ace_aros_runtime_check_signal(mask);
}

LONG AllocSignal(LONG signal_number)
{
    native_activate_task();
    return ace_aros_runtime_alloc_signal(signal_number);
}

void FreeSignal(LONG signal_number)
{
    native_activate_task();
    ace_aros_runtime_free_signal(signal_number);
}

BOOL SetPrompt(CONST_STRPTR prompt)
{
    if (!prompt || native_broker_setprompt(prompt) != 0) {
        if (prompt)
            set_native_broker_error();
        else
            native_ioerr = ERROR_REQUIRED_ARG_MISSING;
        return DOSFALSE;
    }
    if (native_cli_loaded) {
        strncpy(native_cli_prompt, prompt, sizeof(native_cli_prompt) - 1);
        native_cli_prompt[sizeof(native_cli_prompt) - 1] = '\0';
        native_cli.cli_Prompt = native_cli_prompt;
    }
    return DOSTRUE;
}

/*
 * One conversion of the AmigaDOS format grammar, read once and used twice:
 * by the formatter below, and by the argument collector Printf() needs to
 * pull a C va_list apart the way RawDoFmt says the values were passed.  Two
 * copies of this parser would be two things to keep in step.
 *
 * "wide" is not RawDoFmt's -- it has no 64-bit conversion, because the
 * machine it was written for had no 64-bit register.  ACE's own commands
 * spell process and task identifiers %llu, so the length modifier is read
 * here rather than making those callers lie about the width.
 */
struct native_format_spec {
    int left;
    int fill;
    ULONG minimum;
    ULONG maximum;
    int wide;
    char conversion;
};

static CONST_STRPTR native_scan_format(CONST_STRPTR format,
                                       struct native_format_spec *spec)
{
    spec->left = 0;
    spec->fill = ' ';
    spec->minimum = 0;
    spec->maximum = (ULONG)-1;
    spec->wide = 0;

    if (*format == '-') {
        spec->left = 1;
        format++;
    }
    if (*format == '0') {
        spec->fill = '0';
        format++;
    }
    while (*format >= '0' && *format <= '9')
        spec->minimum = spec->minimum * 10u + (ULONG)(*format++ - '0');
    if (*format == '.') {
        format++;
        if (*format >= '0' && *format <= '9') {
            spec->maximum = 0;
            do {
                spec->maximum = spec->maximum * 10u + (ULONG)(*format++ - '0');
            } while (*format >= '0' && *format <= '9');
        }
    }
    /* AmigaDOS writes its 32-bit LONG as %ld, so a single l is the ordinary
       case and says nothing about width; ll, z and j are the host spellings
       of a 64-bit value. */
    if (*format == 'l' && format[1] == 'l') {
        spec->wide = 1;
        format += 2;
    } else if (*format == 'z' || *format == 'j' || *format == 'q') {
        spec->wide = 1;
        format++;
    } else if (*format == 'l' || *format == 'i' || *format == 'h') {
        format++;
    }
    spec->conversion = *format ? *format++ : '\0';
    return format;
}

/*
 * VPrintf() receives AmigaDOS's RawDoFmt data stream, not a C va_list.
 * The real Dir command deliberately passes an array of IPTR values to it
 * (including formats such as "%-32.s"), so forwarding the format string to
 * PutStr() silently loses every directory name.  Keep this small formatter
 * in the DOS host seam; its grammar mirrors exec.library's RawDoFmt parser
 * for the string and scalar forms used by the ported commands.
 */
static LONG native_vprintf(CONST_STRPTR format, const IPTR *data)
{
    BPTR output = Output();
    LONG written = 0;
    size_t argument = 0;

    while (*format) {
        struct native_format_spec spec;
        char number_buffer[sizeof(IPTR) * 8 + 2];
        const char *text = NULL;
        ULONG length = 0;
        SIPTR signed_value = 0;
        IPTR unsigned_value = 0;
        int left;
        int fill;
        ULONG minimum;
        ULONG maximum;
        char conversion;

        if (*format != '%') {
            if (FPutC(output, (unsigned char)*format++) < 0)
                return DOSFALSE;
            written++;
            continue;
        }

        format = native_scan_format(format + 1, &spec);
        left = spec.left;
        fill = spec.fill;
        minimum = spec.minimum;
        maximum = spec.maximum;
        conversion = spec.conversion;

        switch (conversion) {
        case 's':
            text = (const char *)(uintptr_t)data[argument++];
            if (!text)
                text = "";
            length = (ULONG)strlen(text);
            break;

        case 'd':
        case 'D':
        {
            char digits[sizeof(number_buffer)];
            ULONG digit_count = 0;
            int negative;

            signed_value = (SIPTR)data[argument++];
            negative = signed_value < 0;
            /* Through the unsigned type: the most negative value has no
               positive counterpart to negate. */
            unsigned_value = negative ? (IPTR)0 - (IPTR)signed_value :
                                        (IPTR)signed_value;
            do {
                digits[digit_count++] =
                    (char)('0' + (unsigned_value % 10));
                unsigned_value /= 10;
            } while (unsigned_value != 0 && digit_count < sizeof(digits));

            /* The sign belongs inside what gets printed.  It used to be
               written to number_buffer[0] while text was set past it, so
               "%ld" of -5 printed the 5, skipped the minus, and read one
               byte beyond the digits it had written. */
            text = number_buffer;
            if (negative)
                number_buffer[length++] = '-';
            while (digit_count > 0)
                number_buffer[length++] = digits[--digit_count];
            break;
        }

        case 'u':
        case 'U':
        case 'x':
        case 'X':
        case 'p':
        case 'P': {
            static const char digits[] = "0123456789ABCDEF";
            unsigned int base = conversion == 'x' || conversion == 'X' ||
                                conversion == 'p' || conversion == 'P' ? 16 : 10;
            int pointer = conversion == 'p' || conversion == 'P';
            char *end = number_buffer + sizeof(number_buffer);
            char *cursor = end;

            unsigned_value = data[argument++];
            if (pointer) {
                fill = '0';
                minimum = sizeof(APTR) * 2;
            }
            do {
                *--cursor = digits[unsigned_value % base];
                unsigned_value /= base;
            } while (unsigned_value != 0 && cursor > number_buffer);
            text = cursor;
            length = (ULONG)(end - cursor);
            break;
        }

        case 'c':
            number_buffer[0] = (char)data[argument++];
            text = number_buffer;
            length = 1;
            break;

        case 'b': {
            const UBYTE *bstring = (const UBYTE *)(uintptr_t)data[argument++];

            if (!bstring) {
                text = "";
                length = 0;
            } else {
                length = bstring[0];
                text = (const char *)(bstring + 1);
            }
            break;
        }

        case '%':
            number_buffer[0] = '%';
            text = number_buffer;
            length = 1;
            break;

        default:
            /* Match RawDoFmt's useful treatment of an unknown conversion:
               print the conversion character and consume no data value. */
            number_buffer[0] = conversion;
            text = number_buffer;
            length = conversion ? 1u : 0u;
            break;
        }

        if (length > maximum)
            length = maximum;
        if (!left) {
            while (length < minimum) {
                if (FPutC(output, fill) < 0)
                    return DOSFALSE;
                written++;
                minimum--;
            }
        }
        for (ULONG i = 0; i < length; i++) {
            if (FPutC(output, (unsigned char)text[i]) < 0)
                return DOSFALSE;
            written++;
        }
        if (left) {
            while (length < minimum) {
                if (FPutC(output, fill) < 0)
                    return DOSFALSE;
                written++;
                minimum--;
            }
        }
    }
    return Flush(output) == DOSFALSE ? DOSFALSE : written;
}

LONG VPrintf(CONST_STRPTR format, APTR arguments)
{
    return native_vprintf(format, (const IPTR *)arguments);
}

void native_publish_result(int result_code)
{
    /* A standalone command is still useful without a broker.  In that case
       there is simply no session in which to publish the result. */
    if (native_cli_loaded)
        (void)native_broker_setfaillevel((int32_t)native_cli.cli_FailLevel);
    if (native_cli_loaded && native_cli.cli_Background)
        (void)native_broker_setvar("__ACE_ENDCLI", "1",
                                   AMIGA_BROKER_VAR_LOCAL);
    (void)native_broker_setresult((int32_t)result_code, (int32_t)native_ioerr);
    if (native_cli_loaded && native_cli.cli_Background)
        _exit(NATIVE_ENDCLI_STATUS);
}

/* Shell.c records a command-not-found (and other CLI-level failures) in its
 * local CLI, but no command process runs to publish that state to the broker.
 * Promote that error once the shell returns, without replacing a result that
 * a command already published. */
void native_publish_shell_result(void)
{
    if (!native_cli_loaded || native_shell_result_published ||
        native_cli.cli_ReturnCode != RETURN_OK ||
        native_cli.cli_Result2 == 0)
        return;
    native_shell_result_published = 1;
    native_cli.cli_ReturnCode = RETURN_ERROR;
    (void)native_broker_setresult(RETURN_ERROR,
                                   (int32_t)native_cli.cli_Result2);
}

/*
 * Global variables are files, one per variable, in ENV: -- and in ENVARC:
 * too when they are meant to outlive the boot. That is not an implementation
 * detail of AmigaDOS, it is the interface: a program reads ENV:SYS/theme.var
 * by opening it, `Type ENV:Editor` prints one, and Delete removes one, all
 * without going near GetVar(). rom/dos/{getvar,setvar,deletevar}.c are the
 * description this follows.
 *
 * Local variables stay in the broker. On AmigaOS they live on the process's
 * own pr_LocalVars list, which every command in that shell shares because
 * every command in that shell is that process. An ACE command is a separate
 * Linux process, so the list has to live somewhere both can reach, and that
 * is what the broker is.
 */
static int global_var_name(const char *volume, CONST_STRPTR name,
                           char *result, size_t result_size)
{
    int written;

    if (!name || !*name)
        return -1;
    /* The volume ends in ':', so this is AddPart() for the only shape that
       reaches here -- and it keeps any subdirectory in the name, which is
       how ENV:SYS/theme.var works. */
    written = snprintf(result, result_size, "%s%s", volume, (const char *)name);
    return written > 0 && (size_t)written < result_size ? 0 : -1;
}

static LONG global_var_read(const char *volume, CONST_STRPTR name,
                            STRPTR buffer, LONG size, LONG flags)
{
    char path[PATH_MAX];
    BPTR file;
    LONG count;

    if (!buffer || size <= 0 ||
        global_var_name(volume, name, path, sizeof(path)) != 0)
        return -1;
    file = Open(path, MODE_OLDFILE);
    if (!file)
        return -1;
    count = Read(file, buffer, size);
    Close(file);
    if (count < 0)
        return -1;
    if (!(flags & GVF_BINARY_VAR)) {
        LONG index = 0;

        /* A variable is the first line of its file unless the caller says
           it wants the bytes. */
        while (index < count && buffer[index] != '\n')
            index++;
        if (index >= size)
            index = size - 1;
        buffer[index] = '\0';
        return index;
    }
    if (!(flags & GVF_DONT_NULL_TERM)) {
        if (count >= size)
            count = size - 1;
        buffer[count] = '\0';
    }
    return count;
}

static BOOL global_var_write(const char *volume, CONST_STRPTR name,
                             CONST_STRPTR value, LONG size)
{
    char path[PATH_MAX];
    BPTR file;
    LONG written;

    if (global_var_name(volume, name, path, sizeof(path)) != 0)
        return DOSFALSE;
    file = Open(path, MODE_NEWFILE);
    if (!file)
        return DOSFALSE;
    written = size > 0 ? Write(file, (APTR)value, size) : 0;
    Close(file);
    return written == size ? DOSTRUE : DOSFALSE;
}

static BOOL global_var_delete(const char *volume, CONST_STRPTR name)
{
    char path[PATH_MAX];

    if (global_var_name(volume, name, path, sizeof(path)) != 0)
        return DOSFALSE;
    return DeleteFile(path);
}

LONG GetVar(CONST_STRPTR name, STRPTR buffer, LONG size, LONG flags)
{
    char value[PATH_MAX];
    uint32_t broker_flags = 0;
    size_t length;

    if ((flags & 0xff) == LV_ALIAS)
        broker_flags |= AMIGA_BROKER_VAR_ALIAS;
    if (!(flags & GVF_GLOBAL_ONLY)) {
        if (native_broker_getvar(name, broker_flags | AMIGA_BROKER_VAR_LOCAL,
                                 value, sizeof(value)) == 0) {
            length = strlen(value);
            if (!buffer || size <= 0 || length >= (size_t)size) {
                native_ioerr = ERROR_LINE_TOO_LONG;
                return -1;
            }
            memcpy(buffer, value, length + 1);
            return (LONG)length;
        }
    }
    /* ENV: first and ENVARC: after: a variable that has been changed this
       boot shadows the saved one, which is the point of having both. */
    if ((flags & 0xff) == LV_VAR && !(flags & GVF_LOCAL_ONLY)) {
        LONG result = global_var_read("ENV:", name, buffer, size, flags);

        if (result >= 0)
            return result;
        result = global_var_read("ENVARC:", name, buffer, size, flags);
        if (result >= 0)
            return result;
    }
    native_ioerr = ERROR_OBJECT_NOT_FOUND;
    return -1;
}

BOOL SetVar(CONST_STRPTR name, CONST_STRPTR value, LONG size, LONG flags)
{
    char truncated[PATH_MAX];
    const char *stored = value;
    uint32_t broker_flags = 0;
    size_t length;

    if (size >= 0) {
        length = (size_t)size;
        if (length >= sizeof(truncated)) {
            native_ioerr = ERROR_LINE_TOO_LONG;
            return DOSFALSE;
        }
        memcpy(truncated, value, length);
        truncated[length] = '\0';
        stored = truncated;
    } else {
        length = strlen(value);
        size = (LONG)length;
    }
    if ((flags & 0xff) == LV_VAR && (flags & GVF_GLOBAL_ONLY)) {
        if (!global_var_write("ENV:", name, stored, size))
            return DOSFALSE;
        /* GVF_SAVE_VAR is what makes a variable survive the next boot: it
           goes to the archive as well as to the live copy. */
        if ((flags & GVF_SAVE_VAR) &&
            !global_var_write("ENVARC:", name, stored, size))
            return DOSFALSE;
        return DOSTRUE;
    }
    if ((flags & 0xff) == LV_ALIAS)
        broker_flags |= AMIGA_BROKER_VAR_ALIAS;
    broker_flags |= AMIGA_BROKER_VAR_LOCAL;
    if (native_broker_setvar(name, stored, broker_flags) != 0) {
        set_native_broker_error();
        return DOSFALSE;
    }
    return DOSTRUE;
}

BOOL DeleteVar(CONST_STRPTR name, LONG flags)
{
    uint32_t broker_flags = 0;

    if ((flags & 0xff) == LV_VAR && (flags & GVF_GLOBAL_ONLY)) {
        BOOL removed = global_var_delete("ENV:", name);

        /* Without GVF_SAVE_VAR the archived copy stays, so the variable
           comes back at the next boot -- which is the documented way to undo
           a change made only for this one. */
        if (flags & GVF_SAVE_VAR)
            removed = global_var_delete("ENVARC:", name) || removed;
        return removed;
    }
    if ((flags & 0xff) == LV_ALIAS)
        broker_flags |= AMIGA_BROKER_VAR_ALIAS;
    broker_flags |= AMIGA_BROKER_VAR_LOCAL;
    if (native_broker_deletevar(name, broker_flags) != 0) {
        set_native_broker_error();
        return DOSFALSE;
    }
    return DOSTRUE;
}

/*
 * Printf() takes an AmigaDOS format string, not a C one.  %ld there is the
 * 32-bit LONG this port still typedefs it as; a C library on this host reads
 * the same spelling as a 64-bit long.  Handing the format to vsnprintf()
 * therefore printed every negative LONG as a huge positive number -- "Eval 3
 * - 10" answered 4294967289 -- and got positives right only by the accident
 * of the upper half of the register happening to be zero.
 *
 * So the arguments are collected the way the Amiga grammar says they were
 * passed, into the same IPTR stream VPrintf() takes, and printed by the same
 * formatter.  One grammar, one formatter, both callers.
 */
#define NATIVE_PRINTF_MAX_ARGUMENTS 32

static LONG native_printf_arguments(CONST_STRPTR format, va_list arguments,
                                    IPTR *values, size_t limit)
{
    size_t count = 0;

    while (*format) {
        struct native_format_spec spec;

        if (*format != '%') {
            format++;
            continue;
        }
        format = native_scan_format(format + 1, &spec);
        /* Neither consumes a value: %% is a literal, and an unknown
           conversion is echoed by the formatter without taking one. */
        if (spec.conversion == '%' || spec.conversion == '\0')
            continue;
        if (!strchr("sbdDuUxXpPc", spec.conversion))
            continue;
        if (count >= limit)
            return DOSFALSE;

        switch (spec.conversion) {
        case 's':
        case 'b':
        case 'p':
        case 'P':
            values[count++] = (IPTR)(uintptr_t)va_arg(arguments, void *);
            break;
        case 'd':
        case 'D':
            /* Sign-extended into the slot, which is what makes the same
               formatter serve both widths. */
            values[count++] = spec.wide ?
                (IPTR)(SIPTR)va_arg(arguments, long long) :
                (IPTR)(SIPTR)va_arg(arguments, int);
            break;
        case 'c':
            values[count++] = (IPTR)(unsigned int)va_arg(arguments, int);
            break;
        default:
            values[count++] = spec.wide ?
                (IPTR)va_arg(arguments, unsigned long long) :
                (IPTR)va_arg(arguments, unsigned int);
            break;
        }
    }
    return DOSTRUE;
}

LONG Printf(CONST_STRPTR format, ...)
{
    IPTR values[NATIVE_PRINTF_MAX_ARGUMENTS];
    va_list arguments;
    LONG collected;

    if (!format)
        return DOSFALSE;
    va_start(arguments, format);
    collected = native_printf_arguments(format, arguments, values,
                                        sizeof(values) / sizeof(values[0]));
    va_end(arguments);
    if (collected == DOSFALSE) {
        native_ioerr = ERROR_LINE_TOO_LONG;
        return DOSFALSE;
    }
    return native_vprintf(format, values);
}

LONG PutStr(CONST_STRPTR string)
{
    LONG length = string ? (LONG)strlen((const char *)string) : 0;

    return Write(Output(), (APTR)string, length) == length ?
           DOSTRUE : DOSFALSE;
}

#include "aros_fault.c"

LONG FGetC(BPTR handle)
{
    struct native_console_handle *console = native_console_pointer(handle);
    FILE *file = console ? console->input :
        (handle ? as_file(handle) : selected_input());
    struct native_console_endpoint *endpoint = console ? console->endpoint :
        (handle ? native_endpoint_for_handle(handle) :
         native_endpoint_for_file(file));
    int character;

    if (endpoint && (file != stdin || native_input_is_raw())) {
        unsigned char byte;
        size_t actual = 0;
        int error = native_console_endpoint_read(endpoint, &byte, 1,
                                                 &actual);

        if (error != AMIGA_IOERR_OK || actual == 0) {
            native_ioerr = error != AMIGA_IOERR_OK ? error :
                           ERROR_OBJECT_NOT_FOUND;
            return ENDSTREAMCH;
        }
        character = byte;
    } else {
        character = native_input_getc(file);
    }

    if (character == EOF) {
        native_ioerr = ERROR_OBJECT_NOT_FOUND;
        return ENDSTREAMCH;
    }
    native_last_read_file = file;
    native_last_read_char = character;
    return (UBYTE)character;
}

LONG Read(BPTR handle, APTR buffer, LONG length)
{
    struct native_console_handle *console = native_console_pointer(handle);
    struct native_console_endpoint *endpoint;
    size_t result;
    FILE *file = console ? console->input :
        (handle ? as_file(handle) : selected_input());

    endpoint = console ? console->endpoint :
        (handle ? native_endpoint_for_handle(handle) :
         native_endpoint_for_file(file));

    if (!buffer || length <= 0)
        return 0;
    if (endpoint && (file != stdin || native_input_is_raw())) {
        size_t actual = 0;
        int error = native_console_endpoint_read(endpoint, buffer,
                                                 (size_t)length, &actual);

        if (error != AMIGA_IOERR_OK)
            native_ioerr = error;
        return (LONG)actual;
    }
    if (file == stdin) {
        size_t index;

        /* A raw console Read() is a byte-stream operation.  Reading one
           byte through stdio and then polling between bytes can return the
           first byte of an injected console reply by itself when the
           producer and consumer are scheduled apart.  Unchanged Amiga
           programs such as Vim expect the complete currently available
           control reply, so use the underlying descriptor in raw mode. */
        if (native_input_is_raw()) {
            ssize_t bytes;

            /* Anything the cooked line editor already read off the
               descriptor is console input the program has not seen yet --
               keys typed ahead while the command was starting.  Hand those
               back before touching the descriptor, and without waiting for
               more, exactly as a single availability-sized read would. */
            if (native_editor_line_position < native_editor_line_length) {
                size_t available = native_editor_line_length -
                                   native_editor_line_position;

                if (available > (size_t)length)
                    available = (size_t)length;
                memcpy(buffer, native_editor_line +
                       native_editor_line_position, available);
                native_editor_line_position += available;
                return (LONG)available;
            }
            bytes = ace_console_channel_read(&native_current_console_channel,
                                             buffer, (size_t)length);
            if (bytes < 0)
                native_ioerr = errno;
            return (LONG)bytes;
        }

        for (index = 0; index < (size_t)length; index++) {
            /* A raw console read is commonly preceded by WaitForChar(),
               and must return the bytes available at that instant rather
               than block trying to fill Vim's larger scratch buffer. */
            if (native_input_is_raw() && index != 0 &&
                !WaitForChar(handle, 0))
                break;
            int character = native_input_getc(file);

            if (character == EOF)
                break;
            ((unsigned char *)buffer)[index] = (unsigned char)character;
        }
        result = index;
    } else {
        result = fread(buffer, 1, (size_t)length, file);
    }
    if (result == 0 && ferror(file))
        native_ioerr = errno;
    return (LONG)result;
}

LONG FRead(BPTR handle, APTR buffer, LONG block_size, LONG block_count)
{
    struct native_console_handle *console = native_console_pointer(handle);
    struct native_console_endpoint *endpoint;
    size_t result;
    FILE *file = console ? console->input :
        (handle ? as_file(handle) : selected_input());

    endpoint = console ? console->endpoint :
        (handle ? native_endpoint_for_handle(handle) :
         native_endpoint_for_file(file));

    if (!buffer || block_size <= 0 || block_count <= 0)
        return 0;
    if (endpoint && (file != stdin || native_input_is_raw())) {
        size_t bytes = (size_t)block_size * (size_t)block_count;
        LONG actual;

        actual = Read(handle, buffer, (LONG)bytes);
        if (actual <= 0)
            return 0;
        return actual / block_size;
    }
    if (file == stdin) {
        size_t bytes = (size_t)block_size * (size_t)block_count;
        result = (size_t)Read(handle, buffer, (LONG)bytes) /
                 (size_t)block_size;
    } else {
        result = fread(buffer, (size_t)block_size, (size_t)block_count,
                       file);
    }
    if (result == 0 && ferror(file))
        native_ioerr = errno;
    return (LONG)result;
}

LONG UnGetC(BPTR handle, LONG character)
{
    struct native_console_handle *console = native_console_pointer(handle);
    FILE *file = console ? console->input :
        (handle ? as_file(handle) : selected_input());
    int result;

    /* -1 is AmigaDOS for "the character just read", which is how readitem.c
       puts back the delimiter that ended an item -- a newline, most of the
       time. Pushing back the byte 0xff instead, which is what casting -1
       produces, left that newline consumed and a spurious byte in its place:
       enough for If's skip loop, which reads to the end of the line after
       finding its EndIf, to run on and swallow the line after it. */
    if (character == -1) {
        if (file != native_last_read_file || native_last_read_char == EOF) {
            native_ioerr = ERROR_OBJECT_WRONG_TYPE;
            return ENDSTREAMCH;
        }
        character = native_last_read_char;
    }
    result = ungetc((unsigned char)character, file);
    if (result == EOF) {
        native_ioerr = errno;
        return ENDSTREAMCH;
    }
    native_last_read_char = EOF;
    return result;
}

BPTR SelectInput(BPTR handle)
{
    BPTR old = Input();
    struct native_console_handle *console = native_console_pointer(handle);

    native_input = console ? console->input : (handle ? as_file(handle) :
                                               stdin);
    native_selected_input_endpoint = console ? console->endpoint :
        ((handle == (BPTR)stdin ||
          handle == (BPTR)&native_stdin_handle.amiga) ?
         native_current_console_endpoint : NULL);
    return old;
}

BPTR SelectOutput(BPTR handle)
{
    BPTR old = Output();
    struct native_console_handle *console = native_console_pointer(handle);

    native_output = console ? console->output : (handle ? as_file(handle) :
                                                 stdout);
    native_selected_output_endpoint = console ? console->endpoint :
        ((handle == (BPTR)stdout ||
          handle == (BPTR)&native_stdout_handle.amiga) ?
         native_current_console_endpoint : NULL);
    return old;
}

LONG Seek(BPTR handle, LONG position, LONG mode)
{
    if (native_console_is_handle(handle)) {
        native_ioerr = ERROR_ACTION_NOT_KNOWN;
        return -1;
    }
    FILE *file = handle ? as_file(handle) : selected_input();
    long old = ftell(file);
    int whence = mode == OFFSET_BEGINNING ? SEEK_SET :
                 mode == OFFSET_END ? SEEK_END : SEEK_CUR;

    if (old < 0 || fseek(file, position, whence) != 0) {
        native_ioerr = errno;
        return -1;
    }
    return (LONG)old;
}

void SetProgramName(CONST_STRPTR name)
{
    if (!name)
        name = "";
    strncpy(native_program_name, name, sizeof(native_program_name) - 1);
    native_program_name[sizeof(native_program_name) - 1] = '\0';
}

/* The name the shell entered this command under, which a command reports
   its own errors against. ACE has no CLI-owned command name to read it back
   out of, so the program name is whatever SetProgramName() was last told --
   for a command started by RunCommand(), argv[0]. */
LONG GetProgramName(STRPTR buffer, LONG length)
{
    size_t used = strlen(native_program_name);

    if (!buffer || length <= 0)
        return DOSFALSE;
    if (used >= (size_t)length) {
        native_ioerr = ERROR_LINE_TOO_LONG;
        return DOSFALSE;
    }
    memcpy(buffer, native_program_name, used + 1);
    return DOSTRUE;
}

BPTR SetProgramDir(BPTR lock)
{
    BPTR old = native_program_dir;
    native_program_dir = lock;
    return old;
}

BPTR ParentOfFH(BPTR file)
{
    char current[PATH_MAX];
    struct native_lock *lock;

    (void)file;
    if (native_broker_getcwd(current, sizeof(current)) != 0)
        return BNULL;
    lock = calloc(1, sizeof(*lock));
    if (!lock)
        return BNULL;
    snprintf(lock->path, sizeof(lock->path), "%s", current);
    return lock;
}

LONG ExamineFH(BPTR file, struct FileInfoBlock *fib)
{
    struct stat information;

    if (!file || !fib || fstat(fileno(as_file(file)), &information) != 0) {
        native_ioerr = errno;
        return DOSFALSE;
    }
    fib->fib_DirEntryType = S_ISDIR(information.st_mode) ? 1 : -1;
    fib->fib_Protection = 0;
    return DOSTRUE;
}

void bug(const char *format, ...)
{
    (void)format;
}

/* ReadArgs() and FreeArgs() are imported from AROS's dos.library.  Keep the
   public symbols at the host seam while the real implementation owns the
   template grammar, /M behavior, aliases, and allocation lifetime. */
extern struct RDArgs *ace_aros_ReadArgs(CONST_STRPTR template,
                                        IPTR *arguments,
                                        struct RDArgs *rdargs);
extern void ace_aros_FreeArgs(struct RDArgs *rdargs);

struct RDArgs *ReadArgs(CONST_STRPTR template, IPTR *arguments,
                        struct RDArgs *rdargs)
{
    return ace_aros_ReadArgs(template, arguments, rdargs);
}

void FreeArgs(struct RDArgs *rdargs)
{
    ace_aros_FreeArgs(rdargs);
}
