/* Test-only Unix stream -> host IPv4 simulator relay. Never opens a UART. */
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int forward(int from, int to)
{
    char buffer[2048];
    ssize_t count = read(from, buffer, sizeof(buffer));
    ssize_t offset = 0;
    if (count < 0 && errno == EINTR) return 1;
    if (count <= 0) return count == 0 ? 0 : -1;
    while (offset < count) {
        ssize_t written = send(to, buffer + offset, count - offset, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        offset += written;
    }
    return 1;
}

int main(int argc, char **argv)
{
    struct sockaddr_un local;
    struct sockaddr_in remote;
    struct stat existing;
    char *end;
    long port;
    int listener = -1, backend = -1, client = -1, result = 1;
    if (argc != 4 || strncmp(argv[1], "/tmp/ot-sim-", 12) != 0) return 2;
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    if (strlen(argv[1]) >= sizeof(local.sun_path)) return 2;
    strcpy(local.sun_path, argv[1]);
    if (lstat(argv[1], &existing) == 0 || errno != ENOENT) return 2;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    if (inet_pton(AF_INET, argv[2], &remote.sin_addr) != 1) return 2;
    port = strtol(argv[3], &end, 10);
    if (*end || port <= 0 || port > 65535) return 2;
    remote.sin_port = htons((unsigned short)port);
    alarm(45);
    umask(077);
    backend = socket(AF_INET, SOCK_STREAM, 0);
    if (backend < 0 || connect(backend, (struct sockaddr *)&remote, sizeof(remote)) != 0) goto done;
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&local, sizeof(local)) != 0 || listen(listener, 1) != 0) goto done;
    client = accept(listener, NULL, NULL);
    if (client < 0) goto done;
    for (;;) {
        fd_set reads;
        int n;
        FD_ZERO(&reads); FD_SET(client, &reads); FD_SET(backend, &reads);
        n = select((client > backend ? client : backend) + 1, &reads, NULL, NULL, NULL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (FD_ISSET(client, &reads)) {
            n = forward(client, backend);
            if (n <= 0) { result = n == 0 ? 0 : 1; break; }
        }
        if (FD_ISSET(backend, &reads)) {
            n = forward(backend, client);
            if (n <= 0) { result = n == 0 ? 0 : 1; break; }
        }
    }
done:
    if (result) perror("simulator relay");
    if (client >= 0) close(client);
    if (listener >= 0) { close(listener); unlink(argv[1]); }
    if (backend >= 0) close(backend);
    return result;
}
