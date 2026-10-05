/* Minimal test-only client for the private Matter service IPC socket. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define HEADER_SIZE 10
#define MAX_PAYLOAD 2048

static int write_all(int fd, const unsigned char *data, size_t length)
{
    while (length != 0) {
        ssize_t written = write(fd, data, length);
        if (written <= 0) return -1;
        data += written;
        length -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, unsigned char *data, size_t length)
{
    while (length != 0) {
        ssize_t received = read(fd, data, length);
        if (received <= 0) return -1;
        data += received;
        length -= (size_t)received;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct sockaddr_un address;
    struct timeval timeout;
    unsigned char frame[HEADER_SIZE + MAX_PAYLOAD];
    unsigned char response[HEADER_SIZE];
    unsigned long opcode, length;
    int fd;

    if (argc != 4 || strlen(argv[1]) >= sizeof(address.sun_path)) return 2;
    opcode = strtoul(argv[2], NULL, 10);
    length = strlen(argv[3]);
    if (opcode < 1 || opcode > 11 || length > MAX_PAYLOAD) return 2;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, argv[1], strlen(argv[1]) + 1);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 3;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return 4;
    }
    memset(frame, 0, HEADER_SIZE);
    frame[0] = (unsigned char)(length & 0xff);
    frame[1] = (unsigned char)((length >> 8) & 0xff);
    frame[2] = 1;
    frame[3] = 0;
    frame[4] = (unsigned char)opcode;
    frame[6] = 1;
    memcpy(frame + HEADER_SIZE, argv[3], length);
    if (write_all(fd, frame, HEADER_SIZE + length) != 0 ||
        read_all(fd, response, sizeof(response)) != 0) {
        close(fd);
        return 5;
    }
    length = (unsigned long)response[0] | ((unsigned long)response[1] << 8);
    if (response[2] != 1 || response[3] != 1 || response[4] != opcode ||
        response[5] != 0 || response[6] != 1 || response[7] != 0 ||
        response[8] != 0 || response[9] != 0 || length > MAX_PAYLOAD ||
        read_all(fd, frame, length) != 0) {
        close(fd);
        return 6;
    }
    if (fwrite(frame, 1, length, stdout) != length || fputc('\n', stdout) == EOF) {
        close(fd);
        return 7;
    }
    close(fd);
    return 0;
}
