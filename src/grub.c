#include "cact_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stat.h>

#define GRUB_KERNEL_SECTOR_OFF  0x5c
#define GRUB_DISKBOOT_LIST_OFF  0x1f4
#define GRUB_MASTER_BOOT_RECORD 0x1b8
#define SECT 512

#define GPT_CORE_EMBED_LBA 34u

static const char *BIOS_MOD_FILES[] = {
    "boot.mod",
    "multiboot2.mod",
    "part_msdos.mod",
    "part_gpt.mod",
    "ext2.mod",
    "fat.mod",
    "search.mod",
    "search_fs_file.mod",
    "normal.mod",
    "configfile.mod",
    "ls.mod",
    "cat.mod",
    "test.mod",
    "true.mod",
    "echo.mod",
    "reboot.mod",
    "sleep.mod",
    "read.mod",
    "help.mod",
    "minicmd.mod",
    "terminal.mod",
    "terminfo.mod",
    "biosdisk.mod",
    NULL,
};

static const char *GRUB_CFG =
    "set timeout=5\n"
    "set default=0\n"
    "menuentry \"CactOS\" {\n"
    "    search --no-floppy --set=root --file /boot/cctkfs.img\n"
    "    multiboot2 /boot/kernel.bin\n"
    "    module2 /boot/cctkfs.img cctkfs\n"
    "    boot\n"
    "}\n";

static int read_file_all(const char *path, char **out, uint32_t *outlen) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    uint32_t cap = 8192, n = 0;
    char *b = malloc(cap);
    if (!b) {
        close(fd);
        return -1;
    }
    for (;;) {
        if (n + 512 >= cap) {
            char *nb = realloc(b, cap * 2);
            if (!nb) {
                free(b);
                close(fd);
                return -1;
            }
            b = nb;
            cap *= 2;
        }
        ssize_t r = read(fd, b + n, cap - n);
        if (r <= 0)
            break;
        n += (uint32_t)r;
    }
    close(fd);
    *out = b;
    *outlen = n;
    return 0;
}

void ci_grub_cfg_write(const char *root_mnt) {
    char path[256];
    snprintf(path, sizeof(path), "%s/boot/grub", root_mnt);
    ci_mkdir_p(path);
    snprintf(path, sizeof(path), "%s/boot/grub/grub.cfg", root_mnt);
    ci_write_file(path, GRUB_CFG, (uint32_t)strlen(GRUB_CFG));
}

int ci_deploy_grub_files(const struct ci_plan *plan, const char *root_mnt,
                         const char *payload_grub) {
    char src[300], dst[300];

    if (plan->scheme == SCHEME_GPT_EFI) {
        snprintf(src, sizeof(src), "%s/i386-efi/bootia32.efi", payload_grub);
        if (!ci_file_exists(src)) {
            printf("EFI loader payload not found (%s); ESP left without loader\n", src);
            return -1;
        }
        snprintf(dst, sizeof(dst), "%s/boot/EFI/BOOT/bootia32.efi", root_mnt);
        snprintf(src, sizeof(src), "%s/boot/EFI/BOOT", root_mnt);
        ci_mkdir_p(src);
        if (ci_copy_file(src, dst) != 0) {
            printf("failed to copy EFI loader\n");
            return -1;
        }
        snprintf(dst, sizeof(dst), "%s/boot/EFI/BOOT/grub.cfg", root_mnt);
        ci_write_file(dst, GRUB_CFG, (uint32_t)strlen(GRUB_CFG));
        return 0;
    }

    snprintf(dst, sizeof(dst), "%s/boot/grub/i386-pc", root_mnt);
    ci_mkdir_p(dst);
    for (int i = 0; BIOS_MOD_FILES[i]; i++) {
        snprintf(src, sizeof(src), "%s/i386-pc/%s", payload_grub,
                 BIOS_MOD_FILES[i]);
        if (!ci_file_exists(src))
            continue;
        snprintf(dst, sizeof(dst), "%s/boot/grub/i386-pc/%s", root_mnt,
                 BIOS_MOD_FILES[i]);
        if (ci_copy_file(src, dst) != 0)
            printf("warning: failed to stage grub module %s\n",
                   BIOS_MOD_FILES[i]);
    }
    return 0;
}

int ci_grub_install(const struct ci_disk *d, const struct ci_plan *plan,
                    const char *payload_grub) {
    if (plan->scheme == SCHEME_GPT_EFI)
        return 0;

    uint32_t embed = (plan->scheme == SCHEME_GPT_BIOS) ? GPT_CORE_EMBED_LBA : 1u;

    char boot_path[300], core_path[300];
    snprintf(boot_path, sizeof(boot_path), "%s/i386-pc/boot.img", payload_grub);
    snprintf(core_path, sizeof(core_path), "%s/i386-pc/core.img", payload_grub);

    char *boot = NULL, *core = NULL;
    uint32_t blen = 0, clen = 0;
    if (read_file_all(boot_path, &boot, &blen) != 0 || blen < SECT) {
        if (blen)
            free(boot);
        printf("grub: boot.img missing from payload (%s)\n", boot_path);
        return -1;
    }
    if (read_file_all(core_path, &core, &clen) != 0 || clen == 0) {
        free(boot);
        printf("grub: core.img missing from payload (%s)\n", core_path);
        return -1;
    }

    uint64_t core_sectors = (clen + SECT - 1) / SECT;

    struct ci_partdev parts[CI_MAX_PARTS];
    int np = ci_disk_parts(d, parts, CI_MAX_PARTS);
    uint64_t first = 0;
    for (int i = 0; i < np; i++) {
        if (parts[i].start > 0 && (!first || parts[i].start < first))
            first = parts[i].start;
    }
    if (!first || embed + core_sectors > first) {
        printf("grub: cannot embed core.img (%u sectors) at LBA %u: "
               "first partition starts at %u. Re-partition with a 1 MiB "
               "aligned layout.\n",
               (unsigned)core_sectors, (unsigned)embed, (unsigned)first);
        free(boot);
        free(core);
        return -1;
    }

    if (embed != 1) {
        uint32_t v = embed;
        memcpy(boot + GRUB_KERNEL_SECTOR_OFF, &v, 4);
        memset(boot + GRUB_KERNEL_SECTOR_OFF + 4, 0, 4);
        v = (uint32_t)(embed + 1);
        memcpy(core + GRUB_DISKBOOT_LIST_OFF, &v, 4);
        memset(core + GRUB_DISKBOOT_LIST_OFF + 4, 0, 4);
    }

    int fd = open(d->data, O_RDWR);
    if (fd < 0) {
        printf("grub: cannot open %s\n", d->data);
        free(boot);
        free(core);
        return -1;
    }

    char mbr[SECT];
    if (pread(fd, mbr, SECT, 0) != SECT) {
        close(fd);
        free(boot);
        free(core);
        return -1;
    }
    memcpy(mbr, boot, GRUB_MASTER_BOOT_RECORD);
    if (pwrite(fd, mbr, SECT, 0) != SECT) {
        close(fd);
        free(boot);
        free(core);
        return -1;
    }

    free(boot);

    ssize_t w = pwrite(fd, core, clen, (off_t)(embed * SECT));
    close(fd);
    free(core);
    if (w != (ssize_t)clen) {
        printf("grub: failed to write core.img\n");
        return -1;
    }
    printf("grub: boot.img -> LBA 0, core.img (%u sectors) -> LBA %u\n",
           (unsigned)core_sectors, (unsigned)embed);
    return 0;
}
