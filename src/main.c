/*
 * main.c — the CactOS installer wizard.
 *
 * The screens are plain sequential steps (disk -> confirm -> scheme ->
 * partition -> format -> mount -> media -> firmware -> system -> summary ->
 * install), each drawn through the full-screen TUI, so the installer reads and
 * behaves like bsdinstall/archinstall rather than a numbered prompt list.
 *
 * Everything that touches the target lives behind an explicit confirmation,
 * and the firmware step is the one place the target's contents differ from the
 * medium: only the groups the user ticks are re-packed into the small archive
 * the installed system carries.
 */

#include "cact_install.h"
#include "tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stat.h>

#define PAYLOAD_BOOT    "/usr/lib/cact-install/boot"
#define PAYLOAD_GRUB    "/usr/lib/cact-install/grub"

/* The boot cctkfs module, served in place by the kernel.  It publishes its size
 * (0 = this medium carried no module), so it is the one image guaranteed to
 * match the system the installer is running on — which is why the media bundle
 * does not carry a copy of it: a copy would be a stale image, and packing it
 * into cctkfs.img would nest the image inside itself. */
#define IMAGE_DEV       "/dev/cctkfs"

#define TOOL_FDISK      "/usr/sbin/fdisk"
#define TOOL_MKFS_EXT4  "/usr/sbin/mkfs.ext4"
#define TOOL_MKFS_FAT32 "/usr/sbin/mkfs.fat32"
#define TOOL_ROOTFS     "/usr/sbin/cact-rootfs"
#define TOOL_FIRMEXTR   "/usr/bin/firmextr"

/* The installer media (boot/kernel.bin, grub/) is baked into cctkfs.img, so the
 * bundled path below is the normal case; g_boot_dir/g_grub_dir point at it, or
 * at a work dir firmextr filled from an older localrepo archive that still
 * carried the media.  The image to install comes from /dev/cctkfs. */
static char g_boot_dir[256] = PAYLOAD_BOOT;
static char g_grub_dir[256] = PAYLOAD_GRUB;

#define LOCALREPO_WORK  "/tmp/cact-install-payload"

/* Wizard position shown in the header. */
#define CI_STEPS 9

static const char *slabel(char *buf, int n, int i, const char *name) {
    snprintf(buf, n, "%s   [%d/%d]", name, i, CI_STEPS);
    return buf;
}

static const char *localrepo_candidates[] = {
    "/tmp/localrepo.tar.zst",   /* cgoct staged it into tmpfs at boot */
    "/dev/localrepo",           /* GRUB module, served by the kernel */
    "/localrepo.tar.zst",
    "/boot/localrepo.tar.zst",
    NULL
};

/* A candidate is only an archive if it carries bytes: an empty /dev/localrepo
 * (a boot medium whose GRUB did not hand the module over) or a zero-length file
 * would otherwise be picked and streamed as an empty archive. */
static int localrepo_has_data(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    if (S_ISREG(st.st_mode) || S_ISCHR(st.st_mode))
        return st.st_size > 0;
    return 1;
}

static const char *find_localrepo(void) {
    for (int i = 0; localrepo_candidates[i]; i++)
        if (localrepo_has_data(localrepo_candidates[i]))
            return localrepo_candidates[i];
    return NULL;
}

/* Stream the installer media out of the archive (only that subtree) into a
 * work dir, for media built before the payload moved into cctkfs.img. */
static int payload_from_archive(void) {
    const char *arc = find_localrepo();
    if (!arc)
        return 0;
    if (!ci_file_exists(TOOL_FIRMEXTR))
        return 0;

    char *argv[] = { (char *)"firmextr", (char *)"-f", (char *)arc,
                     (char *)"-C", (char *)LOCALREPO_WORK,
                     (char *)"usr/lib/cact-install/", NULL };
    if (ci_exec(TOOL_FIRMEXTR, argv) != 0)
        return 0;
    snprintf(g_boot_dir, sizeof(g_boot_dir),
             LOCALREPO_WORK "/usr/lib/cact-install/boot");
    snprintf(g_grub_dir, sizeof(g_grub_dir),
             LOCALREPO_WORK "/usr/lib/cact-install/grub");
    return 1;
}

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
    char hostname[64];
    int  media_mounted;
    int  root_mounted;
    int  esp_mounted;

    struct ci_fw_group fw[CI_FW_MAX_GROUPS];
    int                fw_n;
    int                fw_total_selected;
};

static int tool_ok(const char *path) {
    return ci_file_exists(path);
}

static void fmt_size(uint64_t sectors, char *out, int n) {
    ci_fmt_bytes(sectors * 512, out, n);
}

