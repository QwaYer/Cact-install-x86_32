#include "cact_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stat.h>

#define PAYLOAD_BOOT "/lib/cact-install/boot"
#define PAYLOAD_GRUB "/lib/cact-install/grub"

static void die(const char *msg) {
    printf("cact-install: %s\n", msg);
    ci_term_done();
    exit(1);
}

static void banner(void) {
    printf("\n");
    printf("  CactOS x86_32 installer\n");
    printf("  wizard: disk -> scheme -> partitions -> mkfs -> files -> grub -> configs\n");
    printf("\n");
}

static void fmt_size(uint64_t sectors, char *out, int n) {
    uint64_t bytes = sectors * 512;
    if (bytes >= (1024ull * 1024 * 1024))
        snprintf(out, n, "%u GiB", (unsigned)(bytes >> 30));
    else if (bytes >= (1024ull * 1024))
        snprintf(out, n, "%u MiB", (unsigned)(bytes >> 20));
    else
        snprintf(out, n, "%u KiB", (unsigned)(bytes >> 10));
}

static void ensure_modules(void) {
    printf("[0] loading filesystem modules\n");
    module_load("/lib/ext4.cctk", (unsigned)-1, (unsigned)-1);
    module_load("/lib/fat32.cctk", (unsigned)-1, (unsigned)-1);
}

static void print_parts(const struct ci_disk *d) {
    struct ci_partdev parts[CI_MAX_PARTS];
    int np = ci_disk_parts(d, parts, CI_MAX_PARTS);
    if (np <= 0) {
        printf("  (no partitions / no partition table yet)\n");
        return;
    }
    for (int i = 0; i < np; i++) {
        char sz[24];
        fmt_size(parts[i].size, sz, sizeof(sz));
        printf("  %s  start %u  %u sectors (%s)%s\n",
               parts[i].name, (unsigned)parts[i].start,
               (unsigned)parts[i].size, sz,
               parts[i].table == 2 ? "  gpt" :
               parts[i].table == 1 ? "  mbr" : "");
    }
}

static int select_disk(struct ci_disk *sel) {
    struct ci_disk disks[CI_MAX_DISKS];
    int nd = ci_list_disks(disks, CI_MAX_DISKS);
    if (nd <= 0) {
        printf("no block disks found under /dev\n");
        return 0;
    }

    printf("Disks found:\n");
    for (int i = 0; i < nd; i++) {
        char sz[24];
        fmt_size(disks[i].sectors, sz, sizeof(sz));
        printf("  %s  %s\n", disks[i].base, sz);
    }
    printf("\n");

    const char **items = malloc(sizeof(char *) * nd);
    if (!items)
        die("out of memory");
    for (int i = 0; i < nd; i++) {
        items[i] = disks[i].base;
    }
    int c = ci_menu("Select the disk to install CactOS onto:", items, nd);
    free(items);
    if (c <= 0)
        return 0;
    *sel = disks[c - 1];
    printf("\nSelected target: %s\n", sel->base);
    printf("Current partitions on %s:\n", sel->name);
    print_parts(sel);
    printf("\n");
    return 1;
}

static int choose_scheme(void) {
    static const char *items[] = {
        "MBR / BIOS - classic legacy BIOS, single ext4 root",
        "GPT / BIOS - legacy BIOS with GPT, single ext4 root",
        "GPT / EFI  - UEFI, FAT32 ESP + ext4 root",
        "Manual     - partition with fdisk, then assign roles",
    };
    int c = ci_menu("Choose the partition scheme:", items, 4);
    if (c <= 0)
        return 0;
    return c == 4 ? SCHEME_MANUAL : c;
}

static int check_payload(void) {
    char k[256], i[256];
    snprintf(k, sizeof(k), PAYLOAD_BOOT "/kernel.bin");
    snprintf(i, sizeof(i), PAYLOAD_BOOT "/cctkfs.img");
    if (ci_file_exists(k) && ci_file_exists(i))
        return 1;
    return 0;
}

