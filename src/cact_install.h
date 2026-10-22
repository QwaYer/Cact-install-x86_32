#ifndef CACT_INSTALL_H
#define CACT_INSTALL_H

#include <stdint.h>
#include <stddef.h>

#define CI_MAX_DISKS  8
#define CI_MAX_PARTS  16
#define CI_MAX_PLAN   6

#define SCHEME_MBR_BIOS  1
#define SCHEME_GPT_BIOS  2
#define SCHEME_GPT_EFI   3
#define SCHEME_MANUAL    4

#define ROLE_ROOT  1
#define ROLE_ESP   2

#define PT_EXT4  "ext4"
#define PT_FAT32 "fat32"

#define MNT_ROOT "/tmp/cactroot"
#define MNT_SRC  "/tmp/cactsrc"
#define MNT_ESP  MNT_ROOT "/boot/efi"

struct ci_disk {
    char      name[16];
    char      base[32];
    uint64_t  sectors;
};

struct ci_partdev {
    char      name[16];
    uint64_t  start;
    uint64_t  size;
    int       table;
};

struct ci_lpart {
    char      dev[20];
    char      fstype[12];
    char      mnt[40];
    int       number;
    int       role;
    int       to_format;
};

struct ci_plan {
    int       scheme;
    int       nparts;
    int       root_idx;
    int       esp_idx;
    struct ci_lpart parts[CI_MAX_PLAN];
};

struct ci_fdisk_cmd {
    int       n;
    char      tok[40][24];
};

/* ui.c */
struct tui;
void ci_term_init(void);
void ci_term_done(void);
/* The live TUI handle, for steps that draw their own screens. */
struct tui *ci_tui(void);
int  ci_menu(const char *title, const char **items, int n); /* >0 pick, 0 back, -1 input lost */
int  ci_confirm(const char *q, int def);                   /* 1 yes, 0 no, -1 input lost */
int  ci_prompt(const char *q, char *buf, int size);
long ci_prompt_num(const char *q, long def);
void ci_press(void);

/* run.c */
int ci_exec(const char *path, char *const argv[]);

/* archive.c */
/* Re-pack localrepo.tar.zst for the target, dropping every member under
 * skip_prefix (the installer's own media).  Returns 0 on success. */
int ci_archive_rebuild(const char *src, const char *dst, const char *skip_prefix);

/* Walk the tar members of `src` without unpacking: `cb` sees every header in
 * order and may stop the walk by returning non-zero. */
typedef int (*ci_arc_cb)(const char *name, char type,
                         unsigned long long size, void *ud);
int ci_archive_walk(const char *src, ci_arc_cb cb, void *ud);

/* Stream `src` into a fresh zstd archive at `dst`, keeping only the members
 * `keep` accepts.  This is how the target gets a small firmware archive
 * instead of a verbatim copy of the full 500 MB one. */
typedef int (*ci_arc_keep)(const char *name, char type, void *ud);
int ci_archive_repack(const char *src, const char *dst,
                      ci_arc_keep keep, void *ud);

/* firmware.c */
#define CI_FW_MAX_GROUPS 256
#define CI_FW_NAME_MAX   64

struct ci_fw_group {
    char     name[CI_FW_NAME_MAX];   /* path component under usr/lib/firmware/ */
    uint32_t files;
    uint64_t bytes;
    uint8_t  wanted;                 /* the kernel log named a member of it */
    uint8_t  selected;
};

/* List the firmware groups in `arc`.  Returns the group count, or -1. */
int ci_fw_scan(const char *arc, struct ci_fw_group *out, int max);

/* Mark groups whose members this machine asked for.  The live medium carries
 * no /usr/lib/firmware, so every driver request lands in the kernel log as
 * "firmware: <name> not found" — that log is the detection.  Returns the
 * number of groups marked. */
int ci_fw_detect(struct ci_fw_group *groups, int n);

/* Selection context for ci_fw_keep(): the groups, with their `selected` flags. */
struct ci_fw_sel {
    const struct ci_fw_group *groups;
    int                       n;
};

/* Keep-predicate for ci_archive_repack(): directory entries always, plus any
 * member that lands in a selected firmware group. */
int ci_fw_keep(const char *name, char type, void *ud);

/* Categories the firmware screen groups the list under.  The order here is the
 * order the sections appear in. */
enum {
    CI_FW_CAT_CPU = 0,   /* microcode: node-independent, usually wanted */
    CI_FW_CAT_GPU,
    CI_FW_CAT_NET,
    CI_FW_CAT_BT,
    CI_FW_CAT_AUDIO,
    CI_FW_CAT_STORAGE,
    CI_FW_CAT_OTHER,
    CI_FW_CAT_COUNT
};

int         ci_fw_category(const char *group);
const char *ci_fw_category_name(int cat);

/* One row of the firmware screen: either a category header or a group. */
struct ci_fw_row {
    char          label[80];
    unsigned char header;     /* 1: a section title, not selectable */
    int           group;      /* index into the caller's group array, -1 for a header */
};

#define CI_FW_MAX_ROWS (CI_FW_MAX_GROUPS + CI_FW_CAT_COUNT)

/* Order the groups by category (then name) and interleave a header per
 * non-empty category, filling each row's display label.  Returns row count. */
int ci_fw_arrange(const struct ci_fw_group *groups, int n,
                  struct ci_fw_row *out, int max);

/* fsops.c */
void ci_fmt_bytes(uint64_t bytes, char *out, int n);
int  ci_mkdir_p(const char *path);
int  ci_file_exists(const char *path);
int  ci_write_file(const char *path, const char *data, uint32_t len);

/* Returns 0 on success, -1 on failure (nothing is left behind either way). */
int  ci_copy_file(const char *src, const char *dst);
int  ci_mount(const char *dev, const char *mnt, const char *fstype);
int  ci_umount(const char *dev_or_mnt);

/* disk.c */
int      ci_list_disks(struct ci_disk *out, int max);
int      ci_disk_parts(const struct ci_disk *d, struct ci_partdev *out, int max);
int      ci_disk_rescan(const struct ci_disk *d);
uint64_t ci_probe_capacity(const char *node_path);

/* layout.c */
int ci_layout_guided(const struct ci_disk *d, int scheme, long esp_mib,
                     struct ci_plan *plan, struct ci_fdisk_cmd *cmd);
int ci_layout_manual(const struct ci_disk *d, struct ci_plan *plan,
                     const struct ci_partdev *parts, int nparts);
int ci_run_fdisk(const struct ci_disk *d, const struct ci_fdisk_cmd *cmd);
void ci_plan_print(const struct ci_plan *plan);
void ci_fill_part_devices(const struct ci_disk *d, struct ci_plan *plan);

/* grub.c */
int ci_grub_install(const struct ci_disk *d, const struct ci_plan *plan,
                    const char *payload_grub);
/* 0 on success, -1 if the loader files could not be staged.  These helpers do
 * not print: the deploy screen owns the display. */
int ci_deploy_grub_files(const struct ci_plan *plan, const char *root_mnt,
                         const char *payload_grub);
/* 0 on success, -1 when grub.cfg could not be written. */
int ci_grub_cfg_write(const char *root_mnt);

#endif
