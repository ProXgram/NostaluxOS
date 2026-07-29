# NostaluxOS

NostaluxOS is a small x86-64 hobby operating system built as a learning project. It boots through a handcrafted
two-stage BIOS loader, enters long mode, and runs a freestanding kernel with a graphical shell and desktop.

## Current features

- **Handmade boot flow** — an MBR boot sector and second-stage loader load the kernel, configure paging, and enter
  64-bit long mode.
- **VBE framebuffer console** — the loader selects an 800x600, 32-bit VBE mode. The text console and desktop are
  rendered into that linear framebuffer; this is not a VGA text-mode interface.
- **Interactive shell** — `help` lists commands for system information, colors, history, arithmetic, files, a
  non-destructive 64 KB memory sample, games, sound, reboot, shutdown, and launching the GUI.
- **Truthful flat filesystem** — `ls`, `cat`, `hexdump`, `touch`, `write`, `append`, and `rm` normally operate on a
  persistent ATA-backed filesystem stored inside the raw disk image. Missing, unreadable, or corrupt storage mounts
  an explicitly labeled volatile session volume; corrupt media is preserved without automatic formatting.
- **Graphical desktop** — `gui` opens a Start menu, taskbar, movable and resizable kernel windows, Files, Paint,
  Notepad, Calculator, settings, monitors, games, and other retro desktop apps.
- **Writable desktop apps** — Files creates, opens, renames, and deletes files. Notepad edits and saves text files,
  including insertion at a clicked cursor position, while Paint saves its 17x17 canvas as a real 24-bit BMP that
  Image Viewer can reopen. Files, Settings, Browser, About, and the shell identify whether storage is persistent or
  session-only.
- **Shell-backed GUI Terminal** — desktop terminal commands go through the same dispatcher and handlers as the boot
  shell. Commands that need exclusive console control or would stop the desktop are clearly reported as unavailable.
- **Local Browser** — Browser opens real text files with `file:<name>` and automatically refreshes `about:files`,
  `about:system`, and the live read-only `system.log`. It explicitly rejects Internet URLs because there is no network
  driver or TCP/IP stack.
- **Kernel-backed diagnostics** — System Monitor graphs measured CPU busy-versus-idle time and distinguishes
  normalized usable RAM from the captured BIOS E820 map, currently mapped RAM, fixed reservations, and committed heap
  memory. Task Manager enumerates only genuine scheduler tasks and isolated app processes instead of presenting
  desktop windows or synthetic work as processes.
- **Desktop settings** — theme and wallpaper choices are stored in `desktop.cfg` and restored when storage is
  persistent; a volatile backend clearly reports that changes last only for the current session.
- **Filesystem image viewer** — Files validates image bytes before reporting success, and the isolated Image Viewer
  renders valid uncompressed 24-bit BMP files. A small `nostalux.bmp` is installed automatically as a working example;
  BMPs can contain up to 8,191 bytes when enough shared extent records are free.
- **AI Assistant** — a small offline, rule-based ELF app answers questions about real NostaluxOS features and reads
  the actual RTC for time requests. It is deterministic rather than a trained model or network service, and it does
  not pretend to perform actions it cannot perform.
- **Isolated Apps v1** — a strict x86-64 ELF loader validates fixed-address executables, rejects writable code, and
  maps each process into its own ring-3 address space with a guarded NX stack. The checked `INT 0x80` ABI provides
  capability-gated file handles and atomic file replacement, retained windows, queued keyboard/pointer input, dynamic
  memory mappings, startup arguments, logging, time, yield, and exit. Every interrupt and system-call return sanitizes
  user flags and safely converts malformed user instruction/stack state into a contained ring-3 fault, so app crashes
  terminate only that process.
- **Separate desktop applications** — Calculator, Notepad, Image Viewer, and AI Assistant are independently built ELF
  executables packaged inside the read-only OS image. The kernel reinspects their immutable ELF bytes on every launch,
  so app packages consume no records in the small user filesystem. Notepad atomically replaces its selected file when
  saving.
- **Real app launcher and isolation probes** — `apps` lists validated manifests and bounded process history;
  `app hello` runs the embedded sample, while `app fault-probe` deliberately touches supervisor-only memory to prove
  that the page fault is contained and the shell keeps running. `app hang-probe` sets the direction flag and spins
  without yielding; timer preemption returns control to the shell, where `appkill <process-id>` stops it.
  `rflags-probe` and `stack-probe` are integration diagnostics for hostile interrupt-return flags and stack pointers.
  The hello sample also runs once during boot.

