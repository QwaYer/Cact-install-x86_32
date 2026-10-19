#include "cact_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stat.h>

#define MODE_DIR  0755
#define MODE_FILE 0644

int ci_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int ci_mkdir_p(const char *path) {
    char tmp[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            struct stat st;
            if (stat(tmp, &st) != 0) {
                if (mkdir(tmp, MODE_DIR) != 0) {
                    if (stat(tmp, &st) != 0)
                        return -1;
                }
            }
            *p = '/';
        }
    }
    struct stat st;
    if (stat(tmp, &st) != 0) {
        if (mkdir(tmp, MODE_DIR) != 0) {
            if (stat(tmp, &st) != 0)
                return -1;
        }
    }
    return 0;
}

int ci_write_file(const char *path, const char *data, uint32_t len) {
    if (len == (uint32_t)-1)
        len = (uint32_t)strlen(data);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, MODE_FILE);
    if (fd < 0)
        return -1;
    uint32_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += (uint32_t)n;
    }
    close(fd);
    chmod(path, MODE_FILE);
    return 0;
}

int ci_copy_file(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0)
        return -1;
    if (S_ISDIR(st.st_mode))
        return -1;                 /* callers copy files only */
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, MODE_FILE);
    if (out < 0) {
        close(in);
        return -1;
    }
    /* Read to EOF, not to st.st_size: a source that cannot report its size — a
     * device such as /dev/localrepo — reports 0, and a size-driven loop would
     * copy nothing while returning success, leaving an empty destination. */
    char buf[4096];
    uint32_t total = 0;
    int rc = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0) { rc = -1; break; }
        if (n == 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) { rc = -1; break; }
            off += w;
        }
        if (rc != 0)
            break;
        total += (uint32_t)n;
    }
    close(in);
    close(out);
    if (rc != 0 || total == 0) {
        /* Never leave a partial — or empty — copy behind: the caller cannot
         * tell it apart from a good one once the file exists. */
        unlink(dst);
        return -1;
    }
    chmod(dst, st.st_mode & 07777);
    return 0;
}

int ci_mount(const char *dev, const char *mnt, const char *fstype) {
    return mount(dev, mnt, fstype, 0, NULL);
}

int ci_umount(const char *dev_or_mnt) {
    return umount(dev_or_mnt);
}
