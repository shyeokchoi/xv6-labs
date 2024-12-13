#include "kernel/types.h"
#include "user.h"

#define FALSE (0)
#define TRUE (1)
#define BYTES_OF_INT (4)
static int depth = -1;

void cleanup_pipe(int is_child_exists, int read_from_parent_fd, int write_to_child_fd)
{
    close(read_from_parent_fd);
    if (is_child_exists) {
        close(write_to_child_fd);
        wait((void*)0);
    }
}

/**
 * @note gen_pipe_stage is responsible of closing @param read_from_parent_fd
 */
void gen_pipe_stage(int read_from_parent_fd)
{
    ++depth;
    int is_child_exists = FALSE;
    int p[2]; // pipe to communicate with child process. p[0]: read end / p[1]: write end
    int org;  // original prime number passed from the parent

    int res = read(read_from_parent_fd, &org, BYTES_OF_INT);
    if (res < 0) {
        close(read_from_parent_fd);
        fprintf(2, "consumer<%d>: initial read failed\n", depth);
        return;
    }
    if (res == 0) {
        // parent closed the pipe right after generating child. terminate..
        close(read_from_parent_fd);
        return;
    }

    fprintf(1, "prime %d\n", org);

    int x;
    while ((res = read(read_from_parent_fd, &x, BYTES_OF_INT)) > 0) {
        if (x % org == 0) {
            continue;
        }

        // when a new input which is not a multiple of `org` is found, pass it to the child
        if (is_child_exists) {
            res = write(p[1], &x, BYTES_OF_INT);
            if (res <= 0) {
                fprintf(2, "consumer<%d>: writing output to child failed.\n", depth);
                cleanup_pipe(is_child_exists, read_from_parent_fd, p[1]);
                return;
            }
        } else {
            // no child was generated before. make one...
            if (pipe(p) < 0) {
                fprintf(2, "consumer<%d>: pipe failed.\n", depth);
                cleanup_pipe(is_child_exists, read_from_parent_fd, p[1]);
                return;
            }

            int pid = fork();
            if (pid > 0) {
                is_child_exists = TRUE;
                close(p[0]); // close read end of the pipe connected to the child
                if (write(p[1], &x, BYTES_OF_INT) < BYTES_OF_INT) {
                    fprintf(2, "consumer<%d>: initial write to the child failed.\n", depth);
                    cleanup_pipe(is_child_exists, read_from_parent_fd, p[1]);
                    return;
                }
            } else if (pid == 0) {
                close(read_from_parent_fd); // close read_from_parent_fd because its ownership is held by the parent
                close(p[1]);                // close write end of the pipe connected to the parent
                gen_pipe_stage(p[0]);
                return;
            } else {
                fprintf(2, "consumer<%d>: fork failed.\n", depth);
                cleanup_pipe(is_child_exists, read_from_parent_fd, p[1]);
                return;
            }
        }
    }

    if (res < 0) {
        fprintf(2, "consumer<%d>: reading input from parent failed.\n", depth);
    }
    cleanup_pipe(is_child_exists, read_from_parent_fd, p[1]);
    return;
}

int main()
{
    int p[2]; // p[0]: read end / p[1]: write end
    if (pipe(p) < 0) {
        fprintf(2, "pipe failed.\n");
        exit(1);
    };

    int pid = fork();
    if (pid > 0) { // producer (top-level process)
        close(p[0]);
        for (int i = 2; i < 280; ++i) {
            int res = write(p[1], &i, BYTES_OF_INT);
            if (res < 0) {
                fprintf(2, "top level process: write failed.\n");
                break;
            }
        }
        close(p[1]);
        wait((void*)0);
    } else if (pid == 0) { // consumer
        close(p[1]);
        gen_pipe_stage(p[0]); // NOTE: this function is responsible of closing p[0]
    } else {
        fprintf(2, "fork error.\n");
        exit(1);
    }

    return 0;
}
