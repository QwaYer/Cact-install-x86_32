#include "cact_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stat.h>

#define PAYLOAD_BOOT    "/lib/cact-install/boot"
#define PAYLOAD_GRUB    "/lib/cact-install/grub"

#define TOOL_FDISK      "/sbin/fdisk"
#define TOOL_MKFS_EXT4  "/sbin/mkfs.ext4"
#define TOOL_MKFS_FAT32 "/sbin/mkfs.fat32"
#define TOOL_ROOTFS     "/sbin/cact-rootfs"

/* Wizard step results. */
#define W_NEXT  1
#define W_BACK  0
#define W_QUIT (-1)

/* Everything the wizard carries from one step to the next. */
struct wiz {
    struct ci_disk disk;
    struct ci_plan plan;
    int  scheme;
    char root_dev[40];
    char kernel_src[256];
    char img_src[256];
    int  media_mounted;
    int  root_mounted;
    int  esp_mounted;
};

static void die(const char *msg) {
    printf("cact-install: %s\n", msg);
    ci_term_done();
    exit(1);
}

static int tool_ok(const char *path) {
    if (ci_file_exists(path))
        return 1;
    printf("required program not found: %s\n", path);
    return 0;
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

/* ── payload location ─────────────────────────────────────────────────── */

static int check_payload(void) {
    char k[256], i[256];
    snprintf(k, sizeof(k), PAYLOAD_BOOT "/kernel.bin");
    snprintf(i, sizeof(i), PAYLOAD_BOOT "/cctkfs.img");
    return ci_file_exists(k) && ci_file_exists(i);
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

    printf("kernel.bin / cctkfs.img were not found bundled or on any partition.\n");
    char buf[256];
    if (ci_prompt("path to kernel.bin (empty = skip): ", buf, sizeof(buf)) && buf[0]) {
        if (!ci_file_exists(buf)) {
            printf("%s does not exist\n", buf);
            return 0;
        }
        snprintf(kernel, 256, "%s", buf);
    } else {
        return 0;
    }
    if (ci_prompt("path to cctkfs.img (empty = skip): ", buf, sizeof(buf)) && buf[0]) {
        if (!ci_file_exists(buf)) {
            printf("%s does not exist\n", buf);
            return 0;
        }
        snprintf(img, 256, "%s", buf);
    } else {
        return 0;
    }
    return 1;
}

/* ── formatting / mounting ────────────────────────────────────────────── */

static int format_part(const struct ci_lpart *p) {
    char dev[40];
    snprintf(dev, sizeof(dev), "/dev/%s", p->dev);
    printf("mkfs %s -> %s (%s)\n", p->fstype, dev, p->mnt);

    if (!strcmp(p->fstype, PT_EXT4)) {
        if (!tool_ok(TOOL_MKFS_EXT4))
            return -1;
        char *argv[] = { TOOL_MKFS_EXT4, "-q", "-L", "cactos", dev, NULL };
        return ci_exec(TOOL_MKFS_EXT4, argv);
    }
    if (!strcmp(p->fstype, PT_FAT32)) {
        if (!tool_ok(TOOL_MKFS_FAT32))
            return -1;
        char *argv[] = { TOOL_MKFS_FAT32, "-q", "-n", "CACTOS-ESP", dev, NULL };
        return ci_exec(TOOL_MKFS_FAT32, argv);
    }
    return -1;
}

static int deploy_step(const struct ci_plan *plan, const char *kernel_src,
                       const char *img_src) {
    printf("[deploy] installing kernel and userland image\n");
    if (!tool_ok(TOOL_ROOTFS))
        return -1;
    char *argv[] = { TOOL_ROOTFS, "-k", (char *)kernel_src,
                     "-m", (char *)img_src, (char *)MNT_ROOT, NULL };
    int rc = ci_exec(TOOL_ROOTFS, argv);
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

static int write_one(const char *path, const char *data, uint32_t len) {
    if (ci_write_file(path, data, len) != 0) {
        printf("cannot write %s\n", path);
        return -1;
    }
    return 0;
}

static int config_step(const struct ci_plan *plan, const char *root_dev) {
    char path[256];
    char buf[160];
    char hostname[64] = "cactos";
    int bad = 0;

    if (ci_prompt("Hostname [cactos]: ", buf, sizeof(buf)) && buf[0])
        snprintf(hostname, sizeof(hostname), "%s", buf);

    if (ci_mkdir_p(MNT_ROOT "/etc") != 0) {
        printf("cannot create " MNT_ROOT "/etc\n");
        return -1;
    }

    snprintf(path, sizeof(path), MNT_ROOT "/etc/hostname");
    bad |= write_one(path, hostname, (uint32_t)strlen(hostname));

    snprintf(path, sizeof(path), MNT_ROOT "/etc/cgoct.conf");
    bad |= write_one(path, cgf, (uint32_t)strlen(cgf));

    snprintf(path, sizeof(path), MNT_ROOT "/etc/mounts");
    {
        char mounts[160];
        int n = 0, cap = (int)sizeof(mounts);
        for (int i = 0; i < plan->nparts && n < cap - 1; i++) {
            const struct ci_lpart *p = &plan->parts[i];
            if (!p->role)
                continue;
            const char *tgt = (p->role == ROLE_ESP) ? "/boot/efi" : "/";
            int w = snprintf(mounts + n, (size_t)(cap - n), "/dev/%s %s %s\n",
                             p->dev, tgt, p->fstype);
            if (w <= 0 || w >= cap - n)
                break;
            n += w;
        }
        bad |= write_one(path, mounts, (uint32_t)n);
    }

    snprintf(path, sizeof(path), MNT_ROOT "/etc/cact-install.conf");
    snprintf(buf, sizeof(buf),
             "target=%s\nroot=%s\nscheme=%d\nhostname=%s\n",
             root_dev, plan->parts[plan->root_idx].dev, plan->scheme, hostname);
    bad |= write_one(path, buf, (uint32_t)strlen(buf));

    if (bad)
        return -1;
    printf("[configs] wrote hostname, cgoct.conf, mounts, cact-install.conf\n");
    return 0;
}

/* ── steps ────────────────────────────────────────────────────────────── */

static int step_disk(struct wiz *w) {
    struct ci_disk disks[CI_MAX_DISKS];
    int nd = ci_list_disks(disks, CI_MAX_DISKS);
    if (nd <= 0) {
        printf("no block disks found under /dev — nothing to install onto\n");
        return W_QUIT;
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
    for (int i = 0; i < nd; i++)
        items[i] = disks[i].base;
    int c = ci_menu("Select the disk to install CactOS onto:", items, nd);
    free(items);
    if (c <= 0)
        return W_QUIT;          /* back at the first step means quit */

    w->disk = disks[c - 1];
    w->root_mounted = 0;
    w->esp_mounted = 0;
    memset(&w->plan, 0, sizeof(w->plan));
    w->plan.root_idx = -1;
    w->plan.esp_idx = -1;
    w->root_dev[0] = '\0';

    printf("\nSelected target: %s\n", w->disk.base);
    printf("Current partitions on %s:\n", w->disk.name);
    print_parts(&w->disk);
    printf("\n");
    return W_NEXT;
}

static int step_confirm(struct wiz *w) {
    printf("Target disk: %s\n", w->disk.base);
    int c = ci_confirm("Installing will DESTROY the current partition table "
                       "of this disk. Continue?", 0);
    if (c < 0)
        return W_QUIT;
    if (!c) {
        printf("\nNothing has been changed — back to disk selection.\n\n");
        return W_BACK;
    }
    return W_NEXT;
}

static int step_scheme(struct wiz *w) {
    static const char *items[] = {
        "MBR / BIOS - classic legacy BIOS, single ext4 root",
        "GPT / BIOS - legacy BIOS with GPT, single ext4 root",
        "GPT / EFI  - UEFI, FAT32 ESP + ext4 root",
        "Manual     - partition with fdisk, then assign roles",
    };
    int c = ci_menu("Choose the partition scheme:", items, 4);
    if (c < 0)
        return W_QUIT;
    if (c == 0)
        return W_BACK;
    w->scheme = (c == 4) ? SCHEME_MANUAL : c;
    return W_NEXT;
}

/* Re-read the table; nudge the kernel once if the nodes are not there yet. */
static int verify_parts(struct wiz *w) {
    struct ci_partdev parts[CI_MAX_PARTS];
    int np = ci_disk_parts(&w->disk, parts, CI_MAX_PARTS);
    if (np <= 0) {
        printf("partitions are not visible yet; asking the kernel to re-scan %s\n",
               w->disk.name);
        ci_disk_rescan(&w->disk);
        np = ci_disk_parts(&w->disk, parts, CI_MAX_PARTS);
    }
    if (np <= 0) {
        printf("the kernel still does not expose any partitions on %s\n",
               w->disk.name);
        return 0;
    }
    printf("partitions now on %s:\n", w->disk.name);
    print_parts(&w->disk);
    printf("\n");
    return 1;
}

static int step_manual(struct wiz *w) {
    printf("[partitions] run fdisk on %s\n", w->disk.base);
    printf("create a table (o = MBR, g = GPT) and use 1 MiB alignment\n");
    ci_press();

    if (!tool_ok(TOOL_FDISK))
        return W_BACK;
    if (ci_run_fdisk(&w->disk, NULL) != 0)
        printf("fdisk did not complete cleanly; checking what it wrote\n");

    struct ci_partdev parts[CI_MAX_PARTS];
    int np = ci_disk_parts(&w->disk, parts, CI_MAX_PARTS);
    if (np <= 0) {
        ci_disk_rescan(&w->disk);
        np = ci_disk_parts(&w->disk, parts, CI_MAX_PARTS);
    }
    if (np <= 0) {
        printf("no partitions found on %s after fdisk\n", w->disk.name);
        return W_BACK;
    }

    printf("partitions on %s:\n", w->disk.name);
    print_parts(&w->disk);
    printf("\n");

    if (ci_layout_manual(&w->disk, &w->plan, parts, np) != 0)
        return W_BACK;

    printf("\nPlanned layout:\n");
    ci_plan_print(&w->plan);
    printf("\n");
    return W_NEXT;
}

static int step_layout(struct wiz *w) {
    if (w->scheme == SCHEME_MANUAL)
        return step_manual(w);

    if (!tool_ok(TOOL_FDISK))
        return W_BACK;

    long esp = 512;
    if (w->scheme == SCHEME_GPT_EFI) {
        esp = ci_prompt_num("ESP size in MiB", 512);
        if (esp < 8)
            esp = 512;
    }

    struct ci_fdisk_cmd cmd;
    if (ci_layout_guided(&w->disk, w->scheme, esp, &w->plan, &cmd) != 0) {
        printf("\ncannot build that layout on this disk — pick another scheme\n\n");
        return W_BACK;
    }
    ci_fill_part_devices(&w->disk, &w->plan);

    printf("\nPlanned layout:\n");
    ci_plan_print(&w->plan);

    int c = ci_confirm("\nWrite this partition table to the disk?", 1);
    if (c < 0)
        return W_QUIT;
    if (!c) {
        printf("Layout discarded — back to scheme selection.\n\n");
        return W_BACK;
    }

    printf("[partitions] writing a new table to %s\n", w->disk.base);
    if (ci_run_fdisk(&w->disk, &cmd) != 0) {
        printf("fdisk could not write the table; the disk is left as it "
               "was\n");
        return W_BACK;
    }
    if (!verify_parts(w))
        return W_BACK;
    return W_NEXT;
}

static int step_mkfs(struct wiz *w) {
    printf("[mkfs] partitions to format (their contents will be lost):\n");
    int any = 0;
    for (int i = 0; i < w->plan.nparts; i++) {
        const struct ci_lpart *p = &w->plan.parts[i];
        if (!p->to_format)
            continue;
        printf("  /dev/%s  %s\n", p->dev, p->fstype);
        any = 1;
    }
    if (!any) {
        printf("nothing to format\n");
        return W_BACK;
    }
    printf("\n");

    for (;;) {
        int c = ci_confirm("Format these partitions now?", 1);
        if (c < 0)
            return W_QUIT;
        if (!c)
            return W_BACK;

        int bad = 0;
        for (int i = 0; i < w->plan.nparts; i++) {
            const struct ci_lpart *p = &w->plan.parts[i];
            if (!p->to_format)
                continue;
            if (format_part(p) != 0) {
                printf("mkfs failed on /dev/%s\n", p->dev);
                bad = 1;
            }
        }
        if (!bad)
            return W_NEXT;

        c = ci_confirm("Formatting failed. Retry?", 1);
        if (c < 0)
            return W_QUIT;
        if (!c)
            return W_BACK;
    }
}

static int step_mount(struct wiz *w) {
    const struct ci_lpart *root = &w->plan.parts[w->plan.root_idx];
    snprintf(w->root_dev, sizeof(w->root_dev), "/dev/%s", root->dev);

    if (ci_mkdir_p(MNT_ROOT) != 0 || ci_mkdir_p(MNT_SRC) != 0) {
        printf("cannot create " MNT_ROOT " / " MNT_SRC "\n");
        return W_BACK;
    }

    if (!w->root_mounted) {
        printf("[mount] %s (%s) -> %s\n", w->root_dev, root->fstype, MNT_ROOT);
        if (ci_mount(w->root_dev, MNT_ROOT, root->fstype) != 0) {
            printf("cannot mount %s (is the %s module loaded?)\n",
                   w->root_dev, root->fstype);
            return W_BACK;
        }
        w->root_mounted = 1;
    }

    if (w->plan.esp_idx >= 0 && !w->esp_mounted) {
        const struct ci_lpart *esp = &w->plan.parts[w->plan.esp_idx];
        char dev[40];
        snprintf(dev, sizeof(dev), "/dev/%s", esp->dev);
        if (ci_mkdir_p(esp->mnt) != 0) {
            printf("cannot create %s\n", esp->mnt);
            return W_BACK;
        }
        printf("[mount] %s (%s) -> %s\n", dev, esp->fstype, esp->mnt);
        if (ci_mount(dev, esp->mnt, esp->fstype) != 0) {
            printf("cannot mount ESP %s (is the %s module loaded?)\n",
                   dev, esp->fstype);
            return W_BACK;
        }
        w->esp_mounted = 1;
    }
    return W_NEXT;
}

static int step_sources(struct wiz *w) {
    if (w->media_mounted) {
        if (ci_file_exists(w->kernel_src) && ci_file_exists(w->img_src))
            return W_NEXT;
        ci_umount(MNT_SRC);
        w->media_mounted = 0;
    }

    for (;;) {
        int media = 0;
        if (locate_sources(w->kernel_src, w->img_src, &media)) {
            w->media_mounted = media;
            printf("kernel: %s\nimage : %s\n", w->kernel_src, w->img_src);
            return W_NEXT;
        }
        static const char *items[] = {
            "search for the install media again",
            "go back to the mount step",
            "cancel the installation",
        };
        int c = ci_menu("kernel.bin / cctkfs.img were not found:",
                        items, 3);
        if (c == 1)
            continue;
        if (c == 3 || c < 0)
            return W_QUIT;
        return W_BACK;
    }
}

static int step_deploy(struct wiz *w) {
    if (deploy_step(&w->plan, w->kernel_src, w->img_src) != 0) {
        printf("deploy failed; nothing was bootstrapped into the target\n");
        return W_BACK;
    }
    return W_NEXT;
}

static int step_config(struct wiz *w) {
    if (config_step(&w->plan, w->root_dev) != 0) {
        int c = ci_confirm("Some config files could not be written. Continue?",
                           1);
        if (c < 0)
            return W_QUIT;
        if (!c)
            return W_BACK;
    }
    return W_NEXT;
}

static int step_grub(struct wiz *w) {
    printf("[grub] installing the boot loader\n");
    if (w->plan.scheme == SCHEME_GPT_EFI) {
        printf("EFI scheme: the loader was staged on the ESP during deploy\n");
        return W_NEXT;
    }
    if (ci_grub_install(&w->disk, &w->plan, PAYLOAD_GRUB) != 0) {
        printf("warning: GRUB could not be embedded — the disk may not boot\n");
        int c = ci_confirm("Continue anyway?", 1);
        if (c < 0)
            return W_QUIT;
        if (!c)
            return W_BACK;
    }
    return W_NEXT;
}

/* ── teardown ─────────────────────────────────────────────────────────── */

static void wiz_unmount(struct wiz *w) {
    if (w->esp_mounted) {
        ci_umount(MNT_ESP);
        w->esp_mounted = 0;
    }
    if (w->root_mounted) {
        ci_umount(MNT_ROOT);
        if (w->plan.root_idx >= 0 && w->plan.root_idx < w->plan.nparts) {
            char dev[40];
            snprintf(dev, sizeof(dev), "/dev/%s",
                     w->plan.parts[w->plan.root_idx].dev);
            ci_umount(dev);
        }
        w->root_mounted = 0;
    }
    if (w->media_mounted) {
        ci_umount(MNT_SRC);
        w->media_mounted = 0;
    }
}

static const char *scheme_name(int s) {
    switch (s) {
    case SCHEME_MBR_BIOS: return "MBR BIOS";
    case SCHEME_GPT_BIOS: return "GPT BIOS";
    case SCHEME_GPT_EFI:  return "GPT EFI";
    case SCHEME_MANUAL:   return "manual";
    default:              return "?";
    }
}

enum {
    ST_DISK = 0,
    ST_CONFIRM,
    ST_SCHEME,
    ST_LAYOUT,
    ST_MKFS,
    ST_MOUNT,
    ST_SOURCES,
    ST_DEPLOY,
    ST_CONFIG,
    ST_GRUB,
    ST_DONE
};

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    ci_term_init();
    banner();

    if (geteuid() != 0)
        printf("warning: not running as root; disk operations will fail\n");

    ensure_modules();

    static struct wiz w;
    memset(&w, 0, sizeof(w));
    w.plan.root_idx = -1;
    w.plan.esp_idx = -1;

    int st = ST_DISK;
    while (st != ST_DONE) {
        int r;
        switch (st) {
        case ST_DISK:    r = step_disk(&w);    break;
        case ST_CONFIRM: r = step_confirm(&w); break;
        case ST_SCHEME:  r = step_scheme(&w);  break;
        case ST_LAYOUT:  r = step_layout(&w);  break;
        case ST_MKFS:    r = step_mkfs(&w);    break;
        case ST_MOUNT:   r = step_mount(&w);   break;
        case ST_SOURCES: r = step_sources(&w); break;
        case ST_DEPLOY:  r = step_deploy(&w);  break;
        case ST_CONFIG:  r = step_config(&w);  break;
        case ST_GRUB:    r = step_grub(&w);    break;
        default:         r = W_NEXT;           break;
        }

        if (r == W_QUIT) {
            wiz_unmount(&w);
            printf("\nInstallation cancelled. The target disk was left as it is.\n");
            ci_term_done();
            return 1;
        }

        if (r == W_BACK) {
            if (st == ST_DISK) {
                wiz_unmount(&w);
                printf("\nInstallation cancelled.\n");
                ci_term_done();
                return 1;
            }
            st--;
            if (st < ST_MOUNT)
                wiz_unmount(&w);   /* leave the mount phase cleanly */
        } else {
            st++;
        }
    }

    wiz_unmount(&w);

    printf("\nInstallation complete.\n");
    printf("  disk   : %s\n", w.disk.base);
    printf("  root   : %s\n", w.root_dev);
    if (w.plan.esp_idx >= 0)
        printf("  esp    : /dev/%s\n", w.plan.parts[w.plan.esp_idx].dev);
    printf("  layout : scheme %d (%s)\n", w.plan.scheme,
           scheme_name(w.plan.scheme));

    int c = ci_confirm("Reboot now?", 0);
    if (c == 1) {
        if (reboot(RB_AUTOBOOT) != 0)
            printf("reboot failed\n");
    }

    ci_term_done();
    return 0;
}
