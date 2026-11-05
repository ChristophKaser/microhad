#define _POSIX_C_SOURCE 200112L
#include "ftp.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FTP_TIMEOUT_SECONDS 5

static int safe_text(const char *s) {
    return s && *s && !strchr(s, '\r') && !strchr(s, '\n');
}

static int send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t sent = send(fd, p, n, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        p += sent; n -= (size_t)sent;
    }
    return 0;
}

static int connection(const char *host, unsigned port) {
    struct addrinfo hints, *list, *it;
    char service[8]; int fd = -1;
    struct timeval timeout;
    memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(host, service, &hints, &list)) return -1;
    timeout.tv_sec = FTP_TIMEOUT_SECONDS; timeout.tv_usec = 0;
    for (it = list; it; it = it->ai_next) {
        int flags, status, error = 0; socklen_t error_len = sizeof(error);
        struct pollfd wait;
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) { close(fd); fd = -1; continue; }
        status = connect(fd, it->ai_addr, it->ai_addrlen);
        if (status && errno == EINPROGRESS) {
            wait.fd = fd; wait.events = POLLOUT; wait.revents = 0;
            do { status = poll(&wait, 1, FTP_TIMEOUT_SECONDS * 1000); } while (status < 0 && errno == EINTR);
            if (status > 0 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) && !error) status = 0;
            else status = -1;
        }
        if (!status && !fcntl(fd, F_SETFL, flags)) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(list); return fd;
}

/* Read replies bytewise: no buffered bytes are lost between commands. */
static int reply(struct ftp_session *s, char *last, size_t cap) {
    char line[1024]; int code = 0, multiline = 0; unsigned lines = 0;
    for (;;) {
        size_t n = 0; char c;
        while (n + 1 < sizeof(line)) {
            ssize_t r = recv(s->control, &c, 1, 0);
            if (r < 0 && errno == EINTR) continue;
            if (r != 1) return -1;
            if (c == '\n') break;
            if (c != '\r') line[n++] = c;
        }
        if (n + 1 == sizeof(line) || ++lines > 100) return -1;
        line[n] = 0;
        if (n < 3 || line[0] < '0' || line[0] > '9' ||
            line[1] < '0' || line[1] > '9' || line[2] < '0' || line[2] > '9') {
            if (multiline) continue;
            return -1;
        }
        { int current = (line[0]-'0')*100 + (line[1]-'0')*10 + line[2]-'0';
          if (!multiline && n > 3 && line[3] == '-') { code = current; multiline = 1; continue; }
          if (multiline && (current != code || n < 4 || line[3] != ' ')) continue;
          if (last && cap) { strncpy(last, line, cap-1); last[cap-1] = 0; }
          return current; }
    }
}

static int command(struct ftp_session *s, const char *verb, const char *arg,
                   char *response, size_t capacity) {
    if (arg && !safe_text(arg)) return -1;
    if (send_all(s->control, verb, strlen(verb)) ||
        (arg && (send_all(s->control, " ", 1) || send_all(s->control, arg, strlen(arg)))) ||
        send_all(s->control, "\r\n", 2)) return -1;
    return reply(s, response, capacity);
}

int ftp_connect(struct ftp_session *s, const char *host, unsigned port,
                const char *user, const char *password, const char *directory) {
    int r; s->control = s->data = -1;
    if (!safe_text(host) || !safe_text(user) || !safe_text(password) ||
        !safe_text(directory) || !port || port > 65535) return -1;
    s->control = connection(host, port);
    if (s->control < 0 || reply(s, 0, 0) != 220) goto fail;
    r = command(s, "USER", user, 0, 0);
    if (r == 331) r = command(s, "PASS", password, 0, 0);
    if (r != 230 || command(s, "TYPE", "I", 0, 0) != 200 ||
        command(s, "CWD", directory, 0, 0) != 250) goto fail;
    return 0;
fail: ftp_close(s); return -1;
}

static int passive(struct ftp_session *s) {
    char line[1024], *p, host[INET_ADDRSTRLEN]; unsigned a,b,c,d,hi,lo;
    struct sockaddr_in peer; socklen_t len = sizeof(peer);
    if (s->data != -1) return -1;
    if (command(s, "PASV", 0, line, sizeof(line)) != 227) return -1;
    p = strchr(line, '(');
    if (!p || sscanf(p, "(%u,%u,%u,%u,%u,%u", &a,&b,&c,&d,&hi,&lo) != 6 ||
        a>255 || b>255 || c>255 || d>255 || hi>255 || lo>255 || !(hi*256+lo)) return -1;
    /* Connect only to the authenticated control peer, not the PASV address. */
    if (getpeername(s->control, (struct sockaddr *)&peer, &len) ||
        !inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host))) return -1;
    s->data = connection(host, hi*256+lo);
    return s->data < 0 ? -1 : 0;
}
int ftp_start_upload(struct ftp_session *s, const char *filename) {
    int r;
    if (!safe_text(filename) || passive(s)) return -1;
    r = command(s, "STOR", filename, 0, 0);
    if (r != 125 && r != 150) { close(s->data); s->data = -1; return -1; }
    return 0;
}

int ftp_write(struct ftp_session *s, const void *data, size_t length) {
    return s->data < 0 ? -1 : send_all(s->data, (const char *)data, length);
}
int ftp_finish_upload(struct ftp_session *s) {
    int r;
    if (s->data < 0) return -1;
    if (close(s->data)) { s->data = -1; return -1; }
    s->data = -1; r = reply(s, 0, 0); return r == 226 || r == 250 ? 0 : -1;
}
int ftp_rename(struct ftp_session *s, const char *from, const char *to) {
    return command(s, "RNFR", from, 0, 0) == 350 &&
           command(s, "RNTO", to, 0, 0) == 250 ? 0 : -1;
}
int ftp_delete(struct ftp_session *s, const char *filename) {
    return command(s, "DELE", filename, 0, 0) == 250 ? 0 : -1;
}
int ftp_list(struct ftp_session *s, int (*entry)(const char *, void *), void *context) {
    char line[512], c; size_t n = 0; int r;
    if (passive(s)) return -1;
    r = command(s, "NLST", 0, 0, 0);
    if (r != 125 && r != 150) { close(s->data); s->data = -1; return -1; }
    for (;;) {
        ssize_t got = recv(s->data, &c, 1, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { close(s->data); s->data = -1; return -1; }
        if (!got) break;
        if (c == '\n') { line[n] = 0; if (n && entry(line, context)) { close(s->data); s->data = -1; return -1; } n = 0; }
        else if (c != '\r') { if (n + 1 >= sizeof(line)) { close(s->data); s->data = -1; return -1; } line[n++] = c; }
    }
    if (n) { line[n] = 0; if (entry(line, context)) { close(s->data); s->data = -1; return -1; } }
    close(s->data); s->data = -1; r = reply(s, 0, 0);
    return r == 226 || r == 250 ? 0 : -1;
}
void ftp_close(struct ftp_session *s) {
    if (s->data >= 0) close(s->data);
    if (s->control >= 0) close(s->control);
    s->data = s->control = -1;
}