static int scan_source(char *kernel, char *img) {
    struct ci_disk disks[CI_MAX_DISKS];
    int nd = ci_list_disks(disks, CI_MAX_DISKS);
    for (int di = 0; di < nd; di++) {
        struct ci_partdev parts[CI_MAX_PARTS];
        int np = ci_disk_parts(&disks[di], parts, CI_MAX_PARTS);
        for (int pi = 0; pi < np; pi++) {
            char dev[32];
            snprintf(dev, sizeof(dev), "/dev/%s", parts[pi].name);
            if (ci_mount(dev, MNT_SRC, "auto") != 0)
                continue;
            char k[256], i[256];
            snprintf(k, sizeof(k), MNT_SRC "/boot/kernel.bin");
            snprintf(i, sizeof(i), MNT_SRC "/boot/cctkfs.img");
            if (ci_file_exists(k) && ci_file_exists(i)) {
                snprintf(kernel, 256, "%s", k);
                snprintf(img, 256, "%s", i);
                printf("install media found on %s\n", dev);
                return 1;
            }
            ci_umount(MNT_SRC);
        }
    }
    return 0;
}

static int locate_sources(char *kernel, char *img, int *media_mounted) {
    char k[256], i[256];
    *media_mounted = 0;

    if (check_payload()) {
        snprintf(kernel, 256, PAYLOAD_BOOT "/kernel.bin");
        snprintf(img, 256, PAYLOAD_BOOT "/cctkfs.img");
        printf("using bundled payload (%s)\n", PAYLOAD_BOOT);
        return 1;
    }

    if (scan_source(k, i)) {
        snprintf(kernel, 256, "%s", k);
        snprintf(img, 256, "%s", i);
        *media_mounted = 1;
        return 1;
    }

    printf("kernel.bin / cctkfs.img were not found bundled or on any mounted\n");
    printf("partition.  You can provide paths now.\n");
    char buf[256];
    if (ci_prompt("path to kernel.bin: ", buf, sizeof(buf)) && buf[0]) {
        snprintf(kernel, 256, "%s", buf);
    } else {
        return 0;
    }
    if (ci_prompt("path to cctkfs.img: ", buf, sizeof(buf)) && buf[0]) {
        snprintf(img, 256, "%s", buf);
    } else {
        return 0;
    }
    return 1;
}

static int format_part(const struct ci_lpart *p) {
    char dev[40];
    snprintf(dev, sizeof(dev), "/dev/%s", p->dev);
    printf("mkfs %s -> %s (%s)\n", p->fstype, dev, p->mnt);

    if (!strcmp(p->fstype, PT_EXT4)) {
        char *argv[] = { "/sbin/mkfs.ext4", "-q", "-L", "cactos", dev, NULL };
        return ci_exec("/sbin/mkfs.ext4", argv);
    }
    if (!strcmp(p->fstype, PT_FAT32)) {
        char *argv[] = { "/sbin/mkfs.fat32", "-q", "-n", "CACTOS-ESP", dev, NULL };
        return ci_exec("/sbin/mkfs.fat32", argv);
    }
    return -1;
}

static int mkfs_step(const struct ci_plan *plan) {
    printf("[mkfs] formatting target partitions (data will be lost)\n");
    if (!ci_confirm("Format the partitions listed above now?", 1))
        return 0;
    for (int i = 0; i < plan->nparts; i++) {
        const struct ci_lpart *p = &plan->parts[i];
        if (!p->to_format)
            continue;
        if (format_part(p) != 0) {
            printf("mkfs failed on /dev/%s\n", p->dev);
            return -1;
        }
    }
    return 0;
}

