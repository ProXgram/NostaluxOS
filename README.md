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
  the cooperative scheduler's genuine kernel tasks instead of presenting desktop windows or synthetic work as
  processes.
- **Desktop settings** — theme and wallpaper choices are stored in `desktop.cfg` and restored when storage is
  persistent; a volatile backend clearly reports that changes last only for the current session.
- **Filesystem image viewer** — Files validates image bytes before reporting success, and Image Viewer renders valid
  uncompressed 24-bit BMP files. A small `nostalux.bmp` is installed automatically as a working example; BMPs must fit
  the filesystem's current 1,023-byte per-file limit.
- **AI Assistant** — a basic offline, rule-based intent matcher answers common NostaluxOS questions and can launch
  apps from requests such as `open calculator`. It is intentionally small and deterministic, not a trained model or
  network service.

## Requirements

The supported build environment is an x86-64 or ARM64 Linux system, including WSL, with:

- GNU `make`
- `nasm`
- an x86-64 GNU C toolchain (`gcc` on x86-64, or an x86-64 cross-compiler on ARM64)
- matching GNU binutils (`ld` and `objcopy`)
- a POSIX shell and GNU coreutils, including `cat`, `stat`, `dd`, and `truncate`
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
  binutils-x86-64-linux-gnu qemu-system-x86 qemu-system-gui
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

The suite covers BMP parsing, terminal and shell capture, filesystem corruption/persistence reloads, live log
projection, and normalized E820 memory accounting.

### Filesystem persistence

The filesystem begins at LBA 2048 (the 1 MiB offset) in `build/NostaluxOS.img`. On an ordinary rebuild, the Makefile
replaces the bootloader and kernel while preserving everything from that offset onward. Files created in NostaluxOS
therefore survive both guest restarts and normal `make` rebuilds.

At boot, NostaluxOS formats storage only when both filesystem slots are completely blank. Corrupt or partially
readable storage is never overwritten automatically: the OS mounts a volatile recovery volume and labels it in
Files, Settings, Browser, AI Assistant, `sysinfo`, and About. The on-disk self-test verifies write, rename, and removal
by reloading sectors after each operation.

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

On WSL, the launcher first looks for `qemu-system-x86_64.exe` on `PATH` and in common Windows QEMU install
directories. If found, it uses the native Windows GTK frontend; otherwise it falls back to Linux
`qemu-system-x86_64`.

The normal GTK window shows the complete 800x600 guest framebuffer at a 1:1 scale. Keyboard input follows the pointer
on hover; click the guest once if QEMU has not yet captured the mouse. Press `Ctrl+Alt+G` to release the grab. The host
cursor remains hidden while it is over the guest display.

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
- `kernel/` — freestanding 64-bit kernel sources, headers, assembly entry point, and linker script
- `scripts/` — source-tree validation helpers used by the build
- `tests/` — host-side validation tests run by `make test`
- `Makefile` — build, image assembly, persistence, and QEMU launch orchestration
