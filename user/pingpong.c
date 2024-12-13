// clang-format off
#include "kernel/types.h"
#include "kernel/stat.h"
// clang-format on
#include "user/user.h"
#define ASSERT(condition)                                                                                              \
    if (!(condition)) {                                                                                                \
        fprintf(2, "assertion failed: %s\n", #condition);                                                              \
        exit(1);                                                                                                       \
    }

int main() {
    int p2c[2]; // parent->child pipe: [0] for read, [1] for write
    int c2p[2]; // child->parent pipe: [0] for read, [1] for write
    char buf;

    pipe(p2c);
    pipe(c2p);

    int pid = fork();
    if (pid == 0) {
        // chile process
        char c2p_data = 'c';

        close(p2c[1]);
        close(c2p[0]);
        read(p2c[0], &buf, 1);
        ASSERT(buf == 'p'); // for testing
        fprintf(1, "%d: received ping\n", getpid());
        write(c2p[1], &c2p_data, 1);
        close(c2p[1]);
        close(p2c[0]);
        exit(0);
    } else if (pid > 0) {
        // parent process
        char p2c_data = 'p';

        close(p2c[0]);
        close(c2p[1]);
        write(p2c[1], &p2c_data, 1);
        read(c2p[0], &buf, 1);
        ASSERT(buf == 'c'); // for testing
        fprintf(1, "%d: received pong\n", getpid());
        close(p2c[1]);
        close(c2p[0]);
        wait(0);
        exit(0);
    } else {
        fprintf(2, "failed fork\n");
    }
}
