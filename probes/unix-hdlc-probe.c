/* Test-only stdin/stdout bridge to the experiment's local Unix HDLC endpoint. */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int copy_bytes(int from, int to)
{
    char buffer[2048];
    ssize_t count = read(from, buffer, sizeof(buffer));
    ssize_t offset = 0;
    if (count <= 0) return count == 0 ? 0 : -1;
    while (offset < count) {
        ssize_t written = write(to, buffer + offset, count - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        offset += written;
    }
    return 1;
}

int main(int argc, char **argv)
{
    struct sockaddr_un address;
    int fd;
    if (argc != 2 || strncmp(argv[1], "/tmp/ziggurat-hdlc-", 19) != 0) return 2;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(argv[1]) >= sizeof(address.sun_path)) return 2;
    strcpy(address.sun_path, argv[1]);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("connect experiment endpoint"); return 1;
    }
    for (;;) {
        fd_set reads;
        int result;
        FD_ZERO(&reads); FD_SET(0, &reads); FD_SET(fd, &reads);
        result = select(fd + 1, &reads, NULL, NULL, NULL);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) break;
        if (FD_ISSET(0, &reads) && copy_bytes(0, fd) <= 0) break;
        if (FD_ISSET(fd, &reads) && copy_bytes(fd, 1) <= 0) break;
    }
    close(fd);
    return 0;
}