static void ensure_modules(void) {
    module_load("/usr/lib/modules/ext4.cctk", (unsigned)-1, (unsigned)-1);
    module_load("/usr/lib/modules/fat32.cctk", (unsigned)-1, (unsigned)-1);
}

static const char *scheme_name(int s) {
    switch (s) {
    case SCHEME_MBR_BIOS: return "MBR / BIOS";
    case SCHEME_GPT_BIOS: return "GPT / BIOS";
    case SCHEME_GPT_EFI:  return "GPT / EFI";
    case SCHEME_MANUAL:   return "manual";
    default:              return "?";
    }
}

/* ── payload location ─────────────────────────────────────────────────── */

static const char *find_boot_image(void) {
    static char bundled[256];
    struct stat st;
    if (stat(IMAGE_DEV, &st) == 0 && st.st_size > 0)
        return IMAGE_DEV;
    snprintf(bundled, sizeof(bundled), "%s/cctkfs.img", g_boot_dir);
    if (ci_file_exists(bundled))
        return bundled;
    return NULL;
}

static int check_payload(void) {
    char k[256];
    snprintf(k, sizeof(k), "%s/kernel.bin", g_boot_dir);
    return ci_file_exists(k) && find_boot_image() != NULL;
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

    const char *image;
    if (check_payload()) {
        image = find_boot_image();
        snprintf(kernel, 256, "%s/kernel.bin", g_boot_dir);
        snprintf(img, 256, "%s", image);
        return 1;
    }

    /* Only when the bundle is absent: older media kept the payload in
     * localrepo.tar.zst, which firmextr now streams in full (the subtree may
     * sit anywhere in the archive).  Doing this first would scan the whole
     * 500 MB archive on every install — and a stale staging copy under /tmp
     * would surface as a bogus "zstd: Data corruption detected" that has
     * nothing to do with the payload. */
    payload_from_archive();
    if (check_payload()) {
        image = find_boot_image();
        snprintf(kernel, 256, "%s/kernel.bin", g_boot_dir);
        snprintf(img, 256, "%s", image);
        return 1;
    }

    if (scan_source(k, i)) {
        snprintf(kernel, 256, "%s", k);
        snprintf(img, 256, "%s", i);
        *media_mounted = 1;
        return 1;
    }

    if (!tui_input(ci_tui(), "Install media",
                   "Media   [7/9]",
                   "kernel.bin was not found bundled or on any partition. Give a path:",
                   kernel, 256))
        return 0;
    if (!ci_file_exists(kernel))
        return 0;
    if (!tui_input(ci_tui(), "Install media", "Media   [7/9]",
                   "Path to cctkfs.img:", img, 256))
        return 0;
    if (!ci_file_exists(img))
        return 0;
    return 1;
}

/* ── formatting / mounting ────────────────────────────────────────────── */