static int mount_step(const struct ci_plan *plan, const char *root_dev) {
    if (ci_mkdir_p(MNT_ROOT) != 0 || ci_mkdir_p(MNT_SRC) != 0)
        return -1;

    printf("[mount] %s ext4 -> %s\n", root_dev, MNT_ROOT);
    if (ci_mount(root_dev, MNT_ROOT, PT_EXT4) != 0) {
        printf("cannot mount %s (ext4 module loaded? partition present?)\n", root_dev);
        return -1;
    }
    if (plan->esp_idx >= 0) {
        const char *esp = plan->parts[plan->esp_idx].dev;
        char esp_dev[40];
        snprintf(esp_dev, sizeof(esp_dev), "/dev/%s", esp);
        if (ci_mkdir_p(MNT_ROOT "/boot") != 0)
            return -1;
        printf("[mount] %s fat32 -> %s\n", esp_dev, MNT_ROOT "/boot");
        if (ci_mount(esp_dev, MNT_ROOT "/boot", PT_FAT32) != 0) {
            printf("cannot mount ESP %s (fat32 module loaded?)\n", esp_dev);
            return -1;
        }
    }
    return 0;
}

static int deploy_step(const struct ci_plan *plan, const char *kernel_src,
                       const char *img_src) {
    printf("[deploy] installing kernel and userland image\n");
    char *argv[] = { "/sbin/cact-rootfs", "-k", (char *)kernel_src,
                     "-m", (char *)img_src, (char *)MNT_ROOT, NULL };
    int rc = ci_exec("/sbin/cact-rootfs", argv);
    if (rc != 0) {
        printf("cact-rootfs failed (exit %d)\n", rc);
        return -1;
    }
    ci_grub_cfg_write(MNT_ROOT);
    if (ci_deploy_grub_files(plan, MNT_ROOT, PAYLOAD_GRUB) != 0) {
        printf("warning: grub files were not staged\n");
    }
    return 0;
}

static const char *cgf =
    "# CactOS supervisor config (generated by cact-install)\n"
    "restart_policy=on-failure\n"
    "rescue_shell=1\n"
    "crash_limit=3\n"
    "cooldown_sec=2\n"
    "services=\n";

static void config_step(const struct ci_plan *plan, const char *root_dev) {
    char path[256];
    char buf[128];
    char hostname[64] = "cactos";

    if (ci_prompt("Hostname [cactos]: ", buf, sizeof(buf)) && buf[0])
        snprintf(hostname, sizeof(hostname), "%s", buf);

    snprintf(path, sizeof(path), MNT_ROOT "/etc/hostname");
    ci_write_file(path, hostname, (uint32_t)strlen(hostname));

    snprintf(path, sizeof(path), MNT_ROOT "/etc/cgoct.conf");
    ci_write_file(path, cgf, (uint32_t)strlen(cgf));

    snprintf(path, sizeof(path), MNT_ROOT "/etc/mounts");
    {
        char mounts[160];
        int n = 0;
        for (int i = 0; i < plan->nparts; i++) {
            const struct ci_lpart *p = &plan->parts[i];
            if (!p->role)
                continue;
            n += snprintf(mounts + n, sizeof(mounts) - (size_t)n, "%s\n", p->dev);
        }
        ci_write_file(path, mounts, (uint32_t)n);
    }

    snprintf(path, sizeof(path), MNT_ROOT "/etc/cact-install.conf");
    snprintf(buf, sizeof(buf),
             "target=%s\nroot=%s\nscheme=%d\nhostname=%s\n",
             root_dev, plan->parts[plan->root_idx].dev, plan->scheme, hostname);
    ci_write_file(path, buf, (uint32_t)strlen(buf));
    printf("[configs] wrote hostname, cgoct.conf, mounts, cact-install.conf\n");
}

