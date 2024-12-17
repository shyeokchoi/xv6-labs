// clang-format off
#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
// clang-format on
#include "user/user.h"

#define BUF_SIZE 512

/* if same file name return 1 , else return 0 */
int cmp_file_name(char* path, char* file_name)
{
    uint path_len = strlen(path);
    uint file_name_len = strlen(file_name);

    if (file_name_len > path_len) {
        return 0;
    }

    char* pptr = path + path_len;
    while (pptr != path && *(pptr - 1) != '/') {
        --pptr;
    }

    char* fptr = file_name;
    while (*pptr == *fptr) {
        if (*pptr == '\0') {
            return 1;
        }
        ++pptr;
        ++fptr;
    }
    return 0;
}

int search(char* path, char* file_name)
{
    struct stat st;
    int fd;

    if (cmp_file_name(path, file_name)) { /* when file name match */
        fprintf(1, "%s\n", path);
    }

    if ((fd = open(path, O_RDONLY)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return -1;
    }

    if ((fstat(fd, &st)) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return -1;
    }

    if (st.type == T_DIR) {
        uint64 bytes_read = 0;
        char* buf = malloc(st.size);
        struct dirent* dirent_arr = (struct dirent*)buf; // easy access to buf

        uint64 n; // temp var to store return value of `read` function for each call
        while ((n = read(fd, buf + bytes_read, st.size - bytes_read)) > 0) {
            bytes_read += n;
        }

        if (n < 0) {
            fprintf(2, "read error.\n");
            free(buf);
            close(fd);
            return -1;
        }
        // reaching here, `buf` full of the directory file

        char next_path[BUF_SIZE];
        uint path_len = strlen(path);
        memcpy(next_path, path, path_len);
        next_path[path_len] = '/';
        // next_path : path + '/'

        for (int i = 0; i < st.size / sizeof(struct dirent); ++i) {
            if (dirent_arr[i].inum == 0 || strcmp(dirent_arr[i].name, ".") == 0
                || strcmp(dirent_arr[i].name, "..") == 0) {
                continue;
            }

            if (strlen(path) + 1 + strlen(dirent_arr[i].name) + 1 > BUF_SIZE) { /* plus 1's for '/' and '\0' */
                fprintf(2, "too long path.\n");
                free(buf);
                close(fd);
                return -1;
            }

            strcpy(next_path + path_len + 1, dirent_arr[i].name);
            if (search(next_path, file_name) < 0) {
                free(buf);
                close(fd);
                return -1;
            }
        }
        free(buf);
        close(fd);
        return 0;
    }

    close(fd);
    return 0;
}

int main(int argc, char* argv[])
{
    int res;

    if (argc != 3) {
        fprintf(1, "Usage: find <root> <file_name>\n");
        exit(0);
    }

    res = search(argv[1], argv[2]);
    if (res < 0) {
        fprintf(2, "find failed\n");
        exit(1);
    }

    exit(0);
}
