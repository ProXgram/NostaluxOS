#include <stddef.h>
#include <stdint.h>

#include "background.h"
#include "fs.h"
#include "memtest.h"
#include "shell.h"
#include "system.h"
#include "syslog.h"
#include "terminal.h"
#include "keyboard.h"
#include "mouse.h" // Added include for mouse
#include "timer.h" 
#include "banner.h"
#include "heap.h"
#include "scheduler.h"
#include "gui_demo.h"
#include "kstdio.h"

#define HEAP_START_ADDR  SYSTEM_RESERVED_LOW_MEMORY_BYTES
#define HEAP_TARGET_SIZE (16ull * 1024ull * 1024ull)

static void halt_boot(const char* message) {
    terminal_writestring(message);
    terminal_newline();
    syslog_write(message);
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static void boot_sequence(const struct BootInfo* boot_info) {
    system_cache_boot_info(boot_info);
    const struct BootInfo* cached = system_boot_info();

    terminal_initialize(cached->width, cached->height);

    /*
     * Normalize the complete E820 map for reporting, but place the heap only
     * in the contiguous mapped range that contains its fixed 8 MiB base.
     */
    struct memtest_memory_info memory_info;
    if (!memtest_detect_memory(&memory_info)) {
        halt_boot("Fatal: a valid BIOS E820 memory map is required.");
    }

    size_t memory_bytes = memory_info.heap_upper_limit;
    if (memory_bytes <= HEAP_START_ADDR + 65536u) {
        halt_boot("Fatal: at least 9 MB of RAM is required.");
    }

    size_t heap_size = memory_bytes - (size_t)HEAP_START_ADDR;
    if (heap_size > HEAP_TARGET_SIZE) heap_size = HEAP_TARGET_SIZE;
    heap_init((void*)(uintptr_t)HEAP_START_ADDR, heap_size);
    if (heap_free_space() == 0) {
        halt_boot("Fatal: unable to initialize the kernel heap.");
    }
    system_configure_memory(memory_info.physical_usable_bytes,
                            memory_info.mapped_usable_bytes,
                            memory_info.reserved_low_usable_bytes,
                            heap_size);

    // Initialize timer and input after allocator state is valid.
    timer_init();
    keyboard_init();
    mouse_init();

    // Initialize the currently cooperative task scheduler.
    scheduler_init();

    background_render();
    timer_set_callback(background_animate);
    
    fs_init();
}

void kmain(const struct BootInfo* boot_info) {
    boot_sequence(boot_info);

    // NOTE: We do NOT spawn the GUI task automatically anymore.
    // This prevents the GUI from stealing keyboard input from the shell.
    // The user can type 'gui' in the shell to launch the desktop.

    shell_run();
}
