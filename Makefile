# Shusha build.  Kernel and drivers come from ICDA (see NOTICE);
# everything under user/ is Shusha's own userspace.

CC    ?= gcc
ASM   ?= nasm
LD    ?= ld
QEMU  ?= qemu-system-x86_64
# -cpu max exposes RDRAND/RDSEED under TCG so the CSPRNG gets a hardware source
QEMU_FLAGS ?= -cpu max -m 256M
# 1 = mirror the user console to serial (used by the smoke test)
SERIAL_MIRROR ?= 0
B     := build

KCFLAGS := -ffreestanding -O0 -Wall -Wextra -fno-exceptions -fno-pie -no-pie \
           -fno-asynchronous-unwind-tables -Ikernel -I. -Icrypto -fno-stack-protector \
           -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
           -DSERIAL_SHELL_MIRROR=$(SERIAL_MIRROR) -DCI_SELFTEST=0 -DCI_IMAGE=0 -DSERIAL_VERBOSE=0 -DSC_FREESTANDING \
           -MMD -MP

UCFLAGS := -ffreestanding -O2 -Wall -Wextra -fno-pie -no-pie -mcmodel=large \
           -fno-asynchronous-unwind-tables -fno-stack-protector \
           -mno-mmx -mno-sse -mno-sse2 -nostdlib -Iuser/include -Icrypto -DSC_FREESTANDING \
           -MMD -MP

# ---- kernel -----------------------------------------------------------------

