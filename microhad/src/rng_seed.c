#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SEED_BYTES 32
#define HEADER_BYTES 12
#define FILE_BYTES (HEADER_BYTES + SEED_BYTES)
#define RNDGETENTCNT _IOR('R', 0x00, int)
#define RNDADDENTROPY _IOW('R', 0x03, int[2])

struct rand_pool_info_local {
    int32_t entropy_count;
    int32_t buf_size;
    unsigned char buf[SEED_BYTES];
};

static const unsigned char magic[8] = {'T', 'U', 'Y', 'A', 'R', 'N', 'G', '1'};

static int read_exact(int fd, unsigned char *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t got = read(fd, buf + done, len - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        done += (size_t)got;
    }
    return 0;
}

static int write_exact(int fd, const unsigned char *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t wrote = write(fd, buf + done, len - done);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return -1;
        done += (size_t)wrote;
    }
    return 0;
}

static int make_path(char *out, size_t capacity, const char *path, const char *suffix) {
    int len = snprintf(out, capacity, "%s%s", path, suffix);
    return len > 0 && (size_t)len < capacity ? 0 : -1;
}

static int lock_seed(void) {
    struct flock lock;
    int fd = open("/tmp/ziggurat-rng-seed.lock", O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int restore_seed(const char *path) {
    struct rand_pool_info_local pool;
    struct stat st;
    char used[1024];
    int lock_fd = -1, fd = -1, random_fd = -1, result = 1;
    unsigned char record[FILE_BYTES];
    uint32_t credits;

    if (make_path(used, sizeof(used), path, ".used")) return 2;
    lock_fd = lock_seed();
    if (lock_fd < 0) goto done;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT) { result = 0; goto done; }
        goto done;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0777) != 0600 ||
        st.st_size != FILE_BYTES || rename(path, used) < 0) goto done;

    fd = open(used, O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != 0 || (st.st_mode & 0777) != 0600 || st.st_size != FILE_BYTES ||
        read_exact(fd, record, sizeof(record))) goto done;
    close(fd); fd = -1;

    if (memcmp(record, magic, sizeof(magic))) goto done;
    credits = (uint32_t)record[8] | ((uint32_t)record[9] << 8) |
        ((uint32_t)record[10] << 16) | ((uint32_t)record[11] << 24);
    if (credits < 64 || credits > SEED_BYTES * 8) goto done;

    pool.entropy_count = (int32_t)credits;
    pool.buf_size = SEED_BYTES;
    memcpy(pool.buf, record + HEADER_BYTES, SEED_BYTES);
    random_fd = open("/dev/random", O_RDWR);
    if (random_fd < 0 || ioctl(random_fd, RNDADDENTROPY, &pool) < 0) goto done;
    unlink(used);
    printf("restored %u bits of saved kernel entropy\n", (unsigned int)credits);
    result = 0;
done:
    if (fd >= 0) close(fd);
    if (random_fd >= 0) close(random_fd);
    if (lock_fd >= 0) close(lock_fd);
    if (result) perror("rng-seed restore");
    return result;
}

static int wait_ready(int fd) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    for (;;) {
        int result = poll(&pfd, 1, -1);
        if (result > 0 && (pfd.revents & POLLIN)) return 0;
        if (result < 0 && errno == EINTR) continue;
        if (result == 0) continue;
        return -1;
    }
}

static int save_seed(const char *path) {
    char temp[1024], directory[1024];
    unsigned char record[FILE_BYTES];
    int lock_fd = -1, random_fd = -1, urandom_fd = -1, out_fd = -1;
    int entropy = 0, credits, dir_fd = -1, result = 1;
    const char *slash;

    if (make_path(temp, sizeof(temp), path, ".new")) return 2;
    lock_fd = lock_seed();
    if (lock_fd < 0) goto done;
    random_fd = open("/dev/random", O_RDONLY);
    if (random_fd < 0 || wait_ready(random_fd) < 0 ||
        ioctl(random_fd, RNDGETENTCNT, &entropy) < 0) goto done;
    credits = entropy > SEED_BYTES * 8 ? SEED_BYTES * 8 : entropy;
    if (credits < 64) { errno = EAGAIN; goto done; }

    urandom_fd = open("/dev/urandom", O_RDONLY);
    if (urandom_fd < 0 || read_exact(urandom_fd, record + HEADER_BYTES, SEED_BYTES)) goto done;
    memcpy(record, magic, sizeof(magic));
    record[8] = (unsigned char)credits;
    record[9] = (unsigned char)(credits >> 8);
    record[10] = (unsigned char)(credits >> 16);
    record[11] = (unsigned char)(credits >> 24);
    out_fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (out_fd < 0 || fchmod(out_fd, 0600) < 0 ||
        write_exact(out_fd, record, sizeof(record)) || fsync(out_fd) < 0) goto done;
    if (close(out_fd) < 0) { out_fd = -1; goto done; }
    out_fd = -1;
    if (rename(temp, path) < 0) goto done;
    slash = strrchr(path, '/');
    if (slash && (size_t)(slash - path) < sizeof(directory)) {
        size_t len = (size_t)(slash - path);
        if (len == 0) len = 1;
        memcpy(directory, path, len);
        directory[len] = '\0';
        dir_fd = open(directory, O_RDONLY | O_DIRECTORY);
        if (dir_fd >= 0) (void)fsync(dir_fd);
    }
    {
        char used[1024];
        if (!make_path(used, sizeof(used), path, ".used")) unlink(used);
    }
    printf("saved %d bits of kernel entropy\n", credits);
    result = 0;
done:
    if (out_fd >= 0) close(out_fd);
    if (dir_fd >= 0) close(dir_fd);
    if (urandom_fd >= 0) close(urandom_fd);
    if (random_fd >= 0) close(random_fd);
    if (lock_fd >= 0) close(lock_fd);
    if (result) perror("rng-seed save");
    return result;
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "restore") && strcmp(argv[1], "save")) ||
        argv[2][0] != '/') {
        fprintf(stderr, "usage: rng-seed restore|save /absolute/seed-file\n");
        return 2;
    }
    if (geteuid() != 0) { fputs("rng-seed requires root\n", stderr); return 1; }
    return !strcmp(argv[1], "restore") ? restore_seed(argv[2]) : save_seed(argv[2]);
}