static void umount_step(const struct ci_plan *plan) {
    if (plan->esp_idx >= 0) {
        ci_umount(MNT_ROOT "/boot");
        char dev[40];
        snprintf(dev, sizeof(dev), "/dev/%s", plan->parts[plan->esp_idx].dev);
        ci_umount(dev);
    }
    ci_umount(MNT_ROOT);
    char root_dev[40];
    snprintf(root_dev, sizeof(root_dev), "/dev/%s",
             plan->parts[plan->root_idx].dev);
    ci_umount(root_dev);
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    ci_term_init();
    banner();

    if (geteuid() != 0)
        printf("warning: not running as root; /dev/sys operations will fail\n");

    ensure_modules();

    struct ci_disk disk;
    if (!select_disk(&disk))
        die("no target disk selected");

    if (!ci_confirm("Installing will DESTROY the current partition table of "
                    "this disk. Continue?", 0)) {
        die("aborted by user");
    }

    int scheme = choose_scheme();
    if (!scheme)
        die("aborted by user");

    struct ci_plan plan;
    memset(&plan, 0, sizeof(plan));

    if (scheme == SCHEME_MANUAL) {
        printf("[partitions] run fdisk on %s (create a table, use 1 MiB "
               "alignment)\n", disk.base);
        printf("when fdisk exits the partitions will be assigned roles\n");
        ci_press();
        if (ci_run_fdisk(&disk, NULL) != 0)
            die("fdisk step failed");
        struct ci_partdev parts[CI_MAX_PARTS];
        int np = ci_disk_parts(&disk, parts, CI_MAX_PARTS);
        if (np <= 0)
            die("no partitions on disk after fdisk");
        if (ci_layout_manual(&disk, &plan, parts, np) != 0)
            die("role assignment failed");
    } else {
        long esp = 512;
        if (scheme == SCHEME_GPT_EFI)
            esp = ci_prompt_num("ESP size in MiB", 512);
        struct ci_fdisk_cmd cmd;
        if (ci_layout_guided(&disk, scheme, esp, &plan, &cmd) != 0)
            die("cannot build the requested layout");
        ci_fill_part_devices(&disk, &plan);

        printf("\nPlanned layout:\n");
        ci_plan_print(&plan);
        if (!ci_confirm("\nWrite this partition table to the disk?", 1))
            die("aborted by user");

        if (ci_run_fdisk(&disk, &cmd) != 0)
            die("partition table step failed");

        struct ci_partdev parts[CI_MAX_PARTS];
        int np = ci_disk_parts(&disk, parts, CI_MAX_PARTS);
        if (np <= 0)
            die("kernel did not expose any partitions after the write");
        printf("partitions now on %s:\n", disk.name);
        print_parts(&disk);
        printf("\n");
    }

    printf("\nLayout to install:\n");
    ci_plan_print(&plan);
    printf("\n");

    if (mkfs_step(&plan) != 0)
        die("formatting failed");

    char root_dev[40];
    snprintf(root_dev, sizeof(root_dev), "/dev/%s",
             plan.parts[plan.root_idx].dev);
    if (mount_step(&plan, root_dev) != 0)
        die("cannot mount target filesystems");

    char kernel_src[256], img_src[256];
    int media_mounted = 0;
    if (!locate_sources(kernel_src, img_src, &media_mounted))
        die("kernel.bin / cctkfs.img source not found");

    if (deploy_step(&plan, kernel_src, img_src) != 0)
        die("deploy failed");

    config_step(&plan, root_dev);

    printf("[grub] installing boot loader\n");
    if (plan.scheme == SCHEME_GPT_EFI)
        printf("EFI scheme: loader already staged on the ESP by deploy\n");
    else if (ci_grub_install(&disk, &plan, PAYLOAD_GRUB) != 0)
        printf("warning: GRUB embed failed; the disk may not be bootable.\n"
               "         Bundle the payload (make grub-bundle + stage-media) and reinstall.\n");

    umount_step(&plan);
    if (media_mounted)
        ci_umount(MNT_SRC);

    printf("\nInstallation complete.\n");
    printf("  disk   : %s\n", disk.base);
    printf("  root   : /dev/%s\n", plan.parts[plan.root_idx].dev);
    if (plan.esp_idx >= 0)
        printf("  esp    : /dev/%s\n", plan.parts[plan.esp_idx].dev);
    printf("  layout : scheme %d (%s)\n", plan.scheme,
           plan.scheme == SCHEME_MBR_BIOS ? "MBR BIOS" :
           plan.scheme == SCHEME_GPT_BIOS ? "GPT BIOS" :
           plan.scheme == SCHEME_GPT_EFI ? "GPT EFI" : "manual");

    if (ci_confirm("Reboot now?", 0)) {
        if (reboot(RB_AUTOBOOT) != 0)
            printf("reboot failed\n");
    }

    ci_term_done();
    return 0;
}
