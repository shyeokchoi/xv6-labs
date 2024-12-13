// clang-format off
#include "kernel/types.h"
// clang-format on
#include "kernel/param.h"
#include "user/user.h"

#define BUF_SIZE 1000
#define NULL ((void*)0)
#define TRUE 1
#define FALSE 0

/**
 * @param fd: file descriptor to read from
 * @param buf: buffer to store the read line
 * @return 0 if EOF, -1 if error, >0 if success
 */
int read_line(int fd, char* buf)
{
    int i = 0;
    char c;

    int res;
    while ((res = read(fd, &c, 1)) > 0) {
        if (c == '\n') {
            buf[i] = '\0';
            return res;
        }
        buf[i] = c;
        ++i;
    }

    buf[i] = '\0';
    return res;
}

/**
 * @param argc: number of arguments passed to xargs
 * @param original_argv: arguments passed to xargs
 * @param buf: buffer to store the read line
 * @return arguments to be passed to exec
 */
char** parse_argv(int argc, char* original_argv[], char* buf)
{
    // move args passed to xargs
    static char* exec_argv[MAXARG];

    int i = 1;
    for (; i < argc; ++i) {
        exec_argv[i - 1] = original_argv[i];
    }
    --i;

    // split input line
    int st = 0;
    int en = 0;
    while (buf[en] != '\0') {
        if (buf[en] == ' ') {
            buf[en] = '\0';
            exec_argv[i] = &buf[st];
            ++i;
            st = en + 1;
        }
        ++en;
    }
    exec_argv[i] = &buf[st];
    exec_argv[++i] = NULL;

    return exec_argv;
}

int main(int argc, char* argv[])
{
    if (argc <= 1) {
        fprintf(2, "Usage: xargs <command> [args...]\n");
        exit(1);
    }
    char buf[BUF_SIZE]; // buffer for reading line

    while (1) {
        int res = read_line(0, buf);
        if (res < 0) {
            fprintf(2, "error while reading line.\n");
            exit(1);
        }

        if (res == 0) {
            break;
        }

        // move args passed to xargs
        char** exec_argv = parse_argv(argc, argv, buf);

        // exec
        int pid = fork();
        if (pid > 0) {
            // parent
            pid = wait((int*)0);
        } else if (pid == 0) {
            // child
            exec(argv[1], exec_argv);
            fprintf(2, "exec error\n");
        } else {
            fprintf(2, "fork error\n");
            exit(1);
        }
    }

    return 1;
}