The four migrated desktop apps use retained app windows that can be focused, closed, dragged within the desktop work
area, and hidden or restored with Show Desktop. Their compositor is intentionally small: these windows do not yet
share the kernel desktop's z-order, minimize, maximize, resize, or taskbar-tab implementation; isolated-app windows
form a separate overlay capped at eight windows total and two per process. Kernel tasks still yield cooperatively,
while ring-3 apps are preempted every five 100 Hz PIT ticks (about 50 ms). Every app task has independent
x87/MMX/SSE/SSE2 state; AVX is disabled until the kernel gains an XSAVE-based context format.

## Requirements

The supported build environment is an x86-64 or ARM64 Linux system, including WSL, with:

- GNU `make`
- `nasm`
- an x86-64 GNU C toolchain (`gcc` on x86-64, or an x86-64 cross-compiler on ARM64)
- matching GNU binutils (`ld` and `objcopy`)
- a POSIX shell, GNU coreutils, `find`, and `grep`, including `cat`, `stat`, `dd`, and `truncate`
- util-linux `flock`, used to prevent QEMU, rebuilds, and cleaning from accessing the writable image concurrently
- QEMU (`qemu-system-x86_64`), plus its GTK display module for graphical runs, if you want to run the image
- Python 3 if you want to run the automated headless QEMU smoke test

The kernel requires at least **9 MiB of guest RAM** and works better with **32 MiB or more**. QEMU's normal default
memory allocation is sufficient. The virtual machine also needs a BIOS that provides an E820 physical-memory map and
a VBE implementation that exposes the requested 800x600x32 mode.

### ARM64 host support

NostaluxOS remains an x86-64 BIOS operating system. On an `aarch64` or `arm64` Linux/WSL host, the Makefile
automatically selects an x86-64 cross-toolchain and runs `qemu-system-x86_64` with QEMU's TCG software translator.
The QEMU executable name describes the guest architecture, so it is also the correct executable on an ARM64 host.

On Debian or Ubuntu ARM64, install the native build tools, x86-64 cross-toolchain, and QEMU frontend with:

```sh
sudo apt update
sudo apt install build-essential nasm gcc-x86-64-linux-gnu \
  binutils-x86-64-linux-gnu qemu-system-x86 qemu-system-gui util-linux
```

Then use the normal commands:

```sh
make
make test
make run
```

Software translation is slower than running on an x86-64 host, but the guest functionality is the same. If a
toolchain uses another prefix, override the default, for example:

```sh
make CROSS_COMPILE=x86_64-elf-
```

This support covers ARM64 Linux and ARM64 WSL hosts. A native Apple Silicon/macOS launcher is not currently included
because the build and interactive display path depend on GNU userland tools and QEMU's GTK frontend.

## Build

```sh
make
```

This creates `build/NostaluxOS.img`, a raw disk image containing the bootloader, kernel, and writable filesystem area.
The conflict check reads the source tree directly, so the build works from a ZIP extraction or Downloads folder even
when no `.git` directory is present.

Run all host-side validation tests with:

```sh
make test
```

The suite covers strict ELF and manifest validation, bounded process-history reuse, capability and ownership checks
for the app services, retained-window event routing, app-service dynamic-memory mapping limits and ownership,
Calculator/Notepad/AI behavior, BMP parsing, legacy app-package reclamation, terminal and shell capture, malformed
filesystem extents, commit rollback and persistence reloads, protected reserved package names, VMMouse coordinate
decoding, live log projection, normalized user-return frames, and normalized 64-bit E820 memory accounting.

The QEMU integration test is separate because QEMU is an optional dependency:

```sh
make
make test-qemu-smoke
```

`make test-qemu-smoke` never rebuilds or writes `build/NostaluxOS.img`; it fails with instructions to run `make` if
that image is missing or stale. The harness takes a shared image lock only while making a byte-for-byte disposable
copy, marks that copy read-only, and also enables QEMU snapshot writes. Through QEMU's control monitor, it verifies
that the CPU reaches a CS64/CPL0 halt in the fixed kernel address region, checks that a nonuniform framebuffer of at
least 640x480 contains rendered output, types `app hang-probe`, and observes the probe executing in ring 3 with a
SysV-aligned entry stack plus scrubbed general, x87/MMX, and SSE registers. It then proves timer preemption, verifies
that an app setting the hostile NT flag survives both a system call and repeated interrupt returns, and verifies that
a noncanonical app stack becomes a contained user fault rather than a kernel panic. Finally, it launches a fresh
probe to prove the shell still makes progress. Kernel/app sidecar ELFs are not consulted, so a concurrent compiler
cannot mismatch their metadata with the locked image copy. QEMU is stopped and the entire temporary directory is
discarded. The test never interrupts an interactive guest; if the primary image is already locked, it exits with an
explanation.

