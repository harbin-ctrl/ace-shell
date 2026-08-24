#include "broker_client.h"
#include "broker_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int usage(const char *program)
{
    fprintf(stderr, "usage: %s pwd | cd PATH | assign NAME PATH | resolve PATH | "
                    "name PATH | "
                    "getvar NAME | setvar NAME VALUE | setgvar NAME VALUE | "
                    "delvar NAME | result | cli | doslist | assigns | hold | "
                    "status | socket | "
                    "setresult RC RESULT2 | say warm VOICE | say status\n", program);
    return 2;
}

/*
 * Claims the session the way a shell does and then does nothing until it is
 * killed, which is what makes session lifetime observable without starting a
 * window: the session exists while this runs and is gone once it stops.
 */
static int hold_session(void)
{
    if (native_broker_attach() != 0) {
        perror("attach");
        return 1;
    }
    puts("held");
    fflush(stdout);
    for (;;)
        pause();
}

int main(int argc, char **argv)
{
    char result[4096];

    if (argc == 2 && strcmp(argv[1], "pwd") == 0) {
        if (native_broker_getcwd(result, sizeof(result)) != 0)
            return 1;
        puts(result);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "cd") == 0) {
        if (native_broker_resolve_path(argv[2], result, sizeof(result)) != 0)
            return 1;
        return native_broker_setcwd(result) == 0 ? 0 : 1;
    }
    if (argc == 4 && strcmp(argv[1], "assign") == 0) {
        if (native_broker_assign(argv[2], argv[3]) != 0) {
            perror("assign");
            return 1;
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "resolve") == 0) {
        if (native_broker_resolve_path(argv[2], result, sizeof(result)) != 0)
            return 1;
        puts(result);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "name") == 0) {
        if (native_broker_name_from_host(argv[2], result, sizeof(result)) != 0)
            return 1;
        puts(result);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "getvar") == 0) {
        if (native_broker_getvar(argv[2], 0, result, sizeof(result)) != 0)
            return 1;
        puts(result);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "setvar") == 0)
        return native_broker_setvar(argv[2], argv[3],
                                    AMIGA_BROKER_VAR_LOCAL) == 0 ? 0 : 1;
    if (argc == 4 && strcmp(argv[1], "setgvar") == 0)
        return native_broker_setvar(argv[2], argv[3],
                                    AMIGA_BROKER_VAR_GLOBAL) == 0 ? 0 : 1;
    if (argc == 3 && strcmp(argv[1], "delvar") == 0)
        return native_broker_deletevar(argv[2], 0) == 0 ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "result") == 0) {
        if (native_broker_getresult(result, sizeof(result)) != 0)
            return 1;
        puts(result);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "cli") == 0) {
        if (native_broker_getcli(result, sizeof(result)) != 0)
            return 1;
        fputs(result, stdout);
        putchar('\n');
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "setresult") == 0)
        return native_broker_setresult((int32_t)strtol(argv[2], NULL, 10),
                                       (int32_t)strtol(argv[3], NULL, 10)) == 0 ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "hold") == 0)
        return hold_session();
    if (argc == 2 && strcmp(argv[1], "doslist") == 0) {
        if (native_broker_listdos(result, sizeof(result)) != 0)
            return 1;
        fputs(result, stdout);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "socket") == 0) {
        /* Answers "where would I connect" without starting anything. */
        printf("%s\n", amiga_broker_socket_path());
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        printf("client-protocol\t0x%08x\n",
               (unsigned)AMIGA_BROKER_PROTOCOL_VERSION);
        printf("client-sys\t%s\n", amiga_broker_system_root());
        printf("client-socket\t%s\n", amiga_broker_socket_path());
        if (native_broker_status(result, sizeof(result)) != 0)
            return 1;
        fputs(result, stdout);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "assigns") == 0) {
        if (native_broker_listassigns(result, sizeof(result)) != 0)
            return 1;
        fputs(result, stdout);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "say") == 0 &&
        strcmp(argv[2], "warm") == 0) {
        if (native_broker_say_warm(argv[3]) != 0) {
            perror("say warm");
            return 1;
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "say") == 0 &&
        strcmp(argv[2], "status") == 0) {
        if (native_broker_say_status(result, sizeof(result)) != 0) {
            perror("say status");
            return 1;
        }
        fputs(result, stdout);
        return 0;
    }
    return usage(argv[0]);
}