static int format_part(const struct ci_lpart *p) {
    char dev[40];
    snprintf(dev, sizeof(dev), "/dev/%s", p->dev);
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

/* Put a firmware archive at the target's root: the boot flow copies it to /tmp
 * and firmextr streams firmware out of it on demand.
 *
 * Only the groups ticked in the firmware step are packed; when the user kept
 * every group the source is already the right archive and is copied verbatim
 * (exact bytes, no CPU). */
static void deploy_localrepo(const struct wiz *w) {
    const char *arc = find_localrepo();
    if (!arc) {
        tui_tasks_set_text(ci_tui(), 3, "no archive on this medium");
        return;
    }

    char dst[300];
    snprintf(dst, sizeof(dst), MNT_ROOT "/localrepo.tar.zst");

    int any = 0, all = (w->fw_n > 0);
    for (int i = 0; i < w->fw_n; i++) {
        if (w->fw[i].selected)
            any = 1;
        else
            all = 0;
    }
    if (!any) {
        tui_tasks_set_text(ci_tui(), 3, "no firmware groups selected");
        return;
    }

    tui_tasks_set_text(ci_tui(), 3, all ? "copying (all groups kept)"
                                        : "packing the selection");
    int rc;
    if (all) {
        rc = ci_copy_file(arc, dst);
    } else {
        struct ci_fw_sel sel;
        sel.groups = w->fw;
        sel.n = w->fw_n;
        rc = ci_archive_repack(arc, dst, ci_fw_keep, &sel);
    }

    if (rc != 0) {
        unlink(dst);
        tui_tasks_set_text(ci_tui(), 3, "failed");
        tui_tasks_set(ci_tui(), 3, TUI_TASK_FAIL);
        return;
    }

    struct stat st;
    if (stat(dst, &st) == 0) {
        char sz[24];
        ci_fmt_bytes((uint64_t)st.st_size, sz, sizeof(sz));
        tui_tasks_set_text(ci_tui(), 3, sz);
    }
}

/* ── configuration files ──────────────────────────────────────────────── */

static const char *cgf =
    "# CactOS supervisor config (generated by cact-install)\n"
    "restart_policy=on-failure\n"
    "rescue_shell=1\n"
    "crash_limit=3\n"
    "cooldown_sec=2\n"
    "services=\n";

static int write_one(const char *path, const char *data, uint32_t len) {
    return ci_write_file(path, data, len);
}

static int write_configs(const struct wiz *w) {
    char path[256];
    char buf[160];
    int bad = 0;

    if (ci_mkdir_p(MNT_ROOT "/etc") != 0)
        return -1;

    snprintf(path, sizeof(path), MNT_ROOT "/etc/hostname");
    bad |= write_one(path, w->hostname, (uint32_t)strlen(w->hostname));

    snprintf(path, sizeof(path), MNT_ROOT "/etc/cgoct.conf");
    bad |= write_one(path, cgf, (uint32_t)strlen(cgf));

    snprintf(path, sizeof(path), MNT_ROOT "/etc/mounts");
    {
        char mounts[160];
        int n = 0, cap = (int)sizeof(mounts);
        for (int i = 0; i < w->plan.nparts && n < cap - 1; i++) {
            const struct ci_lpart *p = &w->plan.parts[i];
            if (!p->role)
                continue;
            const char *tgt = (p->role == ROLE_ESP) ? "/boot/efi" : "/";
            int c = snprintf(mounts + n, (size_t)(cap - n), "/dev/%s %s %s\n",
                             p->dev, tgt, p->fstype);
            if (c <= 0 || c >= cap - n)
                break;
            n += c;
        }
        bad |= write_one(path, mounts, (uint32_t)n);
    }

    snprintf(path, sizeof(path), MNT_ROOT "/etc/cact-install.conf");
    snprintf(buf, sizeof(buf), "target=%s\nroot=%s\nscheme=%d\nhostname=%s\n",
             w->disk.base, w->plan.parts[w->plan.root_idx].dev, w->plan.scheme,
             w->hostname);
    bad |= write_one(path, buf, (uint32_t)strlen(buf));

    return bad ? -1 : 0;
}

/* ── steps ────────────────────────────────────────────────────────────── */

static int step_disk(struct wiz *w) {
    struct ci_disk disks[CI_MAX_DISKS];
    static char labels[CI_MAX_DISKS][64];
    static const char *items[CI_MAX_DISKS];
    char label[64];

    int nd = ci_list_disks(disks, CI_MAX_DISKS);
    if (nd <= 0) {
        static const char *l[] = {
            "No block disks were found under /dev.",
            "Attach a disk (or load the storage driver) and start again.",
        };
        tui_notice(ci_tui(), "Disk", slabel(label, sizeof(label), 1, "Disk"),
                   NULL, "Enter quit", l, 2);
        return W_QUIT;
    }

    for (int i = 0; i < nd; i++) {
        char sz[24];
        fmt_size(disks[i].sectors, sz, sizeof(sz));
        snprintf(labels[i], 64, "%-12s %s", disks[i].base, sz);
        items[i] = labels[i];
    }

    int sel = 0;
    if (tui_menu(ci_tui(), "Select the disk to install CactOS onto",
                 slabel(label, sizeof(label), 1, "Disk"),
                 "Everything on the chosen disk will be replaced.",
                 "Up/Down move   Enter select   Left/q quit",
                 items, nd, &sel) != 0)
        return W_QUIT;

    w->disk = disks[sel];
    w->root_mounted = 0;
    w->esp_mounted = 0;
    memset(&w->plan, 0, sizeof(w->plan));
    w->plan.root_idx = -1;
    w->plan.esp_idx = -1;
    w->root_dev[0] = '\0';
    return W_NEXT;
}

static int step_confirm(struct wiz *w) {
    char label[64];
    char note[128];
    snprintf(note, sizeof(note),
             "Target disk: %s — its current partition table will be destroyed.",
             w->disk.base);

    int c = tui_confirm(ci_tui(), "Confirm", slabel(label, sizeof(label), 2, "Confirm"),
                        note, 0);
    if (c < 0)
        return W_QUIT;
    if (!c)
        return W_BACK;
    return W_NEXT;
}

static int step_scheme(struct wiz *w) {
    static const char *items[] = {
        "MBR  / BIOS   classic legacy BIOS, single ext4 root",
        "GPT  / BIOS   legacy BIOS with a GPT, single ext4 root",
        "GPT  / EFI    UEFI, FAT32 ESP + ext4 root",
        "Manual        partition with fdisk, then assign roles",
    };
    char label[64];
    int sel = 0;
    if (tui_menu(ci_tui(), "Choose the partition scheme",
                 slabel(label, sizeof(label), 3, "Scheme"),
                 "Alignment is handled for you; Manual runs fdisk.",
                 "Up/Down move   Enter select   Left back",
                 items, 4, &sel) != 0)
        return W_BACK;
    w->scheme = (sel == 3) ? SCHEME_MANUAL : sel + 1;
    return W_NEXT;
}

static int verify_parts(struct wiz *w) {
    struct ci_partdev parts[CI_MAX_PARTS];
    int np = ci_disk_parts(&w->disk, parts, CI_MAX_PARTS);
    if (np <= 0) {
        ci_disk_rescan(&w->disk);
        np = ci_disk_parts(&w->disk, parts, CI_MAX_PARTS);
    }
    return np > 0;
}

static int step_manual(struct wiz *w) {
    char label[64];
    static const char *l[] = {
        "fdisk will now run on the target disk.",
        "Create a table (o = MBR, g = GPT) and use 1 MiB alignment,",
        "then come back and assign each partition a role.",
    };
    tui_notice(ci_tui(), "Partition", slabel(label, sizeof(label), 4, "Partition"),
               w->disk.base, "Enter run fdisk", l, 3);

    if (!tool_ok(TOOL_FDISK))
        return W_BACK;
    if (ci_run_fdisk(&w->disk, NULL) != 0)
        return W_BACK;

    struct ci_partdev parts[CI_MAX_PARTS];
    int np = ci_disk_parts(&w->disk, parts, CI_MAX_PARTS);
    if (np <= 0) {
        ci_disk_rescan(&w->disk);
        np = ci_disk_parts(&w->disk, parts, CI_MAX_PARTS);
    }
    if (np <= 0)
        return W_BACK;

    if (ci_layout_manual(&w->disk, &w->plan, parts, np) != 0)
        return W_BACK;
    return W_NEXT;
}

static int step_layout(struct wiz *w) {
    char label[64];
    if (w->scheme == SCHEME_MANUAL)
        return step_manual(w);

    if (!tool_ok(TOOL_FDISK)) {
        static const char *l[] = { "fdisk is missing from this medium." };
        tui_notice(ci_tui(), "Partition",
                   slabel(label, sizeof(label), 4, "Partition"),
                   NULL, "Enter back", l, 1);
        return W_BACK;
    }

    long esp = 512;
    if (w->scheme == SCHEME_GPT_EFI) {
        char buf[32] = "512";
        if (tui_input(ci_tui(), "Partition",
                      slabel(label, sizeof(label), 4, "Partition"),
                      "EFI system partition size in MiB:", buf, sizeof(buf)))
            esp = strtol(buf, NULL, 10);
        if (esp < 8)
            esp = 512;
    }

    struct ci_fdisk_cmd cmd;
    if (ci_layout_guided(&w->disk, w->scheme, esp, &w->plan, &cmd) != 0) {
        static const char *l[] = {
            "That layout does not fit on this disk.",
            "Pick another scheme.",
        };
        tui_notice(ci_tui(), "Partition",
                   slabel(label, sizeof(label), 4, "Partition"),
                   NULL, "Enter back", l, 2);
        return W_BACK;
    }
    ci_fill_part_devices(&w->disk, &w->plan);

    char note[256];
    {
        int n = 0;
        for (int i = 0; i < w->plan.nparts; i++) {
            const struct ci_lpart *p = &w->plan.parts[i];
            n += snprintf(note + n, sizeof(note) - (size_t)n, "%s %s%s",
                          p->dev,
                          p->role == ROLE_ESP ? "esp/fat32" : "root/ext4",
                          i + 1 < w->plan.nparts ? "   " : "");
            if (n >= (int)sizeof(note) - 24)
                break;
        }
    }

    int c = tui_confirm(ci_tui(), "Partition",
                        slabel(label, sizeof(label), 4, "Partition"),
                        note, 1);
    if (c < 0)
        return W_QUIT;
    if (!c)
        return W_BACK;

    if (ci_run_fdisk(&w->disk, &cmd) != 0) {
        static const char *l[] = { "fdisk could not write the table; the disk is unchanged." };
        tui_notice(ci_tui(), "Partition",
                   slabel(label, sizeof(label), 4, "Partition"),
                   NULL, "Enter back", l, 1);
        return W_BACK;
    }
    if (!verify_parts(w)) {
        static const char *l[] = { "The kernel still does not expose any partitions." };
        tui_notice(ci_tui(), "Partition",
                   slabel(label, sizeof(label), 4, "Partition"),
                   NULL, "Enter back", l, 1);
        return W_BACK;
    }
    return W_NEXT;
}

static int step_mkfs(struct wiz *w) {
    char label[64];
    char note[200];
    int n = 0, any = 0;

    for (int i = 0; i < w->plan.nparts; i++) {
        const struct ci_lpart *p = &w->plan.parts[i];
        if (!p->to_format)
            continue;
        n += snprintf(note + n, sizeof(note) - (size_t)n, "%s %s%s",
                      p->dev, p->fstype, "   ");
        any = 1;
        if (n >= (int)sizeof(note) - 24)
            break;
    }
    if (!any) {
        static const char *l[] = { "Nothing to format; going back." };
        tui_notice(ci_tui(), "Format", slabel(label, sizeof(label), 5, "Format"),
                   NULL, "Enter back", l, 1);
        return W_BACK;
    }

    int c = tui_confirm(ci_tui(), "Format", slabel(label, sizeof(label), 5, "Format"),
                        note, 1);
    if (c < 0)
        return W_QUIT;
    if (!c)
        return W_BACK;

    int bad = 0;
    for (int i = 0; i < w->plan.nparts; i++) {
        const struct ci_lpart *p = &w->plan.parts[i];
        if (!p->to_format)
            continue;
        if (format_part(p) != 0)
            bad = 1;
    }
    if (bad) {
        static const char *l[] = { "Formatting failed on at least one partition." };
        tui_notice(ci_tui(), "Format", slabel(label, sizeof(label), 5, "Format"),
                   NULL, "Enter back", l, 1);
        return W_BACK;
    }
    return W_NEXT;
}

static int step_mount(struct wiz *w) {
    char label[64];
    const struct ci_lpart *root = &w->plan.parts[w->plan.root_idx];
    snprintf(w->root_dev, sizeof(w->root_dev), "/dev/%s", root->dev);

    if (ci_mkdir_p(MNT_ROOT) != 0 || ci_mkdir_p(MNT_SRC) != 0)
        return W_BACK;

    if (!w->root_mounted) {
        if (ci_mount(w->root_dev, MNT_ROOT, root->fstype) != 0)
            return W_BACK;
        w->root_mounted = 1;
    }

    if (w->plan.esp_idx >= 0 && !w->esp_mounted) {
        const struct ci_lpart *esp = &w->plan.parts[w->plan.esp_idx];
        char dev[40];
        snprintf(dev, sizeof(dev), "/dev/%s", esp->dev);
        if (ci_mkdir_p(esp->mnt) != 0)
            return W_BACK;
        if (ci_mount(dev, esp->mnt, esp->fstype) != 0)
            return W_BACK;
        w->esp_mounted = 1;
    }
    (void)label;
    return W_NEXT;
}

static int step_sources(struct wiz *w) {
    char label[64];
    for (;;) {
        int media = 0;
        if (locate_sources(w->kernel_src, w->img_src, &media)) {
            w->media_mounted = media;
            return W_NEXT;
        }
        static const char *items[] = {
            "Search for the install media again",
            "Go back to the mount step",
            "Cancel the installation",
        };
        int sel = 0;
        if (tui_menu(ci_tui(), "Install media not found",
                     slabel(label, sizeof(label), 7, "Media"),
                     "kernel.bin / cctkfs.img were not found.",
                     "Up/Down move   Enter select   Left back",
                     items, 3, &sel) != 0)
            return W_QUIT;
        if (sel == 0)
            continue;
        if (sel == 2)
            return W_QUIT;
        return W_BACK;
    }
}

/* ── firmware selection ───────────────────────────────────────────────── */

static int step_firmware(struct wiz *w) {
    char label[64];
    const char *arc = find_localrepo();

    if (!arc) {
        static const char *l[] = {
            "No firmware archive was found on this medium, so the target",
            "gets none. Firmware can be added later with cactpkg/firmextr.",
        };
        tui_notice(ci_tui(), "Firmware",
                   slabel(label, sizeof(label), 8, "Firmware"),
                   NULL, "Enter continue", l, 2);
        w->fw_n = 0;
        return W_NEXT;
    }

    /* Listing the groups means decompressing the whole archive, so say so
     * before the screen freezes on the scan. */
    {
        static const char *one[] = { "Scanning the firmware archive" };
        tui_tasks_begin(ci_tui(), "Firmware",
                        slabel(label, sizeof(label), 8, "Firmware"),
                        w->fw_n ? "rescanning" : "this can take a moment",
                        one, 1);
        tui_tasks_set(ci_tui(), 0, TUI_TASK_RUN);
        w->fw_n = ci_fw_scan(arc, w->fw, CI_FW_MAX_GROUPS);
        if (w->fw_n > 0)
            tui_tasks_set_text(ci_tui(), 0, "done");
        tui_tasks_set(ci_tui(), 0, w->fw_n > 0 ? TUI_TASK_OK : TUI_TASK_FAIL);
        tui_tasks_end(ci_tui());
    }

    if (w->fw_n <= 0) {
        static const char *l[] = {
            "The archive could not be read (or carries no firmware).",
            "Continuing without a firmware selection.",
        };
        tui_notice(ci_tui(), "Firmware",
                   slabel(label, sizeof(label), 8, "Firmware"),
                   NULL, "Enter continue", l, 2);
        w->fw_n = 0;
        return W_NEXT;
    }

    /* Preselect what this machine asked for: the live medium has no
     * /usr/lib/firmware, so drivers recorded every request in the kernel log,
     * and the modules that did attach name the hardware in /proc/modules. */
    int marked = ci_fw_detect(w->fw, w->fw_n);
    int fallback_all = 0;
    if (marked == 0) {
        /* Never let an installation end up with no firmware at all just because
         * nothing could be detected: take everything and say so, so the user
         * clears what they know they do not need. */
        for (int i = 0; i < w->fw_n; i++)
            w->fw[i].wanted = 1;
        marked = w->fw_n;
        fallback_all = 1;
    }

    static struct ci_fw_row rows[CI_FW_MAX_ROWS];
    static const char *items[CI_FW_MAX_ROWS];
    static unsigned char sep[CI_FW_MAX_ROWS];
    static unsigned char on[CI_FW_MAX_ROWS];

    int nr = ci_fw_arrange(w->fw, w->fw_n, rows, CI_FW_MAX_ROWS);
    if (nr <= 0) {
        static const char *l[] = { "The firmware list came out empty; skipping." };
        tui_notice(ci_tui(), "Firmware",
                   slabel(label, sizeof(label), 8, "Firmware"),
                   NULL, "Enter continue", l, 1);
        return W_NEXT;
    }

    /* Rows are sections and groups interleaved; the checkbox state lives on the
     * group rows and is written back to the groups on the way out. */
    for (int i = 0; i < nr; i++) {
        items[i] = rows[i].label;
        sep[i]   = rows[i].header;
        on[i]    = rows[i].header
                       ? 0
                       : (unsigned char)(w->fw[rows[i].group].wanted ? 1 : 0);
    }

    char note[200];
    if (fallback_all)
        snprintf(note, sizeof(note),
                 "Nothing could be auto-detected, so every group is selected - "
                 "clear the ones this machine does not need.");
    else
        snprintf(note, sizeof(note),
                 "%d group(s) preselected from this machine (kernel log + loaded "
                 "modules).  Space toggles.", marked);

    if (tui_checklist(ci_tui(), "Choose the firmware to install",
                      slabel(label, sizeof(label), 8, "Firmware"),
                      note,
                      "Up/Down move  Space toggle  a all  n none  r reset  Enter ok",
                      items, sep, on, nr) != 0)
        return W_BACK;

    w->fw_total_selected = 0;
    for (int i = 0; i < nr; i++) {
        if (rows[i].header)
            continue;
        w->fw[rows[i].group].selected = on[i];
        if (on[i])
            w->fw_total_selected++;
    }
    return W_NEXT;
}

/* ── system ───────────────────────────────────────────────────────────── */

static int step_system(struct wiz *w) {
    char label[64];
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", w->hostname[0] ? w->hostname : "cactos");
    if (tui_input(ci_tui(), "System", slabel(label, sizeof(label), 9, "System"),
                  "Hostname:", buf, sizeof(buf)) != 0)
        return W_BACK;
    if (!buf[0])
        snprintf(buf, sizeof(buf), "cactos");
    snprintf(w->hostname, sizeof(w->hostname), "%s", buf);
    return W_NEXT;
}

/* ── summary / install ────────────────────────────────────────────────── */

static int step_summary(struct wiz *w) {
    static char lines[14][120];
    static const char *ptrs[14];
    int n = 0;

    char sz[24];
    fmt_size(w->disk.sectors, sz, sizeof(sz));
    snprintf(lines[n++], 120, "Disk        %s  (%s)", w->disk.base, sz);
    snprintf(lines[n++], 120, "Scheme      %s", scheme_name(w->plan.scheme));
    for (int i = 0; i < w->plan.nparts && n < 10; i++) {
        const struct ci_lpart *p = &w->plan.parts[i];
        snprintf(lines[n++], 120, "Partition   /dev/%s  %s  %s",
                 p->dev, p->fstype,
                 p->role == ROLE_ESP ? "(ESP, /boot/efi)" : "(root, /)");
    }
    if (w->fw_n > 0)
        snprintf(lines[n++], 120, "Firmware    %d of %d groups selected",
                 w->fw_total_selected, w->fw_n);
    else
        snprintf(lines[n++], 120, "Firmware    none (no archive on the medium)");
    snprintf(lines[n++], 120, "Hostname    %s", w->hostname);
    snprintf(lines[n++], 120, "Media       %s", w->img_src);

    for (int i = 0; i < n; i++)
        ptrs[i] = lines[i];

    tui_notice(ci_tui(), "Summary", "Install   [final]",
               "Review, then confirm to write to the disk.",
               "Enter continue   Left back", ptrs, n);

    int c = tui_confirm(ci_tui(), "Install", "Install   [final]",
                        "Write CactOS to the disk now?", 1);
    if (c <= 0)
        return W_BACK;
    return W_NEXT;
}

static int step_deploy(struct wiz *w) {
    static const char *names[] = {
        "Root filesystem (kernel + cctkfs.img)",
        "Bootloader files",
        "Bootloader in the MBR",
        "Firmware archive",
    };
    int ef = (w->plan.scheme == SCHEME_GPT_EFI);
    int failed = 0;

    tui_tasks_begin(ci_tui(), "Installing", "Install   [final]",
                    "writing to the target disk", names, 4);

    /* 1. the root skeleton, kernel and image */
    tui_tasks_set(ci_tui(), 0, TUI_TASK_RUN);
    if (!tool_ok(TOOL_ROOTFS)) {
        tui_tasks_set_text(ci_tui(), 0, "cact-rootfs is missing");
        tui_tasks_set(ci_tui(), 0, TUI_TASK_FAIL);
        failed = 1;
    } else {
        char *argv[] = { TOOL_ROOTFS, "-k", w->kernel_src,
                         "-m", w->img_src, (char *)MNT_ROOT, NULL };
        if (ci_exec(TOOL_ROOTFS, argv) != 0) {
            tui_tasks_set_text(ci_tui(), 0, "cact-rootfs failed");
            tui_tasks_set(ci_tui(), 0, TUI_TASK_FAIL);
            failed = 1;
        } else {
            tui_tasks_set(ci_tui(), 0, TUI_TASK_OK);
        }
    }

    /* 2. grub.cfg and the bootloader files */
    if (!failed) {
        tui_tasks_set(ci_tui(), 1, TUI_TASK_RUN);
        if (ci_grub_cfg_write(MNT_ROOT) != 0 ||
            ci_deploy_grub_files(&w->plan, MNT_ROOT, g_grub_dir) != 0) {
            tui_tasks_set_text(ci_tui(), 1, "not staged");
            tui_tasks_set(ci_tui(), 1, TUI_TASK_FAIL);
        } else {
            tui_tasks_set_text(ci_tui(), 1,
                               ef ? "ESP loader + grub.cfg"
                                  : "grub.cfg + modules");
            tui_tasks_set(ci_tui(), 1, TUI_TASK_OK);
        }
    }

    /* 3. the BIOS core (nothing to embed for EFI) */
    if (!failed) {
        tui_tasks_set(ci_tui(), 2, TUI_TASK_RUN);
        if (ef) {
            tui_tasks_set_text(ci_tui(), 2, "not needed (EFI)");
            tui_tasks_set(ci_tui(), 2, TUI_TASK_OK);
        } else if (ci_grub_install(&w->disk, &w->plan, g_grub_dir) != 0) {
            tui_tasks_set_text(ci_tui(), 2, "could not embed core.img");
            tui_tasks_set(ci_tui(), 2, TUI_TASK_FAIL);
        } else {
            tui_tasks_set(ci_tui(), 2, TUI_TASK_OK);
        }
    }

    /* 4. the firmware selection, packed into the target's archive */
    if (!failed) {
        tui_tasks_set(ci_tui(), 3, TUI_TASK_RUN);
        deploy_localrepo(w);
        if (tui_tasks_state(ci_tui(), 3) != TUI_TASK_FAIL)
            tui_tasks_set(ci_tui(), 3, TUI_TASK_OK);
        else
            failed = 1;
    }

    /* configuration files last: they need the root mounted and deployed */
    if (!failed) {
        if (write_configs(w) != 0) {
            tui_tasks_note(ci_tui(), 0, "warning: some /etc files were not written");
        }
    }

    tui_tasks_end(ci_tui());

    if (failed) {
        static const char *l[] = {
            "The installation did not complete.",
            "The target disk was left partially written.",
        };
        tui_notice(ci_tui(), "Install", "Install   [final]",
                   NULL, "Enter back", l, 2);
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

enum {
    ST_DISK = 0,
    ST_CONFIRM,
    ST_SCHEME,
    ST_LAYOUT,
    ST_MKFS,
    ST_MOUNT,
    ST_SOURCES,
    ST_FIRMWARE,
    ST_SYSTEM,
    ST_SUMMARY,
    ST_DEPLOY,
    ST_DONE
};

static void welcome(void) {
    static const char *l[] = {
        "This wizard installs CactOS onto a disk.",
        "",
        "You will choose a disk and a partition scheme, pick the firmware",
        "that should travel to the target, and confirm a summary. Nothing is",
        "written until that confirmation.",
        "",
        "Up/Down and PgUp/PgDn move, Enter selects, Left goes back.",
    };
    tui_notice(ci_tui(), "CactOS installer", NULL,
               "A guided install, in the spirit of bsdinstall/archinstall.",
               "Enter begin", l, 7);
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    ci_term_init();
    if (!ci_tui()) {
        printf("cact-install: no terminal available\n");
        return 1;
    }

    ensure_modules();

    static struct wiz w;
    memset(&w, 0, sizeof(w));
    w.plan.root_idx = -1;
    w.plan.esp_idx = -1;
    snprintf(w.hostname, sizeof(w.hostname), "cactos");

    welcome();

    int st = ST_DISK;
    while (st != ST_DONE) {
        int r;
        switch (st) {
        case ST_DISK:     r = step_disk(&w);    break;
        case ST_CONFIRM:  r = step_confirm(&w); break;
        case ST_SCHEME:   r = step_scheme(&w);  break;
        case ST_LAYOUT:   r = step_layout(&w);  break;
        case ST_MKFS:     r = step_mkfs(&w);    break;
        case ST_MOUNT:    r = step_mount(&w);   break;
        case ST_SOURCES:  r = step_sources(&w); break;
        case ST_FIRMWARE: r = step_firmware(&w);break;
        case ST_SYSTEM:   r = step_system(&w);  break;
        case ST_SUMMARY:  r = step_summary(&w); break;
        case ST_DEPLOY:   r = step_deploy(&w);  break;
        default:          r = W_NEXT;           break;
        }

        if (r == W_QUIT) {
            wiz_unmount(&w);
            ci_term_done();
            printf("\nInstallation cancelled. The target disk was left as it "
                   "was.\n");
            return 1;
        }
        if (r == W_BACK) {
            if (st == ST_DISK) {
                wiz_unmount(&w);
                ci_term_done();
                printf("\nInstallation cancelled.\n");
                return 1;
            }
            st--;
            if (st < ST_MOUNT)
                wiz_unmount(&w);
        } else {
            st++;
        }
    }

    wiz_unmount(&w);

    /* The completion screen and the reboot question belong to the TUI: after
     * ci_term_done() there is no terminal to prompt on. */
    {
        static char lines[6][120];
        static const char *ptrs[6];
        snprintf(lines[0], 120, "Disk        %s", w.disk.base);
        snprintf(lines[1], 120, "Root        %s", w.root_dev);
        if (w.plan.esp_idx >= 0)
            snprintf(lines[2], 120, "ESP         /dev/%s",
                     w.plan.parts[w.plan.esp_idx].dev);
        else
            snprintf(lines[2], 120, "ESP         none (BIOS boot)");
        snprintf(lines[3], 120, "Firmware    %d of %d group(s)",
                 w.fw_total_selected, w.fw_n);
        snprintf(lines[4], 120, "Hostname    %s", w.hostname);
        for (int i = 0; i < 5; i++)
            ptrs[i] = lines[i];
        tui_notice(ci_tui(), "Installation complete", NULL,
                   "Remove the install medium before rebooting.",
                   "Enter continue", ptrs, 5);
    }

    int c = tui_confirm(ci_tui(), "Reboot", NULL,
                        "Reboot into the new system now?", 0);
    ci_term_done();

    if (c == 1) {
        printf("Rebooting...\n");
        if (reboot(RB_AUTOBOOT) != 0)
            printf("reboot failed\n");
    } else {
        printf("\nInstallation complete. Reboot when ready.\n");
    }
    return 0;
}
