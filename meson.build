# ==============================================================================
# cact-install — Meson build system
# ==============================================================================
#
#   meson setup build-meson --cross-file cross/i686-cact-clang.ini \
#       -Dcactlib=../CactLibc-x86_32
#   ninja -C build-meson             # build-meson/cact-install
#   ninja -C build-meson stage        # copy into -Dlr_sbin (make install)
#   ninja -C build-meson grub-bundle  # GRUB stage files into build-meson/grub
#   ninja -C build-meson stage-media  # kernel + cctkfs + grub into -Dlr_lib
#
# A PIE linked against CactLibc with /lib/ld.so as PT_INTERP, exactly as the
# Makefile built it.  stage-media must run after the LocalRepo image exists.
# ==============================================================================

project('cact-install', 'c',
  meson_version: '>=1.0.0',
  default_options: [
    'c_std=gnu17',
    'warning_level=2',        # -Wall -Wextra, as in the Makefile
    'buildtype=plain',
    'b_staticpic=false',
  ])

fs = import('fs')
bash = find_program('bash')
ld = find_program('ld')

src_root = meson.project_source_root()
build_root = meson.current_build_dir()

# Path options are relative to this repo's root unless absolute.
_cactlib = get_option('cactlib')
cactlib = _cactlib.startswith('/') ? _cactlib : src_root / _cactlib
_lr_sbin = get_option('lr_sbin')
lr_sbin = _lr_sbin.startswith('/') ? _lr_sbin : src_root / _lr_sbin
_lr_lib = get_option('lr_lib')
lr_lib = _lr_lib.startswith('/') ? _lr_lib : src_root / _lr_lib
_lr_repo = get_option('lr_repo')
lr_repo = _lr_repo.startswith('/') ? _lr_repo : src_root / _lr_repo
_kern_build = get_option('kern_build')
kern_build = _kern_build.startswith('/') ? _kern_build : src_root / _kern_build

start_o = cactlib / 'build-meson' / 'start.o'
clibc_so = cactlib / 'build-meson' / 'clibc.so'

if not fs.exists(start_o) or not fs.exists(clibc_so)
  error('CactLibc is not built: expected ' + start_o +
        '  —  build it first:  ninja -C ' + cactlib / 'build-meson')
endif

# Every object in this repo is position independent: the wizard is a PIE.
add_project_arguments('-fPIE', language: 'c')

c_args = [
  '-ffreestanding',
  '-fno-stack-protector',
  '-nostdlib',
  '-ffunction-sections',
  '-fdata-sections',
  '-I' + (cactlib / 'include'),
]

installer_lib = static_library('cact-install',
  files(
    'src/disk.c',
    'src/fsops.c',
    'src/grub.c',
    'src/layout.c',
    'src/main.c',
    'src/run.c',
    'src/ui.c',
  ),
  c_args: c_args)

installer = custom_target('cact-install',
  output: 'cact-install',
  input: installer_lib,
  command: [ld, '-m', 'elf_i386', '-pie', '--dynamic-linker=/lib/ld.so',
            '--hash-style=both', '-nostdlib', '--gc-sections',
            '-T', src_root / 'link.ld', '-o', '@OUTPUT@',
            start_o,
            '--whole-archive', installer_lib, '--no-whole-archive',
            clibc_so],
  depend_files: files('link.ld'),
  build_by_default: true)

# The Makefile's `install` (Meson reserves that name, hence "stage").
run_target('stage',
  command: [bash, '-c', 'set -e; mkdir -p "$1"; cp -f "$2" "$1/cact-install"',
            'bash', lr_sbin, installer])

# The Makefile's `grub-bundle`: GRUB stage files built by the host tool.
grub_payload = build_root / 'grub'

run_target('grub-bundle',
  command: [bash, src_root / 'tools/make-grub-payload.sh', grub_payload])

# The Makefile's `stage-media`: kernel + cctkfs + grub payload into LocalRepo.
run_target('stage-media',
  command: [bash, '-c',
            'set -e; lrlib="$1"; kern="$2"; cctkfs="$3"; grubdir="$4"; ' +
            'test -f "$kern" || { echo "missing $kern — build the kernel first" >&2; exit 1; }; ' +
            'test -f "$cctkfs" || { echo "missing $cctkfs — build the LocalRepo image first" >&2; exit 1; }; ' +
            'test -f "$grubdir/i386-pc/core.img" || { echo "missing grub payload — run grub-bundle first" >&2; exit 1; }; ' +
            'mkdir -p "$lrlib/cact-install/boot"; ' +
            'cp -f "$kern" "$lrlib/cact-install/boot/kernel.bin"; ' +
            'cp -f "$cctkfs" "$lrlib/cact-install/boot/cctkfs.img"; ' +
            'rm -rf "$lrlib/cact-install/grub"; ' +
            'cp -r "$grubdir" "$lrlib/cact-install/grub"',
            'bash', lr_lib, kern_build / 'kernel.bin',
            lr_repo / 'cctkfs.img', grub_payload])
