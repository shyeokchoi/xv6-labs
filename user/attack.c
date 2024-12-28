#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

int
main(int argc, char *argv[])
{
    char* end = sbrk(PGSIZE * 32);
    end = end + 16 * PGSIZE;
    char* secret = end + 32;
    printf("secret: %s\n", secret);
    write(2, secret, 8);
    exit(0);
}
