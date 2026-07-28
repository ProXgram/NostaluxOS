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
- **Graphical desktop** — `gui` opens movable and resizable windows, a Start menu, taskbar, Files, Paint, Notepad,
  Calculator, settings, monitors, games, and other retro desktop apps.
- **Writable desktop apps** — Files creates, opens, renames, and deletes files. Notepad edits and saves text files,
  while Paint saves its 17x17 canvas as a real 24-bit BMP that Image Viewer can reopen. Each app reports whether the
  current storage is persistent or session-only.
- **Shell-backed GUI Terminal** — desktop terminal commands go through the same dispatcher and handlers as the boot
  shell. Commands that need exclusive console control or would stop the desktop are clearly reported as unavailable.
- **Local Browser** — Browser opens real text files with `file:<name>` and automatically refreshes `about:files`,
  `about:system`, and the live read-only `system.log`. It explicitly rejects Internet URLs because there is no network
  driver or TCP/IP stack.
- **Kernel-backed diagnostics** — System Monitor graphs measured CPU busy-versus-idle time and distinguishes complete
  E820 usable RAM, currently mapped RAM, fixed reservations, and committed heap memory. Task Manager enumerates only
  genuine scheduler tasks and isolated app processes instead of presenting desktop windows or synthetic work as
  processes.
- **Desktop settings** — theme and wallpaper choices are stored in `desktop.cfg` and restored when storage is
  persistent; a volatile backend clearly reports that changes last only for the current session.
- **Filesystem image viewer** — Files validates image bytes before reporting success, and Image Viewer renders valid
  uncompressed 24-bit BMP files. A small `nostalux.bmp` is installed automatically as a working example; BMPs must fit
  the filesystem's current 1,023-byte per-file limit.
- **AI Assistant** — a basic offline, rule-based intent matcher answers common NostaluxOS questions and can launch
  apps from requests such as `open calculator`. It is intentionally small and deterministic, not a trained model or
  network service.
- **Isolated Apps v1 foundation** — a strict x86-64 ELF loader validates fixed-address executables, rejects writable
  code, and maps each process into its own ring-3 address space with a guarded NX stack. The checked `INT 0x80` ABI
  currently implements ABI discovery, exit, cooperative yield, bounded log writes, and wall/monotonic time. App
  crashes are recorded and terminate only that process.
- **Real app launcher and isolation probes** — `apps` lists validated manifests and bounded process history;
  `app hello` runs the embedded sample, while `app fault-probe` deliberately touches supervisor-only memory to prove
  that the page fault is contained and the shell keeps running. `app hang-probe` sets the direction flag and spins
  without yielding; timer preemption returns control to the shell, where `appkill <process-id>` stops it. The hello
  sample also runs once during boot.

This is the secure Apps v1 execution foundation, not the completed desktop-app migration. File, window, input, and
dynamic-memory syscall numbers are reserved but deliberately return `unsupported`; Calculator, Notepad, Image Viewer,
and AI Assistant still run as kernel desktop code. The current 1,023-byte filesystem payload limit is also too small
for these ELF samples, so the three validated test applications are embedded in the kernel image for now. Kernel
tasks still yield cooperatively, while ring-3 apps are preempted every five 100 Hz PIT ticks (about 50 ms). Apps v1
deliberately traps x87/MMX/SIMD use until per-process extended-register save and restore support is implemented.

## Requirements

The supported build environment is an x86-64 or ARM64 Linux system, including WSL, with:

- GNU `make`
- `nasm`
- an x86-64 GNU C toolchain (`gcc` on x86-64, or an x86-64 cross-compiler on ARM64)
- matching GNU binutils (`ld` and `objcopy`)
- a POSIX shell and GNU coreutils, including `cat`, `stat`, `dd`, and `truncate`
- util-linux `flock`, used to prevent QEMU, rebuilds, and cleaning from accessing the writable image concurrently
- QEMU (`qemu-system-x86_64`) if you want to run the image

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

The suite covers strict ELF and manifest validation, bounded process-history reuse, the app-launcher dispatcher, BMP
parsing, terminal and shell capture, filesystem corruption/persistence reloads, VMMouse coordinate decoding, live log
projection, and normalized 64-bit E820 memory accounting.

### Filesystem persistence

The filesystem begins at LBA 2048 (the 1 MiB offset) in `build/NostaluxOS.img` by default; advanced builds can override
it consistently with `FS_STORAGE_LBA=<lba>`. On an ordinary rebuild, the Makefile replaces the bootloader and kernel
while preserving everything from that offset onward. Files created in NostaluxOS therefore survive both guest
restarts and normal `make` rebuilds.

At boot, NostaluxOS formats storage only when both filesystem slots are completely blank. Corrupt or partially
readable storage is never overwritten automatically: the OS mounts a volatile recovery volume and labels it in
Files, Settings, Browser, AI Assistant, `sysinfo`, and About. The on-disk self-test verifies write, rename, and removal
by reloading sectors after each operation.

Filesystem v4 protects the commit generation inside the header checksum. This detects accidental metadata corruption;
it is not a cryptographic signature or a defense against deliberate disk tampering. Older v1-v3 images did not cover
the generation field, so two different legacy snapshots cannot be ordered with certainty. Nostalux therefore mounts
the snapshot selected by the legacy generation field as a session-only recovery view and performs no automatic write.
Run `fsupgrade` inside Nostalux to inspect the warning. After reviewing the mounted files, the exact command
`fsupgrade CONFIRM-LEGACY-SNAPSHOT` writes that selected view into the other slot as v4 and reads the table and commit
header back before enabling persistence. This is an explicit choice: files present only in the other legacy snapshot
are not merged. If verification is inconclusive, reboot before taking any further upgrade action.

The disk table holds 32 persistent records. `system.log` is a separate virtual, read-only view of current kernel
events, so it remains available without consuming a record even when the table is full. If an older image contains a
physical file named `system.log`, its bytes are retained under a collision-free `recovered-system-log*.txt` name.

The following actions erase the saved filesystem:

- `make clean`
- deleting or replacing `build/NostaluxOS.img`
- running QEMU with a temporary snapshot and then discarding that snapshot

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

On WSL, the launcher first looks for `qemu-system-x86_64.exe` on `PATH` and in common Windows QEMU install
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
`appkill <process-id>`.

Open **AI Assistant** from the welcome window, desktop icon, Start menu, or Run dialog (`ai` or `assistant`). Press
`Esc` or choose **Exit Desktop** from Start to return to the shell.

In Files, **New** creates a text file, **Open** sends text to Notepad and only validated BMP data to Image Viewer, and
**Rename** updates the current filesystem. **Delete** requires a second click within three seconds before removing the
selected file. Closing a modified Notepad or Paint window saves it. The Files footer reports whether those changes are
persistent or session-only; `system.log` opens as a live read-only Browser page.

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
- `scripts/` — source-tree validation helpers used by the build
- `tests/` — host-side validation tests run by `make test`
- `Makefile` — build, image assembly, persistence, and QEMU launch orchestration