KERNEL_C   := $(filter-out kernel/drivers/audio/sb16.c,$(shell find kernel -name '*.c'))
KERNEL_C   += $(wildcard crypto/*.c)
KERNEL_ASM := kernel/boot.asm kernel/cpu/gdt_flush.asm kernel/cpu/isr.asm \
              kernel/proc/sched.asm kernel/proc/user_enter.asm
KERNEL_BASE_OBJ := $(KERNEL_C:%.c=$(B)/k/%.o) $(KERNEL_ASM:%.asm=$(B)/k/%.asm.o)
KERNEL_OBJ := $(KERNEL_BASE_OBJ) $(B)/k/userprogs.o

# ---- userspace --------------------------------------------------------------
# Each program: user/<dir>/<name>.c -> $(B)/user/<name>.app
# USERPROGS maps the path inside the system to the built file.

ULIB_OBJ := $(B)/u/lib/crt0.asm.o $(B)/u/lib/lib.o $(B)/u/lib/net.o $(B)/u/crypto/mem.o
UGUI_OBJ := $(B)/u/lib/ui.o $(B)/u/lib/draw.o
UPROGS   := init shell wm explorer terminal ipccheck fscheck lanping workspaced ws workspace \
            authd launch greeter login users accounts code seccheck
UCRYPTO  := $(patsubst %,$(B)/u/crypto/%.o,aead chacha20 poly1305 sha2 ed25519 mlkem keccak wrap)

USERPROGS := /sbin/init.app=$(B)/user/init.app \
             /apps/shell.app=$(B)/user/shell.app \
             /apps/wm.app=$(B)/user/wm.app \
             /apps/explorer.app=$(B)/user/explorer.app \
             /apps/terminal.app=$(B)/user/terminal.app \
             /apps/ipccheck.app=$(B)/user/ipccheck.app \
             /apps/fscheck.app=$(B)/user/fscheck.app \
             /apps/lanping.app=$(B)/user/lanping.app \
             /sbin/workspaced.app=$(B)/user/workspaced.app \
             /apps/ws.app=$(B)/user/ws.app \
             /apps/workspace.app=$(B)/user/workspace.app \
             /sbin/authd.app=$(B)/user/authd.app \
             /sbin/launch.app=$(B)/user/launch.app \
             /sbin/greeter.app=$(B)/user/greeter.app \
             /sbin/login.app=$(B)/user/login.app \
             /apps/users.app=$(B)/user/users.app \
             /apps/accounts.app=$(B)/user/accounts.app \
             /apps/code.app=$(B)/user/code.app \
             /apps/seccheck.app=$(B)/user/seccheck.app

# Test images (lan.iso): same programs, but init starts a root shell on the
# text console instead of the login, so scripts can drive the machines.
USERPROGS_TEST := $(subst /sbin/init.app=$(B)/user/init.app,/sbin/init.app=$(B)/user/init-test.app,$(USERPROGS))

UAPPS := $(UPROGS:%=$(B)/user/%.app)

.PHONY: all clean qemu qemu-headless qemu-smoke test editor-test
all: shusha.iso

$(B)/k/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(B)/k/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@

$(B)/u/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(B)/u/crypto/%.o: crypto/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(B)/u/%.asm.o: user/%.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@

$(B)/user/init.app:  $(B)/u/sbin/init.o $(ULIB_OBJ)
$(B)/user/shell.app: $(B)/u/apps/shell.o $(ULIB_OBJ)
$(B)/user/wm.app: $(B)/u/apps/wm.o $(B)/u/lib/draw.o $(B)/u/gen/wallpaper.asm.o $(ULIB_OBJ)

# Wallpaper artwork: committed PNG -> raw premultiplied pixels (stdlib
# Python only) -> linked into the WM.  Regenerate the PNG itself with
# scripts/gen-wallpaper.py (needs Pillow).
$(B)/gen/wallpaper.raw: user/assets/xari-bulbul.png scripts/png2raw.py
	@mkdir -p $(dir $@)
	python3 scripts/png2raw.py $< $@

$(B)/u/gen/wallpaper.asm.o: $(B)/gen/wallpaper.raw
	@mkdir -p $(dir $@)
	printf 'section .rodata\nglobal wallpaper_raw\nalign 16\nwallpaper_raw: incbin "%s"\nsection .note.GNU-stack noalloc noexec nowrite progbits\n' $< > $(B)/gen/wallpaper.asm
	$(ASM) -f elf64 $(B)/gen/wallpaper.asm -o $@
$(B)/user/explorer.app: $(B)/u/apps/explorer.o $(UGUI_OBJ) $(ULIB_OBJ)
$(B)/user/terminal.app: $(B)/u/apps/terminal.o $(B)/u/lib/wsclient.o $(UGUI_OBJ) $(ULIB_OBJ)
$(B)/user/ipccheck.app: $(B)/u/tests/ipccheck.o $(ULIB_OBJ)
$(B)/user/fscheck.app: $(B)/u/tests/fscheck.o $(ULIB_OBJ)
$(B)/user/lanping.app: $(B)/u/tests/lanping.o $(ULIB_OBJ)
$(B)/user/seccheck.app: $(B)/u/tests/seccheck.o $(ULIB_OBJ)
$(B)/user/workspaced.app: $(B)/u/sbin/workspaced.o $(B)/u/lib/chan.o $(UCRYPTO) $(ULIB_OBJ)
$(B)/user/ws.app: $(B)/u/apps/ws.o $(B)/u/lib/wsclient.o $(ULIB_OBJ)
$(B)/user/workspace.app: $(B)/u/apps/workspace.o $(B)/u/lib/wsclient.o $(UGUI_OBJ) $(ULIB_OBJ)
$(B)/user/authd.app: $(B)/u/sbin/authd.o $(B)/u/lib/svc.o $(B)/u/crypto/argon2.o $(B)/u/crypto/blake2b.o $(ULIB_OBJ)
$(B)/user/launch.app: $(B)/u/sbin/launch.o $(ULIB_OBJ)
$(B)/user/greeter.app: $(B)/u/sbin/greeter.o $(B)/u/lib/svc.o $(UGUI_OBJ) $(ULIB_OBJ)
$(B)/user/login.app: $(B)/u/sbin/login.o $(B)/u/lib/svc.o $(ULIB_OBJ)
$(B)/user/users.app: $(B)/u/apps/users.o $(B)/u/lib/svc.o $(ULIB_OBJ)
$(B)/user/accounts.app: $(B)/u/apps/accounts.o $(B)/u/lib/svc.o $(UGUI_OBJ) $(ULIB_OBJ)
$(B)/user/code.app: $(B)/u/apps/code.o $(B)/u/apps/code_lang.o $(B)/u/apps/code_emmet.o $(B)/u/lib/wsclient.o \
                    $(UGUI_OBJ) $(ULIB_OBJ)

$(B)/u/sbin/init-test.o: user/sbin/init.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -DSHUSHA_TEST_IMAGE -c $< -o $@

$(B)/user/init-test.app: $(B)/u/sbin/init-test.o $(ULIB_OBJ) user/lib/user.ld
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -static -T user/lib/user.ld -o $@ $(filter %.o,$^)

$(UAPPS): user/lib/user.ld
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -static -T user/lib/user.ld -o $@ $(filter %.o,$^)

$(B)/gen/userprogs.asm: scripts/gen-initramfs.sh Makefile $(UAPPS)
	@mkdir -p $(dir $@)
	sh scripts/gen-initramfs.sh $@ $(USERPROGS)

$(B)/k/userprogs.o: $(B)/gen/userprogs.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@

$(B)/gen/userprogs-test.asm: scripts/gen-initramfs.sh Makefile $(UAPPS) $(B)/user/init-test.app
	@mkdir -p $(dir $@)
	sh scripts/gen-initramfs.sh $@ $(USERPROGS_TEST)

$(B)/k/userprogs-test.o: $(B)/gen/userprogs-test.asm
	@mkdir -p $(dir $@)
	$(ASM) -f elf64 $< -o $@

$(B)/kernel.bin: $(KERNEL_OBJ) kernel/linker.ld
	$(CC) -T kernel/linker.ld -o $@ -ffreestanding -O0 -nostdlib -fno-pie -no-pie \
	      $(KERNEL_OBJ) -lgcc

$(B)/kernel-test.bin: $(KERNEL_BASE_OBJ) $(B)/k/userprogs-test.o kernel/linker.ld
	$(CC) -T kernel/linker.ld -o $@ -ffreestanding -O0 -nostdlib -fno-pie -no-pie \
	      $(KERNEL_BASE_OBJ) $(B)/k/userprogs-test.o -lgcc

shusha.iso: $(B)/kernel.bin boot/grub/grub.cfg
	rm -rf $(B)/iso && mkdir -p $(B)/iso/boot/grub $(B)/iso/EFI/BOOT
	cp $(B)/kernel.bin $(B)/iso/boot/kernel.bin
	cp boot/grub/grub.cfg $(B)/iso/boot/grub/grub.cfg
	grub-mkstandalone -O x86_64-efi -o $(B)/iso/EFI/BOOT/BOOTX64.EFI \
	    "boot/grub/grub.cfg=boot/grub/grub.cfg"
	grub-mkrescue -o $@ $(B)/iso 2>/dev/null

# ---- run / test -------------------------------------------------------------

qemu: shusha.iso
	$(QEMU) $(QEMU_FLAGS) -cdrom $< -serial stdio -no-reboot

qemu-headless: shusha.iso
	$(QEMU) $(QEMU_FLAGS) -cdrom $< -serial stdio -display none -monitor none -no-reboot

# ISO for the multi-machine LAN tests: boots to the text shell so each
# VM's output can be read from its serial log (scripts/lan.py).
lan.iso: $(B)/kernel-test.bin boot/grub/grub.cfg
	rm -rf $(B)/lan-iso && mkdir -p $(B)/lan-iso/boot/grub
	cp $(B)/kernel-test.bin $(B)/lan-iso/boot/kernel.bin
	sed 's|multiboot2 /boot/kernel.bin icda.live=1|multiboot2 /boot/kernel.bin icda.live=1 shusha.text|' \
	    boot/grub/grub.cfg > $(B)/lan-iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(B)/lan-iso 2>/dev/null

qemu-smoke:
	$(MAKE) clean
	$(MAKE) SERIAL_MIRROR=1 shusha.iso
	sh scripts/qemu-smoke.sh shusha.iso

# Host-side crypto tests against official vectors (no QEMU needed).
test:
	@mkdir -p $(B)/test
	$(CC) -O2 -Wall -Wextra -Werror -Icrypto -Itests -o $(B)/test/crypto_test \
	      tests/crypto_test.c $(wildcard crypto/*.c)
	$(B)/test/crypto_test

# Host tests for the editor's Emmet expansion and language recognition.
editor-test:
	@mkdir -p $(B)/test
	$(CC) -O2 -Wall -Wextra -Werror -Iuser/include -o $(B)/test/editor_test tests/emmet_test.c \
	      user/apps/code_emmet.c user/apps/code_lang.c user/lib/lib.c
	$(B)/test/editor_test

clean:
	rm -rf $(B) shusha.iso lan.iso

-include $(shell find $(B) -name '*.d' 2>/dev/null)
