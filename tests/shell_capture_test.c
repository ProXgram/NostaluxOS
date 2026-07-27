#include "fs.h"
#include "graphics.h"
#include "keyboard.h"
#include "rtc.h"
#include "shell.h"
#include "syslog.h"
#include "system.h"
#include "terminal.h"
#include "timer.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char last_history[128];
static size_t history_count;
static bool test_file_exists;
static struct fs_file test_file = {
    .in_use = true,
    .name = "test.txt",
};
static const struct BootInfo boot_info = {
    .width = 640,
    .height = 480,
};
static const struct system_profile profile = {
    .architecture = "test",
    .memory_total_kb = 131072,
    .memory_mapped_kb = 65536,
    .memory_managed_kb = 16384,
    .memory_reserved_kb = 8192,
    .memory_heap_committed_kb = 1024,
    .memory_used_kb = 9216,
};

void graphics_init(void) {}
uint32_t graphics_get_width(void) { return 640; }
uint32_t graphics_get_height(void) { return 480; }
void graphics_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    (void)x; (void)y; (void)c; (void)fg; (void)bg;
}
void graphics_fill_rect(int x, int y, int w, int h, uint32_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
}

void background_render(void) {}
void background_animate(void) {}
void banner_run(void) {}
void gui_demo_run(void) {}
void snake_game_run(void) {}
void memtest_run_diagnostic(void) {}
void sound_init(void) {}
void sound_beep(uint32_t frequency, int duration_ticks) {
    (void)frequency; (void)duration_ticks;
}
bool ata_init(void) { return true; }

void scheduler_set_current_name(const char* name) { (void)name; }
void timer_set_callback(timer_callback_t callback) { (void)callback; }
void timer_wait(int ticks) { (void)ticks; }
uint64_t timer_get_ticks(void) { return 1234; }
uint64_t timer_get_uptime(void) { return 12; }

void keyboard_read_line_ex(char* buffer, size_t size,
                           keyboard_idle_callback_t on_idle) {
    (void)buffer; (void)size; (void)on_idle;
}
void keyboard_history_record(const char* line) {
    size_t length = strlen(line);
    if (length >= sizeof(last_history)) length = sizeof(last_history) - 1;
    memcpy(last_history, line, length);
    last_history[length] = '\0';
    history_count++;
}
size_t keyboard_history_length(void) { return history_count; }
const char* keyboard_history_entry(size_t index) {
    return index < history_count ? last_history : "";
}

bool rtc_format_time(char* buffer, uint32_t capacity) {
    if (capacity < 9) return false;
    memcpy(buffer, "12:34:56", 9);
    return true;
}

const struct BootInfo* system_boot_info(void) { return &boot_info; }
const struct system_profile* system_profile_info(void) { return &profile; }

size_t syslog_length(void) { return 1; }
const char* syslog_entry(size_t index) {
    return index == 0 ? "test log" : "";
}

size_t fs_file_count(void) { return 0; }
const struct fs_file* fs_file_at(size_t index) { (void)index; return NULL; }
const struct fs_file* fs_find(const char* name) {
    return test_file_exists && strcmp(name, test_file.name) == 0
         ? &test_file : NULL;
}
bool fs_touch(const char* name) {
    if (strcmp(name, test_file.name) != 0) return false;
    test_file_exists = true;
    return true;
}
bool fs_write(const char* name, const char* contents) {
    (void)name; (void)contents; return false;
}
bool fs_append(const char* name, const char* contents) {
    (void)name; (void)contents; return false;
}
bool fs_remove(const char* name) { (void)name; return false; }
fs_backend_status_t fs_backend_status(void) {
    return FS_BACKEND_PERSISTENT;
}
bool fs_backend_is_persistent(void) { return true; }
const char* fs_backend_status_text(void) {
    return "Persistent ATA storage";
}

static void execute(const char* command, char* output, size_t capacity,
                    size_t* length, bool* truncated) {
    assert(shell_execute_capture(command, output, capacity, length, truncated));
}

static void test_real_dispatch(void) {
    char output[1024];
    size_t length = 0;
    bool truncated = true;

    execute("echo hello desktop", output, sizeof(output), &length, &truncated);
    assert(strcmp(output, "hello desktop\n") == 0);
    assert(length == strlen(output));
    assert(!truncated);
    assert(strcmp(last_history, "echo hello desktop") == 0);

    execute("calc 10 * 5", output, sizeof(output), &length, &truncated);
    assert(strcmp(output, "Result: 50\n") == 0);

    execute("time", output, sizeof(output), &length, &truncated);
    assert(strcmp(output, "RTC Time: 12:34:56\n") == 0);

    execute("sysinfo", output, sizeof(output), &length, &truncated);
    assert(strstr(output, "Usable physical RAM: 131072 KB") != NULL);
    assert(strstr(output, "Mapped usable RAM: 65536 KB") != NULL);
    assert(strstr(output,
                  "Kernel committed RAM: 9216 KB (8192 reserved + 1024 heap)")
           != NULL);
    assert(strstr(output, "Storage: Persistent ATA storage") != NULL);

    execute("does-not-exist", output, sizeof(output), &length, &truncated);
    assert(strcmp(output, "Unknown command.\n") == 0);
}

static void test_console_only_guard(void) {
    static const char* commands[] = {
        "clear", "banner", "gui", "sleep 1", "foreground Red",
        "background Blue", "snake", "beep", "reboot", "shutdown",
    };
    char output[256];

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        execute(commands[i], output, sizeof(output), NULL, NULL);
        assert(strstr(output, "unavailable in the desktop terminal") != NULL);
    }
}

static void test_palette_and_help(void) {
    char output[4096];

    execute("palette", output, sizeof(output), NULL, NULL);
    assert(strstr(output, "[ 0] Black") != NULL);
    assert(strstr(output, "[15] White") != NULL);

    execute("help", output, sizeof(output), NULL, NULL);
    assert(strstr(output, "palette") != NULL);
    assert(strstr(output, "[console only]") != NULL);
    assert(strstr(output, "Show current RTC time") != NULL);
    assert(strstr(output, "Show current RTC date/time") == NULL);
    assert(strstr(output, "List filesystem files and sizes") != NULL);
    assert(strstr(output, "List files and usage stats") == NULL);
}

static void test_touch_reports_existing_file(void) {
    char output[128];

    test_file_exists = false;
    execute("touch test.txt", output, sizeof(output), NULL, NULL);
    assert(strcmp(output, "Created test.txt\n") == 0);

    execute("touch test.txt", output, sizeof(output), NULL, NULL);
    assert(strcmp(output, "Already exists: test.txt\n") == 0);
}

static void test_bounds_and_aliasing(void) {
    char output[64] = "calc 4 + 7";
    size_t length = 0;
    bool truncated = false;

    execute(output, output, sizeof(output), &length, &truncated);
    assert(strcmp(output, "Result: 11\n") == 0);
    assert(!truncated);

    execute("echo this will be truncated", output, 8, &length, &truncated);
    assert(length == 7);
    assert(truncated);
    assert(output[7] == '\0');

    char long_command[160];
    memset(long_command, 'x', sizeof(long_command) - 1);
    long_command[sizeof(long_command) - 1] = '\0';
    execute(long_command, output, sizeof(output), &length, &truncated);
    assert(strstr(output, "Command line is too long") != NULL);
}

int main(void) {
    terminal_initialize(640, 480);
    test_real_dispatch();
    test_console_only_guard();
    test_palette_and_help();
    test_touch_reports_existing_file();
    test_bounds_and_aliasing();
    puts("shell capture tests passed");
    return 0;
}
