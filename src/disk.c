#include "cact_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stat.h>
#include <dirent.h>
#include <ioctl_abi.h>

#define CI_MAX_LBA 4194303u
#define CI_SECT    512u
#define CI_MAX_NODES 32

/* ── node naming ──────────────────────────────────────────────────────────
 * Every whole disk and every partition is one block node in /dev (the devfs
 * has no per-device data/status sub-nodes).  Partition names mirror the
 * kernel's blkdev_partition_name(): <disk><n>, or <disk>p<n> when the disk
 * name already ends in a digit.
 */

static int name_ends_digit(const char *s) {
    size_t l = strlen(s);
    return l > 0 && s[l - 1] >= '0' && s[l - 1] <= '9';
}

static void part_name(const char *disk, int number, char *out, int cap) {
    if (name_ends_digit(disk))
        snprintf(out, cap, "%sp%d", disk, number);
    else
        snprintf(out, cap, "%s%d", disk, number);
}

/* Name shape only: is `part` a partition node of `disk`? */
static int is_part_of(const char *part, const char *disk) {
    size_t dl = strlen(disk);
    if (strncmp(part, disk, dl) != 0)
        return 0;
    const char *r = part + dl;
    if (r[0] == 'p' && r[1] >= '0' && r[1] <= '9')
        r++;
    if (!r[0])
        return 0;
    for (; *r; r++)
        if (*r < '0' || *r > '9')
            return 0;
    return 1;
}

static int node_is_block(const char *name) {
    char path[64];
    struct stat st;
    snprintf(path, sizeof(path), "/dev/%s", name);
    return stat(path, &st) == 0 && S_ISBLK(st.st_mode);
}

/* Every block node currently exposed under /dev. */
static int scan_dev(char names[][16], int max) {
    int fd = open("/dev", O_RDONLY);
    if (fd < 0)
        return -1;

    int n = 0;
    struct dirent de[8];
    for (;;) {
        int r = getdents(fd, de, sizeof(de));
        if (r <= 0)
            break;
        int entries = r / (int)sizeof(struct dirent);
        for (int i = 0; i < entries && n < max; i++) {
            const char *nm = de[i].d_name;
            if (!nm[0] || !strcmp(nm, ".") || !strcmp(nm, ".."))
                continue;
            if (strlen(nm) >= 16 || !node_is_block(nm))
                continue;
            snprintf(names[n], 16, "%s", nm);
            n++;
        }
    }
    close(fd);
    return n;
}

/* ── capacity ─────────────────────────────────────────────────────────────
 * The devfs sets a block node's size to its byte capacity (saturating at
 * 4 GiB-1), so stat() is authoritative for every disk we can install onto.
 * Probe the last readable sector only when that value is missing/saturated.
 */

uint64_t ci_probe_capacity(const char *node_path) {
    int fd = open(node_path, O_RDONLY);
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

static uint64_t node_sectors(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISBLK(st.st_mode)) {
        uint64_t bytes = st.st_size;
        if (bytes && bytes < 0xFFFFFFFFull)
            return (bytes + CI_SECT - 1) / CI_SECT;
    }
    return ci_probe_capacity(path);
}

int ci_list_disks(struct ci_disk *out, int max) {
    char names[CI_MAX_NODES][16];
    int n = scan_dev(names, CI_MAX_NODES);
    if (n < 0)
        return -1;

    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        int is_part = 0;
        for (int j = 0; j < n; j++) {
            if (j != i && is_part_of(names[i], names[j])) {
                is_part = 1;
                break;
            }
        }
        if (is_part)
            continue;

        struct ci_disk d;
        memset(&d, 0, sizeof(d));
        snprintf(d.name, sizeof(d.name), "%s", names[i]);
        snprintf(d.base, sizeof(d.base), "/dev/%s", names[i]);
        d.sectors = node_sectors(d.base);
        if (!d.sectors)
            continue;
        out[count++] = d;
    }
    return count;
}

/* ── partition table read ─────────────────────────────────────────────── */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int guid_is_zero(const uint8_t *p) {
    for (int i = 0; i < 16; i++)
        if (p[i])
            return 0;
    return 1;
}

static int mbr_type_extended(uint8_t t) {
    return t == 0x05 || t == 0x0F || t == 0x85;
}

static void add_part(const struct ci_disk *d, int number, uint64_t start,
                     uint64_t len, int table, struct ci_partdev *out,
                     int *count, int max) {
    if (*count >= max || !start || !len)
        return;

    struct ci_partdev p;
    memset(&p, 0, sizeof(p));
    part_name(d->name, number, p.name, (int)sizeof(p.name));
    if (!node_is_block(p.name))
        return;   /* the kernel did not expose this partition */
    p.start = start;
    p.size = len;
    p.table = table;
    out[(*count)++] = p;
}

int ci_disk_parts(const struct ci_disk *d, struct ci_partdev *out, int max) {
    int fd = open(d->base, O_RDONLY);
    if (fd < 0)
        return -1;

    uint8_t lba0[CI_SECT], lba1[CI_SECT];
    if (pread(fd, lba0, CI_SECT, 0) != (ssize_t)CI_SECT ||
        pread(fd, lba1, CI_SECT, (off_t)CI_SECT) != (ssize_t)CI_SECT) {
        close(fd);
        return 0;
    }

    int count = 0;

    if (!memcmp(lba1, "EFI PART", 8)) {
        uint64_t ents_lba = rd64(lba1 + 72);
        uint32_t num      = rd32(lba1 + 80);
        uint32_t esize    = rd32(lba1 + 84);
        if (esize < 128 || esize > 512)
            esize = 128;
        if (num > 128)
            num = 128;
        if (ents_lba && ents_lba <= CI_MAX_LBA) {
            uint8_t ent[128];
            for (uint32_t i = 0; i < num && count < max; i++) {
                off_t off = (off_t)(ents_lba * CI_SECT) +
                            (off_t)((uint64_t)i * esize);
                if (pread(fd, ent, 128, off) != 128)
                    break;
                if (guid_is_zero(ent))
                    continue;
                uint64_t first = rd64(ent + 32);
                uint64_t last  = rd64(ent + 40);
                if (!first || last < first)
                    continue;
                add_part(d, (int)i + 1, first, last - first + 1, 2,
                         out, &count, max);
            }
        }
    } else if (lba0[510] == 0x55 && lba0[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = lba0 + 446 + i * 16;
            uint8_t  type  = e[4];
            uint32_t start = rd32(e + 8);
            uint32_t len   = rd32(e + 12);
            if (type == 0 || start == 0 || len == 0)
                continue;
            if (mbr_type_extended(type) || type == 0xEE)
                continue;
            add_part(d, i + 1, start, len, 1, out, &count, max);
        }
    }

    close(fd);
    return count;
}

/* Ask the kernel to re-read the partition table of `d` and re-expose its
 * /dev nodes.  fdisk does this itself after 'w'; we call it again when the
 * nodes are not there yet, so the install does not depend on ordering. */
int ci_disk_rescan(const struct ci_disk *d) {
    int fd = open("/dev/sys", O_RDWR);
    if (fd < 0)
        return -1;
    int r = ioctl(fd, CACT_SYSCTL_BLKDEV_RESCAN, (void *)d->name);
    close(fd);
    return r;
}
