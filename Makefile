NASM       ?= nasm
CC         ?= gcc
LD         ?= ld
OBJCOPY    ?= objcopy
HOST_CC    ?= cc
FS_STORAGE_LBA ?= 2048

HOST_ARCH ?= $(shell uname -m 2>/dev/null || echo unknown)
ARM64_HOST := $(filter aarch64 arm64,$(HOST_ARCH))

# NostaluxOS is an x86-64 guest even when it is built on an ARM64 host.
# Debian/Ubuntu cross tools use this prefix; other toolchains can override it,
# for example with CROSS_COMPILE=x86_64-elf-.
ifeq ($(origin CROSS_COMPILE),undefined)
ifneq ($(ARM64_HOST),)
CROSS_COMPILE := x86_64-linux-gnu-
else
CROSS_COMPILE :=
endif
endif

TARGET_CC      ?= $(if $(strip $(CROSS_COMPILE)),$(CROSS_COMPILE)gcc,$(CC))
TARGET_LD      ?= $(if $(strip $(CROSS_COMPILE)),$(CROSS_COMPILE)ld,$(LD))
TARGET_OBJCOPY ?= $(if $(strip $(CROSS_COMPILE)),$(CROSS_COMPILE)objcopy,$(OBJCOPY))

CONFLICT_CHECK := ./scripts/check-conflicts.sh

# Added -MMD -MP for automatic dependency tracking
CFLAGS := -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fcf-protection=none \
          -fno-pic -mno-red-zone -mgeneral-regs-only -nostdlib -nostartfiles \
          -Wall -Wextra -Ikernel/include -mno-mmx -mno-sse -mno-sse2 -mno-sse3 \
          -mno-ssse3 -mno-sse4 -mno-avx -DFS_STORAGE_LBA=$(FS_STORAGE_LBA) \
          -MMD -MP
# NASM 3.x diagnoses the bootloader's intentional fixed-address references
# between flat-binary sections. Probe those warning classes because NASM 2.x
# rejects their NASM 3.x names when all other assembler warnings are fatal.
NASM_RELOC_WARNING_FLAGS := $(shell \
	source=/tmp/nostalux-nasm-warning-probe.$$$$.asm; \
	output=/tmp/nostalux-nasm-warning-probe.$$$$.bin; \
	: > "$$source"; \
	if $(NASM) -f bin -Wall -Werror \
		-w-reloc-abs-word -w-reloc-abs-dword \
		-o "$$output" "$$source" >/dev/null 2>&1; then \
		printf '%s' '-w-reloc-abs-word -w-reloc-abs-dword'; \
	fi; \
	rm -f "$$source" "$$output")
NASMFLAGS := -Wall -Werror $(NASM_RELOC_WARNING_FLAGS)

