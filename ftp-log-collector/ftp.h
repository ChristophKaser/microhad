#ifndef GATEWAY_DIAGNOSTICS_FTP_H
#define GATEWAY_DIAGNOSTICS_FTP_H

#include <stddef.h>

struct ftp_session { int control; int data; };

/* All operations return zero on success and never print credentials. */
int ftp_connect(struct ftp_session *session, const char *host, unsigned port,
                const char *user, const char *password, const char *directory);
int ftp_start_upload(struct ftp_session *session, const char *filename);
int ftp_write(struct ftp_session *session, const void *data, size_t length);
int ftp_finish_upload(struct ftp_session *session);
int ftp_rename(struct ftp_session *session, const char *from, const char *to);
int ftp_delete(struct ftp_session *session, const char *filename);
int ftp_list(struct ftp_session *session, int (*entry)(const char *, void *), void *context);
void ftp_close(struct ftp_session *session);

#endif