The default hard timeout is 30 seconds. Slow ARM64 software emulation can use a larger bound:

```sh
make QEMU_SMOKE_TIMEOUT=60 test-qemu-smoke
```

Use `QEMU_SMOKE_ARGS='-accel tcg'` to override the smoke-test accelerator flags. `QEMU`, `QEMU_SMOKE_MEMORY_MIB`, and
`QEMU_SMOKE_LOCK_TIMEOUT` are also configurable.

### Filesystem persistence

The filesystem begins at LBA 2048 (the 1 MiB offset) in `build/NostaluxOS.img` by default; advanced builds can override
it consistently with `FS_STORAGE_LBA=<lba>`. On an ordinary rebuild, the Makefile replaces the bootloader and kernel
while preserving everything from that offset onward. Files created in NostaluxOS therefore survive both guest
restarts and normal `make` rebuilds.

At boot, NostaluxOS formats storage only when both filesystem slots are completely blank. Corrupt or partially
readable storage is never overwritten automatically: the OS mounts a volatile recovery volume and labels it in
Files, Settings, Browser, `sysinfo`, and About. The on-disk self-test verifies write, rename, and removal by reloading
sectors after each operation.

Filesystem v5 stores each logical file as one or more 1,024-byte extent records, for a maximum payload of 8,191 bytes
per file. The two complete on-disk generations are committed table-first and activated with a single checksummed
header sector. Definite capacity or I/O failures roll back the in-memory mutation. Uncertain writes are reconciled by
reading the sectors back; if the outcome cannot be proved, the current bytes remain available on a clearly labeled
volatile volume. Equal integrity-protected generations with different tables, and generation values exactly half the
32-bit serial range apart, are treated as ambiguous and mounted without further disk writes. Version 5 automatically
migrates valid v4 media. The integrity checks detect accidental corruption; they are not authentication and do not
defend against deliberate disk tampering.

Versions 1-3 did not protect the generation field, so two different legacy snapshots cannot be ordered with certainty.
Nostalux therefore mounts the snapshot selected by the legacy generation field as a session-only recovery view and
performs no automatic write. Run `fsupgrade` inside Nostalux to inspect the warning. After reviewing the mounted files,
the exact command `fsupgrade CONFIRM-LEGACY-SNAPSHOT` writes that selected view into the other slot as v5 and reads the
table and commit header back before enabling persistence. This is an explicit choice: files present only in the other
legacy snapshot are not merged. If verification is inconclusive, reboot before taking any further upgrade action.

The disk table holds 32 shared physical extent records. Small files use one record; larger files use one record per
started 1,024 bytes, so the 8,191-byte per-file maximum depends on free shared capacity. System app ELFs are embedded
in the read-only OS image and consume no user-filesystem records. At boot, obsolete filesystem mirrors from earlier
development builds are deleted only when they match the current embedded package or checksummed legacy metadata;
Nostalux attempts to move unknown files using a reserved package name to a collision-free `recovered-*` name and
preserves them in place with a log entry if no safe recovery name can be claimed. `system.log` is a separate virtual,
read-only view of current kernel events and consumes no record. If an older image contains a physical file named
`system.log`, its bytes are retained under a collision-free `recovered-system-log*.txt` name.

The following actions erase the saved filesystem:

- `make clean`
- deleting or replacing `build/NostaluxOS.img`

Discarding a temporary QEMU snapshot loses only changes made inside that snapshot session. It does not erase the
primary image or files that were already saved there.

To intentionally return to a fresh filesystem:

```sh
make clean
make
```

## Run in QEMU

### Windowed interactive mode (default)

```sh
make run
```

`make run-windowed` is an equivalent explicit command.
The launcher holds an exclusive `.nostalux-image.lock` for QEMU's lifetime. A second launch, image rebuild, or
`make clean` waits for that lock instead of modifying the writable raw image underneath a running guest.

On x86-64 WSL, the launcher first looks for `qemu-system-x86_64.exe` on `PATH` and in common Windows QEMU install
directories. If found, it uses the native Windows GTK frontend; otherwise it falls back to Linux
`qemu-system-x86_64`.