BUILD_DIR := build
APP_BUILD_DIR := $(BUILD_DIR)/apps
BOOT_BIN := $(BUILD_DIR)/boot.bin
STAGE2_BIN := $(BUILD_DIR)/stage2.bin
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
KERNEL_BIN := $(BUILD_DIR)/kernel.bin
PAYLOAD_BIN := $(BUILD_DIR)/stage2_kernel.bin
OS_IMAGE := $(BUILD_DIR)/NostaluxOS.img
KERNEL_SRCS := $(wildcard kernel/*.c)
KERNEL_OBJS := $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(KERNEL_SRCS))
HELLO_APP_OBJ := $(APP_BUILD_DIR)/hello.o
HELLO_APP_LINKED := $(APP_BUILD_DIR)/hello.unstripped.elf
HELLO_APP_ELF := $(APP_BUILD_DIR)/hello.elf
HELLO_APP_BLOB_OBJ := $(APP_BUILD_DIR)/hello_blob.o
FAULT_APP_OBJ := $(APP_BUILD_DIR)/fault-probe.o
FAULT_APP_LINKED := $(APP_BUILD_DIR)/fault-probe.unstripped.elf
FAULT_APP_ELF := $(APP_BUILD_DIR)/fault-probe.elf
FAULT_APP_BLOB_OBJ := $(APP_BUILD_DIR)/fault_probe_blob.o
HANG_APP_OBJ := $(APP_BUILD_DIR)/hang-probe.o
HANG_APP_LINKED := $(APP_BUILD_DIR)/hang-probe.unstripped.elf
HANG_APP_ELF := $(APP_BUILD_DIR)/hang-probe.elf
HANG_APP_BLOB_OBJ := $(APP_BUILD_DIR)/hang_probe_blob.o
RFLAGS_APP_OBJ := $(APP_BUILD_DIR)/rflags-probe.o
RFLAGS_APP_LINKED := $(APP_BUILD_DIR)/rflags-probe.unstripped.elf
RFLAGS_APP_ELF := $(APP_BUILD_DIR)/rflags-probe.elf
RFLAGS_APP_BLOB_OBJ := $(APP_BUILD_DIR)/rflags_probe_blob.o
STACK_APP_OBJ := $(APP_BUILD_DIR)/stack-probe.o
STACK_APP_LINKED := $(APP_BUILD_DIR)/stack-probe.unstripped.elf
STACK_APP_ELF := $(APP_BUILD_DIR)/stack-probe.elf
STACK_APP_BLOB_OBJ := $(APP_BUILD_DIR)/stack_probe_blob.o
CALCULATOR_APP_OBJ := $(APP_BUILD_DIR)/calculator.o
CALCULATOR_APP_LINKED := $(APP_BUILD_DIR)/calculator.unstripped.elf
CALCULATOR_APP_ELF := $(APP_BUILD_DIR)/calculator.elf
CALCULATOR_APP_BLOB_OBJ := $(APP_BUILD_DIR)/calculator_blob.o
NOTEPAD_APP_OBJ := $(APP_BUILD_DIR)/notepad.o
NOTEPAD_APP_LINKED := $(APP_BUILD_DIR)/notepad.unstripped.elf
NOTEPAD_APP_ELF := $(APP_BUILD_DIR)/notepad.elf
NOTEPAD_APP_BLOB_OBJ := $(APP_BUILD_DIR)/notepad_blob.o
IMAGE_VIEWER_APP_OBJ := $(APP_BUILD_DIR)/image-viewer.o
IMAGE_VIEWER_APP_LINKED := $(APP_BUILD_DIR)/image-viewer.unstripped.elf
IMAGE_VIEWER_APP_ELF := $(APP_BUILD_DIR)/image-viewer.elf
IMAGE_VIEWER_APP_BLOB_OBJ := $(APP_BUILD_DIR)/image_viewer_blob.o
AI_ASSISTANT_APP_OBJ := $(APP_BUILD_DIR)/ai-assistant.o
AI_ASSISTANT_APP_LINKED := $(APP_BUILD_DIR)/ai-assistant.unstripped.elf
AI_ASSISTANT_APP_ELF := $(APP_BUILD_DIR)/ai-assistant.elf
AI_ASSISTANT_APP_BLOB_OBJ := $(APP_BUILD_DIR)/ai_assistant_blob.o
DESKTOP_APP_ELFS := $(CALCULATOR_APP_ELF) $(NOTEPAD_APP_ELF) \
	$(IMAGE_VIEWER_APP_ELF) $(AI_ASSISTANT_APP_ELF)
APP_ELFS := $(HELLO_APP_ELF) $(FAULT_APP_ELF) $(HANG_APP_ELF) \
	$(RFLAGS_APP_ELF) $(STACK_APP_ELF) $(DESKTOP_APP_ELFS)
KERNEL_EXTRA_OBJS := $(HELLO_APP_BLOB_OBJ) $(FAULT_APP_BLOB_OBJ) \
	$(HANG_APP_BLOB_OBJ) $(RFLAGS_APP_BLOB_OBJ) \
	$(STACK_APP_BLOB_OBJ) $(CALCULATOR_APP_BLOB_OBJ) \
	$(NOTEPAD_APP_BLOB_OBJ) $(IMAGE_VIEWER_APP_BLOB_OBJ) \
	$(AI_ASSISTANT_APP_BLOB_OBJ)

APP_CFLAGS := -std=gnu11 -Os -ffreestanding -fno-builtin \
	-fno-stack-protector -fcf-protection=none -fno-pic -fno-pie \
	-fno-asynchronous-unwind-tables -mno-red-zone -mgeneral-regs-only \
	-mcmodel=large -nostdlib -nostartfiles -Wall -Wextra -Werror \
	-Ikernel/include -Iapps \
	-mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-ssse3 -mno-sse4 \
	-mno-avx

# Include dependency files generated by GCC
DEPS := $(KERNEL_OBJS:.o=.d)

QEMU ?= qemu-system-x86_64
QEMU_NATIVE ?=
FLOCK ?= flock
IMAGE_LOCK ?= .nostalux-image.lock
PYTHON ?= python3
QEMU_SMOKE_TIMEOUT ?= 30
QEMU_SMOKE_LOCK_TIMEOUT ?= 3
QEMU_SMOKE_MEMORY_MIB ?= 128

# Cross-architecture virtualization is not possible with KVM/WHVP. QEMU's TCG
# translates the x86-64 guest on ARM64 hosts.
ifeq ($(origin QEMU_ACCEL),undefined)
ifneq ($(ARM64_HOST),)
QEMU_ACCEL := -accel tcg
else
QEMU_ACCEL :=
endif
endif

QEMU_SMOKE_ARGS ?= $(QEMU_ACCEL)

# Interactive runs prefer a native Windows QEMU executable when one can be
# discovered. Linux QEMU remains the portable fallback and powers headless runs.
QEMU_AUDIO_LINUX ?= -machine pcspk-audiodev=snd0 -audiodev pa,id=snd0
QEMU_AUDIO_NATIVE ?= -machine pcspk-audiodev=snd0 -audiodev dsound,id=snd0
QEMU_AUDIO_HEADLESS ?= -machine pcspk-audiodev=snd0 -audiodev none,id=snd0
# GTK keeps Windows physical X/Y motion in one frontend path, hides the host
# pointer, and preserves a 1:1 guest framebuffer scale. Hover grab also keeps
# keyboard focus with the guest while the pointer is over the display.
QEMU_DISPLAY_FULLSCREEN ?= -display gtk,full-screen=on,grab-on-hover=on,show-cursor=off,show-tabs=off,show-menubar=off,zoom-to-fit=on
QEMU_DISPLAY_WINDOWED ?= -display gtk,grab-on-hover=on,show-cursor=off,show-tabs=off,show-menubar=off,zoom-to-fit=off
QEMU_DISPLAY_HEADLESS ?= -display none -serial mon:stdio

# Preserve everything from the configured ATA filesystem LBA onward when
# replacing the bootloader/kernel during a normal build.
IMAGE_DATA_MIB ?= 32

define RUN_QEMU
	@set -eu; \
	if ! command -v "$(firstword $(FLOCK))" >/dev/null 2>&1; then \
		echo "Error: image locking requires '$(firstword $(FLOCK))' (provided by util-linux on WSL)." >&2; \
		exit 1; \
	fi; \
	exec 9>>"$(IMAGE_LOCK)"; \
	echo "Acquiring exclusive disk-image lock (other runs and rebuilds wait)..."; \
	$(FLOCK) --exclusive 9; \
	if [ "$(HEADLESS)" = "1" ]; then \
		if command -v "$(QEMU)" >/dev/null 2>&1; then \
			echo "Running headless with $(QEMU)"; \
			"$(QEMU)" $(QEMU_ACCEL) $(QEMU_DISPLAY_HEADLESS) $(QEMU_AUDIO_HEADLESS) \
				-drive format=raw,file=$(OS_IMAGE); \
			exit $$?; \
		fi; \
		echo "Error: $(QEMU) was not found. Set QEMU=/path/to/qemu-system-x86_64." >&2; \
		exit 1; \
	fi; \
	native_qemu='$(QEMU_NATIVE)'; \
	auto_native=1; \
	if [ -n "$(ARM64_HOST)" ] && command -v "$(QEMU)" >/dev/null 2>&1; then \
		auto_native=0; \
	fi; \
	if [ -z "$$native_qemu" ] && [ "$$auto_native" = "1" ]; then \
		native_qemu=$$(command -v qemu-system-x86_64.exe 2>/dev/null || true); \
	fi; \
	if [ -z "$$native_qemu" ] && [ "$$auto_native" = "1" ]; then \
		for candidate in \
			"/mnt/c/Program Files/qemu/qemu-system-x86_64.exe" \
			"/mnt/c/Program Files (x86)/qemu/qemu-system-x86_64.exe" \
			"/c/Program Files/qemu/qemu-system-x86_64.exe" \
			"/c/Program Files (x86)/qemu/qemu-system-x86_64.exe"; do \
			if [ -x "$$candidate" ]; then \
				native_qemu="$$candidate"; \
				break; \
			fi; \
		done; \
	fi; \
	if [ -n "$$native_qemu" ] && [ -x "$$native_qemu" ]; then \
		echo "Running with native Windows QEMU: $$native_qemu"; \
		"$$native_qemu" $(QEMU_ACCEL) $(1) $(QEMU_AUDIO_NATIVE) \
			-drive format=raw,file=$(OS_IMAGE); \
		exit $$?; \
	fi; \
	if command -v "$(QEMU)" >/dev/null 2>&1; then \
		if [ -n "$(ARM64_HOST)" ]; then \
			echo "ARM64 host detected; emulating the x86-64 guest with $(QEMU) and TCG."; \
		else \
			echo "Native Windows QEMU not found; using $(QEMU)."; \
		fi; \
		"$(QEMU)" $(QEMU_ACCEL) $(1) $(QEMU_AUDIO_LINUX) \
			-drive format=raw,file=$(OS_IMAGE); \
		exit $$?; \
	fi; \
	echo "Error: no QEMU executable found. Install QEMU or set QEMU_NATIVE/QEMU." >&2; \
	exit 1
endef

.PHONY: all apps clean test test-apps-v1 test-app-catalog-reclaim \
	test-app-services \
	test-app-behavior test-bmp \
	test-terminal-capture test-shell-capture test-fs-persistence \
	test-memory-accounting test-user-return test-vmmouse-decode \
	run run-windowed \
	run-fullscreen test-qemu-smoke qemu-smoke check-conflicts \
	check-target-tools

all: check-conflicts check-target-tools $(OS_IMAGE)

apps: check-conflicts check-target-tools $(APP_ELFS)

check-conflicts:
	@$(CONFLICT_CHECK)

check-target-tools:
	@set -eu; \
	missing=''; \
	for tool in "$(firstword $(NASM))" "$(firstword $(TARGET_CC))" "$(firstword $(TARGET_LD))" "$(firstword $(TARGET_OBJCOPY))"; do \
		if ! command -v "$$tool" >/dev/null 2>&1; then \
			missing="$$missing $$tool"; \
		fi; \
	done; \
	if [ -n "$$missing" ]; then \
		echo "Error: missing x86-64 build tools:$$missing" >&2; \
		if [ -n "$(ARM64_HOST)" ]; then \
			echo "ARM64 host detected ($(HOST_ARCH)); NostaluxOS needs an x86-64 cross-toolchain." >&2; \
			echo "On Debian/Ubuntu install: build-essential nasm gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu" >&2; \
			echo "For another toolchain, set CROSS_COMPILE to its prefix (for example x86_64-elf-)." >&2; \
		fi; \
		exit 1; \
	fi; \
	target=$$($(TARGET_CC) -dumpmachine 2>/dev/null || true); \
	case "$$target" in \
		x86_64*|amd64*) ;; \
		*) \
			echo "Error: $(TARGET_CC) targets '$$target', but NostaluxOS requires an x86-64 compiler." >&2; \
			echo "Set CROSS_COMPILE or TARGET_CC/TARGET_LD/TARGET_OBJCOPY to an x86-64 toolchain." >&2; \
			exit 1 ;; \
	esac

test: test-apps-v1 test-app-catalog-reclaim test-app-services \
	test-app-behavior test-bmp \
	test-terminal-capture test-shell-capture test-fs-persistence \
	test-memory-accounting test-user-return test-vmmouse-decode

test-apps-v1: check-target-tools $(APP_ELFS) | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror \
		-DNOSTALUX_HOST_TEST -Ikernel/include \
		tests/apps_v1_test.c kernel/app_abi.c kernel/app_catalog.c \
		kernel/app_manifest.c kernel/app_process.c kernel/elf64_loader.c \
		-o $(BUILD_DIR)/apps_v1_test
	$(BUILD_DIR)/apps_v1_test $(HELLO_APP_ELF) $(FAULT_APP_ELF) \
		$(HANG_APP_ELF) $(RFLAGS_APP_ELF) $(STACK_APP_ELF) \
		$(DESKTOP_APP_ELFS)

test-app-catalog-reclaim: check-target-tools $(HELLO_APP_ELF) | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror \
		-DNOSTALUX_HOST_TEST -DNOSTALUX_APP_CATALOG_FS_TEST \
		-Ikernel/include \
		tests/app_catalog_reclaim_test.c kernel/app_catalog.c \
		kernel/app_manifest.c kernel/elf64_loader.c \
		-o $(BUILD_DIR)/app_catalog_reclaim_test
	$(BUILD_DIR)/app_catalog_reclaim_test $(HELLO_APP_ELF)

test-app-services: | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Ikernel/include \
		tests/app_services_test.c kernel/app_services.c \
		kernel/app_manifest.c \
		-o $(BUILD_DIR)/app_services_test
	$(BUILD_DIR)/app_services_test

test-app-behavior: | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Werror \
		-DNOSTALUX_HOST_TEST -Ikernel/include -Iapps \
		tests/app_behavior_test.c -o $(BUILD_DIR)/app_behavior_test
	$(BUILD_DIR)/app_behavior_test

test-bmp: | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Ikernel/include \
		tests/bmp_test.c kernel/bmp.c -o $(BUILD_DIR)/bmp_test
	$(BUILD_DIR)/bmp_test

test-terminal-capture: | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Ikernel/include \
		tests/terminal_capture_test.c kernel/terminal.c \
		-o $(BUILD_DIR)/terminal_capture_test
	$(BUILD_DIR)/terminal_capture_test

test-shell-capture: | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -DNOSTALUX_HOST_TEST -Ikernel/include \
		tests/shell_capture_test.c kernel/shell.c kernel/kstdio.c \
		kernel/kstring.c kernel/terminal.c -o $(BUILD_DIR)/shell_capture_test
	$(BUILD_DIR)/shell_capture_test

test-fs-persistence: | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -DNOSTALUX_HOST_TEST \
		-DFS_STORAGE_LBA=$(FS_STORAGE_LBA) -Ikernel/include \
		tests/fs_persistence_test.c kernel/fs.c kernel/kstring.c \
		kernel/syslog.c -o $(BUILD_DIR)/fs_persistence_test
	$(BUILD_DIR)/fs_persistence_test

test-memory-accounting: | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Ikernel/include \
		tests/memory_accounting_test.c kernel/memtest.c kernel/system.c \
		-o $(BUILD_DIR)/memory_accounting_test
	$(BUILD_DIR)/memory_accounting_test

test-user-return: | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Ikernel/include \
		tests/user_return_test.c -o $(BUILD_DIR)/user_return_test
	$(BUILD_DIR)/user_return_test

test-vmmouse-decode: | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Ikernel/include \
		tests/vmmouse_decode_test.c kernel/vmmouse_decode.c \
		-o $(BUILD_DIR)/vmmouse_decode_test
	$(BUILD_DIR)/vmmouse_decode_test

# This target intentionally has no build-artifact prerequisites: it must never
# rebuild or write the user's primary disk image. Run `make` first when the
# image is missing or stale.
test-qemu-smoke: check-conflicts
	@set -eu; \
	if ! command -v "$(firstword $(PYTHON))" >/dev/null 2>&1; then \
		echo "Error: QEMU smoke testing requires $(firstword $(PYTHON))." >&2; \
		exit 1; \
	fi; \
	if ! command -v "$(QEMU)" >/dev/null 2>&1; then \
		echo "Error: QEMU smoke testing requires $(QEMU)." >&2; \
		exit 1; \
	fi; \
	if ! $(MAKE) --no-print-directory -q "$(OS_IMAGE)"; then \
		echo "Error: $(OS_IMAGE) is missing or stale; run 'make' before the smoke test." >&2; \
		exit 1; \
	fi; \
	$(PYTHON) scripts/qemu-smoke.py \
		--qemu "$(QEMU)" \
		--qemu-args "$(QEMU_SMOKE_ARGS)" \
		--image "$(OS_IMAGE)" \
		--image-lock "$(IMAGE_LOCK)" \
		--timeout "$(QEMU_SMOKE_TIMEOUT)" \
		--lock-timeout "$(QEMU_SMOKE_LOCK_TIMEOUT)" \
		--memory-mib "$(QEMU_SMOKE_MEMORY_MIB)"

qemu-smoke: test-qemu-smoke

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(APP_BUILD_DIR): | $(BUILD_DIR)
	@mkdir -p $(APP_BUILD_DIR)

$(HELLO_APP_OBJ): apps/hello.c kernel/include/app_abi.h \
		kernel/include/app_manifest.h Makefile | $(APP_BUILD_DIR)
	$(TARGET_CC) $(APP_CFLAGS) -c $< -o $@

$(HELLO_APP_LINKED): $(HELLO_APP_OBJ) apps/hello.ld | $(APP_BUILD_DIR)
	$(TARGET_LD) -nostdlib -z max-page-size=0x1000 -z noexecstack \
		-T apps/hello.ld -o $@ $(HELLO_APP_OBJ)

$(HELLO_APP_ELF): $(HELLO_APP_LINKED) | $(APP_BUILD_DIR)
	$(TARGET_OBJCOPY) --strip-all $< $@

$(HELLO_APP_BLOB_OBJ): apps/hello_blob.asm $(HELLO_APP_ELF) | $(APP_BUILD_DIR)
	$(NASM) -f elf64 apps/hello_blob.asm -o $@

$(FAULT_APP_OBJ): apps/fault_probe.c Makefile | $(APP_BUILD_DIR)
	$(TARGET_CC) $(APP_CFLAGS) -c $< -o $@

$(FAULT_APP_LINKED): $(FAULT_APP_OBJ) apps/hello.ld | $(APP_BUILD_DIR)
	$(TARGET_LD) -nostdlib -z max-page-size=0x1000 -z noexecstack \
		-T apps/hello.ld -o $@ $(FAULT_APP_OBJ)

$(FAULT_APP_ELF): $(FAULT_APP_LINKED) | $(APP_BUILD_DIR)
	$(TARGET_OBJCOPY) --strip-all $< $@

$(FAULT_APP_BLOB_OBJ): apps/fault_probe_blob.asm $(FAULT_APP_ELF) | $(APP_BUILD_DIR)
	$(NASM) -f elf64 apps/fault_probe_blob.asm -o $@

$(HANG_APP_OBJ): apps/hang_probe.c Makefile | $(APP_BUILD_DIR)
	$(TARGET_CC) $(APP_CFLAGS) -c $< -o $@

$(HANG_APP_LINKED): $(HANG_APP_OBJ) apps/hello.ld | $(APP_BUILD_DIR)
	$(TARGET_LD) -nostdlib -z max-page-size=0x1000 -z noexecstack \
		-T apps/hello.ld -o $@ $(HANG_APP_OBJ)

$(HANG_APP_ELF): $(HANG_APP_LINKED) | $(APP_BUILD_DIR)
	$(TARGET_OBJCOPY) --strip-all $< $@

$(HANG_APP_BLOB_OBJ): apps/hang_probe_blob.asm $(HANG_APP_ELF) | $(APP_BUILD_DIR)
	$(NASM) -f elf64 apps/hang_probe_blob.asm -o $@

$(RFLAGS_APP_OBJ): apps/rflags_probe.c kernel/include/app_abi.h \
		Makefile | $(APP_BUILD_DIR)
	$(TARGET_CC) $(APP_CFLAGS) -c $< -o $@

$(RFLAGS_APP_LINKED): $(RFLAGS_APP_OBJ) apps/hello.ld | $(APP_BUILD_DIR)
	$(TARGET_LD) -nostdlib -z max-page-size=0x1000 -z noexecstack \
		-T apps/hello.ld -o $@ $(RFLAGS_APP_OBJ)

$(RFLAGS_APP_ELF): $(RFLAGS_APP_LINKED) | $(APP_BUILD_DIR)
	$(TARGET_OBJCOPY) --strip-all $< $@

$(RFLAGS_APP_BLOB_OBJ): apps/rflags_probe_blob.asm \
		$(RFLAGS_APP_ELF) | $(APP_BUILD_DIR)
	$(NASM) -f elf64 apps/rflags_probe_blob.asm -o $@

$(STACK_APP_OBJ): apps/stack_probe.c Makefile | $(APP_BUILD_DIR)
	$(TARGET_CC) $(APP_CFLAGS) -c $< -o $@

$(STACK_APP_LINKED): $(STACK_APP_OBJ) apps/hello.ld | $(APP_BUILD_DIR)
	$(TARGET_LD) -nostdlib -z max-page-size=0x1000 -z noexecstack \
		-T apps/hello.ld -o $@ $(STACK_APP_OBJ)

$(STACK_APP_ELF): $(STACK_APP_LINKED) | $(APP_BUILD_DIR)
	$(TARGET_OBJCOPY) --strip-all $< $@

$(STACK_APP_BLOB_OBJ): apps/stack_probe_blob.asm \
		$(STACK_APP_ELF) | $(APP_BUILD_DIR)
	$(NASM) -f elf64 apps/stack_probe_blob.asm -o $@

$(CALCULATOR_APP_OBJ): apps/calculator.c apps/app_ui.h \
		kernel/include/app_abi.h Makefile | $(APP_BUILD_DIR)
	$(TARGET_CC) $(APP_CFLAGS) -c $< -o $@

$(NOTEPAD_APP_OBJ): apps/notepad.c apps/app_ui.h \
		kernel/include/app_abi.h Makefile | $(APP_BUILD_DIR)
	$(TARGET_CC) $(APP_CFLAGS) -c $< -o $@

$(IMAGE_VIEWER_APP_OBJ): apps/image_viewer.c apps/app_ui.h \
		kernel/include/app_abi.h Makefile | $(APP_BUILD_DIR)
	$(TARGET_CC) $(APP_CFLAGS) -c $< -o $@

$(AI_ASSISTANT_APP_OBJ): apps/ai_assistant.c apps/app_ui.h \
		kernel/include/app_abi.h Makefile | $(APP_BUILD_DIR)
	$(TARGET_CC) $(APP_CFLAGS) -c $< -o $@

$(CALCULATOR_APP_LINKED): $(CALCULATOR_APP_OBJ) apps/hello.ld | $(APP_BUILD_DIR)
	$(TARGET_LD) -nostdlib -z max-page-size=0x1000 -z noexecstack \
		-T apps/hello.ld -o $@ $(CALCULATOR_APP_OBJ)

$(NOTEPAD_APP_LINKED): $(NOTEPAD_APP_OBJ) apps/hello.ld | $(APP_BUILD_DIR)
	$(TARGET_LD) -nostdlib -z max-page-size=0x1000 -z noexecstack \
		-T apps/hello.ld -o $@ $(NOTEPAD_APP_OBJ)

$(IMAGE_VIEWER_APP_LINKED): $(IMAGE_VIEWER_APP_OBJ) apps/hello.ld | $(APP_BUILD_DIR)
	$(TARGET_LD) -nostdlib -z max-page-size=0x1000 -z noexecstack \
		-T apps/hello.ld -o $@ $(IMAGE_VIEWER_APP_OBJ)

$(AI_ASSISTANT_APP_LINKED): $(AI_ASSISTANT_APP_OBJ) apps/hello.ld | $(APP_BUILD_DIR)
	$(TARGET_LD) -nostdlib -z max-page-size=0x1000 -z noexecstack \
		-T apps/hello.ld -o $@ $(AI_ASSISTANT_APP_OBJ)

$(CALCULATOR_APP_ELF): $(CALCULATOR_APP_LINKED) | $(APP_BUILD_DIR)
	$(TARGET_OBJCOPY) --strip-all $< $@

$(NOTEPAD_APP_ELF): $(NOTEPAD_APP_LINKED) | $(APP_BUILD_DIR)
	$(TARGET_OBJCOPY) --strip-all $< $@

$(IMAGE_VIEWER_APP_ELF): $(IMAGE_VIEWER_APP_LINKED) | $(APP_BUILD_DIR)
	$(TARGET_OBJCOPY) --strip-all $< $@

$(AI_ASSISTANT_APP_ELF): $(AI_ASSISTANT_APP_LINKED) | $(APP_BUILD_DIR)
	$(TARGET_OBJCOPY) --strip-all $< $@

$(CALCULATOR_APP_BLOB_OBJ): apps/calculator_blob.asm \
		$(CALCULATOR_APP_ELF) | $(APP_BUILD_DIR)
	$(NASM) -f elf64 apps/calculator_blob.asm -o $@

$(NOTEPAD_APP_BLOB_OBJ): apps/notepad_blob.asm \
		$(NOTEPAD_APP_ELF) | $(APP_BUILD_DIR)
	$(NASM) -f elf64 apps/notepad_blob.asm -o $@

$(IMAGE_VIEWER_APP_BLOB_OBJ): apps/image_viewer_blob.asm \
		$(IMAGE_VIEWER_APP_ELF) | $(APP_BUILD_DIR)
	$(NASM) -f elf64 apps/image_viewer_blob.asm -o $@

$(AI_ASSISTANT_APP_BLOB_OBJ): apps/ai_assistant_blob.asm \
		$(AI_ASSISTANT_APP_ELF) | $(APP_BUILD_DIR)
	$(NASM) -f elf64 apps/ai_assistant_blob.asm -o $@

$(KERNEL_ELF): kernel/entry.asm $(KERNEL_OBJS) $(KERNEL_EXTRA_OBJS) \
		kernel/linker.ld | $(BUILD_DIR)
	$(NASM) -f elf64 kernel/entry.asm -o $(BUILD_DIR)/entry.o
	$(TARGET_LD) -nostdlib -z max-page-size=0x1000 -T kernel/linker.ld \
		-o $@ $(BUILD_DIR)/entry.o $(KERNEL_OBJS) $(KERNEL_EXTRA_OBJS)

$(BUILD_DIR)/%.o: kernel/%.c Makefile | $(BUILD_DIR)
	$(TARGET_CC) $(CFLAGS) -c $< -o $@

$(KERNEL_BIN): $(KERNEL_ELF) | $(BUILD_DIR)
	$(TARGET_OBJCOPY) -O binary $(KERNEL_ELF) $@

$(STAGE2_BIN): bootloader/stage2.asm $(KERNEL_BIN) | $(BUILD_DIR)
	@KERNEL_SIZE=$$(stat -c%s $(KERNEL_BIN)); \
	$(NASM) -f bin $(NASMFLAGS) -DKERNEL_SIZE_BYTES=$$KERNEL_SIZE bootloader/stage2.asm -o $@

$(PAYLOAD_BIN): $(STAGE2_BIN) $(KERNEL_BIN) | $(BUILD_DIR)
	cat $(STAGE2_BIN) $(KERNEL_BIN) > $@

$(BOOT_BIN): bootloader/boot.asm $(PAYLOAD_BIN) | $(BUILD_DIR)
	@TOTAL_SIZE=$$(stat -c%s $(PAYLOAD_BIN)); \
	TOTAL_SECTORS=$$(( (TOTAL_SIZE + 511) / 512 )); \
	$(NASM) -f bin $(NASMFLAGS) -DTOTAL_SECTORS=$$TOTAL_SECTORS bootloader/boot.asm -o $@

# DrvFS can timestamp a newly replaced image fractionally ahead of WSL's
# clock, so the recipe lets that subsecond skew settle before returning.
$(OS_IMAGE): $(BOOT_BIN) $(PAYLOAD_BIN)
	@set -eu; \
	if ! command -v "$(firstword $(FLOCK))" >/dev/null 2>&1; then \
		echo "Error: image locking requires '$(firstword $(FLOCK))' (provided by util-linux on WSL)." >&2; \
		exit 1; \
	fi; \
	exec 9>>"$(IMAGE_LOCK)"; \
	echo "Acquiring exclusive disk-image lock..."; \
	$(FLOCK) --exclusive 9; \
	TMP_IMAGE=$$(mktemp '$@.tmp.XXXXXX'); \
	SAVED_STORAGE=$$(mktemp '$@.storage.tmp.XXXXXX'); \
	FS_OFFSET=$$(( $(FS_STORAGE_LBA) * 512 )); \
	MIN_IMAGE_SIZE=$$(( FS_OFFSET + $(IMAGE_DATA_MIB) * 1024 * 1024 )); \
	trap 'rm -f "$$TMP_IMAGE" "$$SAVED_STORAGE"' 0 1 2 15; \
	if [ -f '$@' ]; then \
		OLD_SIZE=$$(stat -c%s '$@'); \
		if [ "$$OLD_SIZE" -gt "$$FS_OFFSET" ]; then \
			dd if='$@' of="$$SAVED_STORAGE" bs=512 skip=$(FS_STORAGE_LBA) 2>/dev/null; \
		fi; \
	fi; \
	cat $(BOOT_BIN) $(PAYLOAD_BIN) > "$$TMP_IMAGE"; \
	BOOT_END=$$(stat -c%s "$$TMP_IMAGE"); \
	if [ "$$BOOT_END" -gt "$$FS_OFFSET" ]; then \
		echo "Error: boot payload ($$BOOT_END bytes) overlaps filesystem LBA $(FS_STORAGE_LBA)." >&2; \
		exit 1; \
	fi; \
	truncate -s "$$FS_OFFSET" "$$TMP_IMAGE"; \
	if [ -s "$$SAVED_STORAGE" ]; then \
		cat "$$SAVED_STORAGE" >> "$$TMP_IMAGE"; \
	fi; \
	IMAGE_SIZE=$$(stat -c%s "$$TMP_IMAGE"); \
	if [ "$$IMAGE_SIZE" -lt "$$MIN_IMAGE_SIZE" ]; then \
		truncate -s "$$MIN_IMAGE_SIZE" "$$TMP_IMAGE"; \
	fi; \
	mv "$$TMP_IMAGE" '$@'; \
	touch '$@'; \
	sleep 1; \
	rm -f "$$SAVED_STORAGE"; \
	trap - 0 1 2 15

clean:
	@set -eu; \
	case "$(BUILD_DIR)" in \
		build|./build) ;; \
		*) \
			echo "Error: refusing to clean unexpected build directory '$(BUILD_DIR)'." >&2; \
			exit 1 ;; \
	esac; \
	if ! command -v "$(firstword $(FLOCK))" >/dev/null 2>&1; then \
		echo "Error: image locking requires '$(firstword $(FLOCK))' (provided by util-linux on WSL)." >&2; \
		exit 1; \
	fi; \
	exec 9>>"$(IMAGE_LOCK)"; \
	echo "Acquiring exclusive disk-image lock before cleaning..."; \
	$(FLOCK) --exclusive 9; \
	rm -rf -- "$(BUILD_DIR)"

run: check-conflicts check-target-tools $(OS_IMAGE)
	$(call RUN_QEMU,$(QEMU_DISPLAY_WINDOWED))

run-windowed: check-conflicts check-target-tools $(OS_IMAGE)
	$(call RUN_QEMU,$(QEMU_DISPLAY_WINDOWED))

run-fullscreen: check-conflicts check-target-tools $(OS_IMAGE)
	$(call RUN_QEMU,$(QEMU_DISPLAY_FULLSCREEN))

-include $(DEPS)
