#include "cact_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stat.h>
#include <dirent.h>

#define CI_MAX_LBA 4194303u

int ci_read_status(const char *path, char *buf, int max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = 0;
    for (;;) {
        if (n >= max - 1)
            break;
        ssize_t r = read(fd, buf + n, (size_t)(max - 1 - n));
        if (r <= 0)
            break;
        n += (int)r;
    }
    close(fd);
    buf[n] = '\0';
    return n;
}

uint64_t ci_probe_capacity(const char *data_path) {
    int fd = open(data_path, O_RDONLY);
    if (fd < 0)
        return 0;
    unsigned char tmp[512];
    if (pread(fd, tmp, 512, 0) != 512) {
        close(fd);
        return 0;
    }
    uint64_t lo = 0, hi = 1;
    while (hi <= CI_MAX_LBA && pread(fd, tmp, 512, (off_t)(hi * 512)) == 512) {
        lo = hi;
        if (hi > CI_MAX_LBA / 2) {
            hi = CI_MAX_LBA;
            break;
        }
        hi *= 2;
    }
    if (hi <= CI_MAX_LBA && pread(fd, tmp, 512, (off_t)(hi * 512)) != 512) {
        while (hi - lo > 1) {
            uint64_t mid = lo + (hi - lo) / 2;
            if (pread(fd, tmp, 512, (off_t)(mid * 512)) == 512)
                lo = mid;
            else
                hi = mid;
        }
    }
    close(fd);
    return lo + 1;
}

static int status_has(const char *buf, const char *needle) {
    return strstr(buf, needle) != NULL;
}

static int status_field(const char *buf, const char *key, char *out, int outsize) {
    const char *p = strstr(buf, key);
    if (!p)
        return -1;
    p += strlen(key);
    while (*p == ' ' || *p == '\t')
        p++;
    int n = 0;
    while (*p && *p != '\n' && n < outsize - 1)
        out[n++] = *p++;
    out[n] = '\0';
    return n;
}

int ci_list_disks(struct ci_disk *out, int max) {
    int fd = open("/dev", O_RDONLY);
    if (fd < 0)
        return -1;

    int count = 0;
    struct dirent de[8];
    for (;;) {
        int n = getdents(fd, de, sizeof(de));
        if (n <= 0)
            break;
        int entries = n / (int)sizeof(struct dirent);
        for (int i = 0; i < entries && count < max; i++) {
            const char *nm = de[i].d_name;
            if (!nm[0] || !strcmp(nm, ".") || !strcmp(nm, ".."))
                continue;

            struct ci_disk d;
            memset(&d, 0, sizeof(d));
            snprintf(d.name, sizeof(d.name), "%s", nm);
            snprintf(d.base, sizeof(d.base), "/dev/%s", nm);
            snprintf(d.data, sizeof(d.data), "/dev/%s/data", nm);
            snprintf(d.status, sizeof(d.status), "/dev/%s/status", nm);

            struct stat st;
            if (stat(d.base, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            if (stat(d.data, &st) != 0 || !S_ISBLK(st.st_mode))
                continue;

            char s[512];
            if (ci_read_status(d.status, s, sizeof(s)) < 0)
                continue;
            if (status_has(s, "type: partition"))
                continue;

            d.sectors = ci_probe_capacity(d.data);
            out[count++] = d;
        }
    }
    close(fd);
    return count;
}

int ci_disk_parts(const struct ci_disk *d, struct ci_partdev *out, int max) {
    int fd = open("/dev", O_RDONLY);
    if (fd < 0)
        return -1;

    int count = 0;
    struct dirent de[8];
    char want_disk[16];
    snprintf(want_disk, sizeof(want_disk), "disk: %s", d->name);

    for (;;) {
        int n = getdents(fd, de, sizeof(de));
        if (n <= 0)
            break;
        int entries = n / (int)sizeof(struct dirent);
        for (int i = 0; i < entries && count < max; i++) {
            const char *nm = de[i].d_name;
            if (!nm[0] || !strcmp(nm, ".") || !strcmp(nm, ".."))
                continue;

            struct ci_partdev p;
            memset(&p, 0, sizeof(p));
            snprintf(p.name, sizeof(p.name), "%s", nm);

            char base[32], data[40], status[40];
            snprintf(base, sizeof(base), "/dev/%s", nm);
            snprintf(data, sizeof(data), "/dev/%s/data", nm);
            snprintf(status, sizeof(status), "/dev/%s/status", nm);

            struct stat st;
            if (stat(base, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            if (stat(data, &st) != 0 || !S_ISBLK(st.st_mode))
                continue;

            char s[512];
            if (ci_read_status(status, s, sizeof(s)) < 0)
                continue;
            if (!status_has(s, "type: partition"))
                continue;
            if (!status_has(s, want_disk))
                continue;

            char v[24];
            if (status_field(s, "start_lba:", v, sizeof(v)) >= 0)
                p.start = (uint64_t)strtoul(v, NULL, 10);
            if (status_field(s, "size_lba:", v, sizeof(v)) >= 0)
                p.size = (uint64_t)strtoul(v, NULL, 10);
            p.table = status_has(s, "table: gpt") ? 2 : status_has(s, "table: mbr") ? 1 : 0;
            out[count++] = p;
        }
    }
    close(fd);
    return count;
}
