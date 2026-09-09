# Makefile for cact-install — native CactOS disk installer wizard (/sbin/cact-install)
#
#   make                 build build/sbin/cact-install
#   make install         stage the binary into LocalRepo lib/sbin
#   make grub-bundle     build GRUB stage files into build/grub (host grub-mkimage)
#   make stage-media     stage kernel.bin + cctkfs.img + grub into LocalRepo
#                        lib/cact-install (run AFTER the LocalRepo image exists)
#   make clean

ROOT := $(abspath .)

CACTLIB ?= $(abspath ../CactLibc-x86_32)
LR_SBIN ?= $(abspath ../LocalRepoCactOS-x86_32/lib/sbin)
LR_LIB  ?= $(abspath ../LocalRepoCactOS-x86_32/lib)
KERN_BUILD ?= $(abspath ../CactKernel-x86_32/build)
LR_REPO ?= $(abspath ../LocalRepoCactOS-x86_32)

CC      := gcc
LD      := ld
START_O := $(CACTLIB)/build/pic/start.o
LIBC_SO := $(CACTLIB)/clibc.so

CFLAGS := -m32 -ffreestanding -fPIE -fno-stack-protector -nostdlib \
          -ffunction-sections -fdata-sections \
          -I$(CACTLIB)/include -Wall -Wextra

LDFLAGS := -m elf_i386 -pie --dynamic-linker=/lib/ld.so --hash-style=both \
           -nostdlib --gc-sections -T $(ROOT)/link.ld

SRCS := $(wildcard $(ROOT)/src/*.c)
OBJD := $(ROOT)/build/obj
OUT  := $(ROOT)/build/sbin/cact-install

GRUB_PAYLOAD := $(ROOT)/build/grub
GRUB_BUNDLE  := $(ROOT)/tools/make-grub-payload.sh

.PHONY: all install grub-bundle stage-media clean

all: $(OUT)

$(LIBC_SO) $(START_O):
	@test -f $(LIBC_SO) && test -f $(START_O) || \
		(echo >&2 "Missing libc — build CactLibc-x86_32 first (CACTLIB=$(CACTLIB))"; exit 1)

$(OBJD):
	mkdir -p $(OBJD)

$(OBJD)/%.o: $(ROOT)/src/%.c | $(OBJD) $(LIBC_SO) $(START_O)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT): $(patsubst $(ROOT)/src/%.c,$(OBJD)/%.o,$(SRCS)) $(START_O) $(LIBC_SO)
	@mkdir -p $(dir $(OUT))
	$(LD) $(LDFLAGS) $(START_O) $(patsubst $(ROOT)/src/%.c,$(OBJD)/%.o,$(SRCS)) $(LIBC_SO) -o $@

grub-bundle:
	$(GRUB_BUNDLE) $(GRUB_PAYLOAD)

install: all
	@mkdir -p $(LR_SBIN)
	cp -f $(OUT) $(LR_SBIN)/cact-install

stage-media: all
	@test -f $(KERN_BUILD)/kernel.bin || \
		(echo >&2 "missing $(KERN_BUILD)/kernel.bin — build CactKernel first (KERN_BUILD=$(KERN_BUILD))"; exit 1)
	@test -f $(LR_REPO)/cctkfs.img || \
		(echo >&2 "missing $(LR_REPO)/cctkfs.img — build the LocalRepo image first"; exit 1)
	@test -f $(GRUB_PAYLOAD)/i386-pc/core.img || \
		(echo >&2 "missing grub payload — run 'make grub-bundle' first"; exit 1)
	@mkdir -p $(LR_LIB)/cact-install/boot
	cp -f $(KERN_BUILD)/kernel.bin $(LR_LIB)/cact-install/boot/kernel.bin
	cp -f $(LR_REPO)/cctkfs.img     $(LR_LIB)/cact-install/boot/cctkfs.img
	rm -rf $(LR_LIB)/cact-install/grub
	cp -r $(GRUB_PAYLOAD) $(LR_LIB)/cact-install/grub

clean:
	rm -rf build
