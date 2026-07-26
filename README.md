# NostaluxOS

NostaluxOS is a small x86-64 hobby operating system built as a learning project. It boots through a handcrafted
two-stage BIOS loader, enters long mode, and runs a freestanding kernel with a graphical shell and desktop.

## Current features

- **Handmade boot flow** — an MBR boot sector and second-stage loader load the kernel, configure paging, and enter
  64-bit long mode.
- **VBE framebuffer console** — the loader selects an 800x600, 32-bit VBE mode. The text console and desktop are
  rendered into that linear framebuffer; this is not a VGA text-mode interface.
- **Interactive shell** — `help` lists commands for system information, colors, history, arithmetic, files, memory
  diagnostics, games, sound, reboot, shutdown, and launching the GUI.
- **Persistent flat filesystem** — `ls`, `cat`, `hexdump`, `touch`, `write`, `append`, and `rm` operate on a small
  ATA-backed filesystem stored inside the raw disk image.
- **Graphical desktop** — `gui` opens movable and resizable windows, a Start menu, taskbar, Files, Paint, Notepad,
  Calculator, settings, monitors, games, and other retro desktop apps.
- **Kernel-backed diagnostics** — System Monitor graphs measured CPU busy time and tracked memory use, while Task
  Manager enumerates the cooperative scheduler's real kernel tasks rather than treating windows as processes.
- **Filesystem image viewer** — Image Viewer validates and renders uncompressed 24-bit BMP files stored in the
  ATA-backed filesystem. A small `nostalux.bmp` is installed automatically as a working example; BMPs must fit the
  filesystem's current 1,023-byte per-file limit.
- **AI Assistant** — an offline rule-based helper answers common NostaluxOS questions and can launch apps from
  requests such as `open calculator`.

The Browser app is currently a visual demo; NostaluxOS does not yet include a network stack.

## Requirements

The supported build environment is an x86-64 Linux system or WSL with:

- GNU `make`
- `nasm`
- an x86-64 GNU C toolchain (`gcc`)
- GNU binutils (`ld` and `objcopy`)
- a POSIX shell and GNU coreutils, including `cat`, `stat`, `dd`, and `truncate`
- QEMU (`qemu-system-x86_64`) if you want to run the image

The kernel requires at least **9 MiB of guest RAM** and works better with **32 MiB or more**. QEMU's normal default
memory allocation is sufficient. The virtual machine also needs a BIOS that provides an E820 physical-memory map and
a VBE implementation that exposes the requested 800x600x32 mode.

## Build

```sh
make
```

This creates `build/NostaluxOS.img`, a raw disk image containing the bootloader, kernel, and writable filesystem area.
The conflict check reads the source tree directly, so the build works from a ZIP extraction or Downloads folder even
when no `.git` directory is present.

Run the host-side BMP validation tests with:

```sh
make test
```

### Filesystem persistence

The filesystem begins at LBA 2048 (the 1 MiB offset) in `build/NostaluxOS.img`. On an ordinary rebuild, the Makefile
replaces the bootloader and kernel while preserving everything from that offset onward. Files created in NostaluxOS
therefore survive both guest restarts and normal `make` rebuilds.

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

Open **AI Assistant** from the welcome window, desktop icon, Start menu, GUI Terminal (`ai`), or Run dialog (`ai` or
`assistant`). Press `Esc` or choose **Exit Desktop** from Start to return to the shell.

## Cleaning

```sh
make clean
```

This removes the entire `build/` directory, including the disk image and any files saved in its persistent filesystem.

## Repository layout

- `bootloader/` — 16-bit boot sector plus the protected-mode/long-mode transition stage
- `kernel/` — freestanding 64-bit kernel sources, headers, assembly entry point, and linker script
- `scripts/` — source-tree validation helpers used by the build
- `tests/` — host-side parser tests run by `make test`
- `Makefile` — build, image assembly, persistence, and QEMU launch orchestration
