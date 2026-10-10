/* Host-only libc interception: capture actual syslog bodies without /dev/log. */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *tag = "";
void openlog(const char *ident, int option, int facility) {
    (void)option;
    (void)facility;
    tag = ident;
}
void syslog(int priority, const char *format, ...) {
    const char *path = getenv("MATTER_SYSLOG_CAPTURE");
    char body[1024], line[1200];
    va_list args;
    if (!path) return;
    va_start(args, format);
    int size = vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    if (size < 0 || size >= (int)sizeof(body)) abort();
    int length = snprintf(line, sizeof(line), "%s\t%d\t%s\n", tag, priority, body);
    if (length < 0 || length >= (int)sizeof(line)) abort();
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) abort();
    if (write(fd, line, (size_t)length) != length) abort();
    close(fd);
}
