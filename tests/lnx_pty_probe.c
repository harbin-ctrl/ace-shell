#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    printf("isatty %d %d %d\n",
           isatty(STDIN_FILENO), isatty(STDOUT_FILENO),
           isatty(STDERR_FILENO));
    fflush(stdout);
    return 0;
}
