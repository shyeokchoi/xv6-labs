#include "kernel/types.h"
#include "user/user.h"

#define STDOUT_FD 1

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(STDOUT_FD, "usage: sleep <# of ticks>\n");
        exit(0);
    }

    int tick = atoi(argv[1]);
    sleep(tick);
    exit(0);
}