The normal GTK window shows the complete 800x600 guest framebuffer at a 1:1 scale. On QEMU, NostaluxOS uses the
VMware-compatible VMMouse protocol for absolute host/guest coordinates, so diagonal movement is delivered as one
coherent position rather than separate horizontal and vertical steps. Other machines fall back to relative PS/2
packets. Keyboard input follows the pointer on hover; press `Ctrl+Alt+G` to release the grab. The host cursor is hidden
over the guest display so the Nostalux cursor replaces it.

### Full-screen interactive mode

```sh
make run-fullscreen
```

Full-screen mode enables GTK's `zoom-to-fit`, so the complete guest framebuffer is scaled down when necessary instead
of being cropped by the host display or Windows DPI scaling. Press `Ctrl+Alt+F` to toggle full-screen mode.

### Headless mode

```sh
make HEADLESS=1 run
```

Headless mode uses the executable selected by `QEMU`, disables the graphical display, multiplexes the serial port and
QEMU monitor on standard I/O, and uses QEMU's `none` audio backend. The NostaluxOS console itself is framebuffer-based,
so this mode is intended primarily for automated boot checks and monitor-driven tests.

For a bounded, assertion-based check that cannot modify the primary image, use `make test-qemu-smoke` instead.

### QEMU overrides

Override either executable when auto-detection does not match your installation:

```sh
make QEMU_NATIVE='/mnt/d/Tools/qemu/qemu-system-x86_64.exe' run
make QEMU=/usr/local/bin/qemu-system-x86_64 run
```

Linux interactive runs use the PulseAudio backend by default. On a system without PulseAudio compatibility, override
the audio flags or select the silent backend:

```sh
make QEMU_AUDIO_LINUX='-machine pcspk-audiodev=snd0 -audiodev none,id=snd0' run
```

## Using NostaluxOS

QEMU displays the boot banner and then the `nostalux>` shell prompt. Type `help` for the current command list and
`gui` to launch the desktop.

Use `apps` to inspect the separate ELF catalog and recent process outcomes. `app hello` exercises the implemented
Apps v1 calls and exits normally. `app fault-probe` intentionally causes a user-mode page fault; the command should
report that the process was isolated and then return to the prompt. `app hang-probe` intentionally never yields;
the shell should return after a short bounded launch period. Run `apps`, note the probe's process ID, and stop it with
`appkill <process-id>`. `app rflags-probe` and `app stack-probe` are destructive-to-their-own-process diagnostics
used by the QEMU integration test; they cannot modify another process or the kernel.

Open **AI Assistant** from the welcome window, desktop icon, Start menu, or Run dialog (`ai` or `assistant`). `Esc`
closes AI Assistant while it has keyboard focus. Choose **Exit Desktop** from Start to return to the shell, or press
`Esc` when no isolated app has keyboard focus.

In Files, **New** creates a text file, **Open** sends text to Notepad and only validated BMP data to Image Viewer, and
**Rename** updates the current filesystem. **Delete** requires a second click within three seconds before removing the
selected file. Closing a modified Notepad atomically saves it; if the commit fails, the editor stays open with its
unsaved text so closing can retry. Closing Paint saves it. Files refuses to rename or delete a file while an isolated
Notepad process is editing it. Reserved app-package filenames cannot be changed through normal shell, Files, or app
service operations. The Files footer reports whether changes are persistent or session-only; `system.log` opens as a
live read-only Browser page.

Browser is a local document viewer rather than a web browser. Try `about:home`, `about:files`, `about:system`,
`file:system.log`, or `file:readme.txt`. Dynamic pages refresh every second and can also be reloaded immediately with
**Refresh**. Internet addresses remain unsupported and are rejected with an explanation.

## Cleaning

```sh
make clean
```

This removes the entire `build/` directory, including the disk image and any files saved in its persistent filesystem.

## Repository layout

- `bootloader/` — 16-bit boot sector plus the protected-mode/long-mode transition stage
- `apps/` — separately compiled x86-64 ELF applications and their linker/blob definitions
- `kernel/` — freestanding 64-bit kernel sources, headers, assembly entry point, and linker script
- `scripts/` — source-tree validation and monitor-driven QEMU smoke-test helpers
- `tests/` — host-side validation tests run by `make test`
- `Makefile` — build, image assembly, persistence, and QEMU launch orchestration
