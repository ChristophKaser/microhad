/* Drop only the upload cache of an exact, isolated test artifact. No global cache operations. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char prefix[] = "/tuya/native-test/ot-matter-";
    const char *suffix;
    struct stat st;
    int fd, result;
    if (argc != 2 || strncmp(argv[1], prefix, sizeof(prefix) - 1) != 0) return 2;
    suffix = argv[1] + sizeof(prefix) - 1;
    if (strspn(suffix, "0123456789abcdef") != 32) return 2;
    suffix += 32;
    if (strcmp(suffix, "/probe") != 0 && strcmp(suffix, "/relay") != 0 &&
        strcmp(suffix, "/mthread-matter") != 0) return 2;
    fd = open(argv[1], O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { perror("cache helper open"); return 1; }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) { close(fd); return 2; }
    if (fsync(fd) != 0) { perror("cache helper sync"); close(fd); return 1; }
    result = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
    if (result != 0) { errno = result; perror("cache helper advise"); return 1; }
    return 0;
}
