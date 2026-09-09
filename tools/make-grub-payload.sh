#!/usr/bin/env bash
# Build the GRUB stage payload for cact-install.
#
# Produces, under OUT_DIR:
#   i386-pc/boot.img                 GRUB MBR stage1
#   i386-pc/core.img                 BIOS core (searches /boot/cctkfs.img)
#   i386-pc/*.mod                    trimmed BIOS module set for /boot/grub/i386-pc
#   i386-efi/bootia32.efi            i386 EFI core (all modules embedded)
#
# The BIOS core is self-locating: an embedded config finds the partition that
# carries /boot/cctkfs.img, sets root and prefix to ($root)/boot/grub, then
# normal loads /boot/grub/grub.cfg from there (modules from /boot/grub/i386-pc).
# The EFI core is compiled with prefix /EFI/BOOT and reads /EFI/BOOT/grub.cfg
# from the ESP that firmware booted; all needed modules are embedded.

set -eu

OUT_DIR="${1:-build/grub}"
GRUB_SRC="${GRUB_SRC:-/usr/lib/grub}"
BIOS_DIR="${GRUB_SRC}/i386-pc"
EFI_DIR="${GRUB_SRC}/i386-efi"
MKIMAGE="${MKIMAGE:-grub-mkimage}"

# modules linked into the BIOS core (available before grub.cfg is read)
BIOS_CORE_MODS="biosdisk part_msdos part_gpt ext2 fat search search_fs_file normal configfile boot multiboot2"
# module files staged to /boot/grub/i386-pc (loaded on demand by grub.cfg/insmod)
BIOS_DISK_MODS="boot multiboot2 part_msdos part_gpt ext2 fat search search_fs_file normal configfile ls cat test true echo reboot sleep read help minicmd terminal terminfo"
# modules embedded into the i386 EFI core (no on-disk module directory needed)
EFI_CORE_MODS="part_gpt part_msdos ext2 fat search search_fs_file normal configfile boot multiboot2 ls cat test true echo reboot sleep read help minicmd"

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}/i386-pc" "${OUT_DIR}/i386-efi"

if [ ! -d "${BIOS_DIR}" ]; then
    echo "grub i386-pc modules not found at ${BIOS_DIR}" >&2
    exit 1
fi
if ! command -v "${MKIMAGE}" >/dev/null 2>&1; then
    echo "grub-mkimage not found" >&2
    exit 1
fi

cat > "${OUT_DIR}/embed-bios.cfg" <<'EOF'
search --no-floppy --set=root --file /boot/cctkfs.img
set prefix=($root)/boot/grub
EOF

# --- BIOS (i386-pc) -----------------------------------------------------
cp -f "${BIOS_DIR}/boot.img" "${OUT_DIR}/i386-pc/boot.img"
"${MKIMAGE}" -O i386-pc -c "${OUT_DIR}/embed-bios.cfg" \
    -p /boot/grub -o "${OUT_DIR}/i386-pc/core.img" ${BIOS_CORE_MODS}

for m in ${BIOS_DISK_MODS}; do
    if [ -f "${BIOS_DIR}/${m}.mod" ]; then
        cp -f "${BIOS_DIR}/${m}.mod" "${OUT_DIR}/i386-pc/${m}.mod"
    else
        echo "warning: ${m}.mod missing from ${BIOS_DIR}" >&2
    fi
done

# --- EFI (i386-efi, best effort) ----------------------------------------
if [ -d "${EFI_DIR}" ] && [ -f "${EFI_DIR}/normal.mod" ]; then
    "${MKIMAGE}" -O i386-efi -p /EFI/BOOT \
        -o "${OUT_DIR}/i386-efi/bootia32.efi" ${EFI_CORE_MODS}
else
    echo "warning: no i386-efi grub dir (${EFI_DIR}); EFI scheme payload omitted" >&2
fi

echo "grub payload -> ${OUT_DIR}"
ls -la "${OUT_DIR}/i386-pc" "${OUT_DIR}/i386-efi" 2>/dev/null || true
