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
#include "app_catalog.h"
#include "app_process.h"
#include "app_runtime.h"

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

    // Kernel tasks yield cooperatively; ring-3 apps receive timer quanta.
    scheduler_init();

    /*
     * Mount storage before catalog setup so obsolete filesystem mirrors from
     * earlier development builds can be reclaimed without risking unknown
     * files. Current app packages live in the read-only OS image.
     */
    app_process_table_reset();
    fs_init();
    if (app_catalog_initialize_embedded() == APP_CATALOG_OK) {
        syslog_write("Apps: embedded ELF catalog validated");
        (void)app_catalog_reclaim_legacy_filesystem_images();
    } else {
        syslog_write("Apps: embedded ELF catalog unavailable");
    }

    background_render();
    timer_set_callback(background_animate);
}

void kmain(const struct BootInfo* boot_info) {
    boot_sequence(boot_info);

    /*
     * Keep maskable interrupts disabled through driver initialization.
     * Individual drivers unmask their PIC lines only after their state is
     * ready; make those initialized devices globally interruptible now.
     */
    __asm__ volatile("sti" ::: "memory");

    enum app_runtime_launch_result app_result =
        app_runtime_run_catalog_id("hello");
    if (app_result == APP_RUNTIME_LAUNCH_OK) {
        syslog_write("Apps: ring-3 hello exited normally");
    } else {
        syslog_write(app_runtime_launch_result_text(app_result));
    }

    // NOTE: We do NOT spawn the GUI task automatically anymore.
    // This prevents the GUI from stealing keyboard input from the shell.
    // The user can type 'gui' in the shell to launch the desktop.

    shell_run();
}
