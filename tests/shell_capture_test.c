#include "ata.h"
#include "app_catalog.h"
#include "app_process.h"
#include "app_runtime.h"
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
static fs_legacy_upgrade_status_t test_upgrade_status =
    FS_LEGACY_UPGRADE_UNAVAILABLE;
static size_t test_upgrade_calls;
static uint64_t terminated_app_process_id;
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
static const struct app_catalog_entry test_apps[] = {
    {
        .manifest = {
            .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
            .abi_version = NOSTALUX_APP_ABI_VERSION,
            .capabilities = APP_CAPABILITY_LOG | APP_CAPABILITY_TIME,
            .id = "hello",
            .display_name = "Hello Nostalux",
            .executable = "hello.elf",
            .description = "Separate ELF app",
        },
    },
    {
        .manifest = {
            .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
            .abi_version = NOSTALUX_APP_ABI_VERSION,
            .capabilities = 0,
            .id = "fault-probe",
            .display_name = "Fault Isolation Probe",
            .executable = "fault-probe.elf",
            .description = "Supervisor-memory isolation probe",
        },
    },
    {
        .manifest = {
            .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
            .abi_version = NOSTALUX_APP_ABI_VERSION,
            .capabilities = 0,
            .id = "hang-probe",
            .display_name = "Preemption Hang Probe",
            .executable = "hang-probe.elf",
            .description = "Non-yielding preemption probe",
        },
    },
};
static const struct app_process_info test_process = {
    .process_id = 7,
    .app_id = "hello",
    .display_name = "Hello Nostalux",
    .state = APP_PROCESS_EXITED,
    .exit_code = 0,
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
ata_init_result_t ata_init(void) { return ATA_INIT_READY; }

void scheduler_set_current_name(const char* name) { (void)name; }
void schedule(void) {}
bool scheduler_terminate_app_process(uint64_t app_process_id) {
    terminated_app_process_id = app_process_id;
    return app_process_id == 42;
}
void timer_set_callback(timer_callback_t callback) { (void)callback; }
void timer_wait(int ticks) { (void)ticks; }
uint64_t timer_get_ticks(void) { return 1234; }
uint64_t timer_get_uptime(void) { return 12; }

size_t app_catalog_count(void) {
    return sizeof(test_apps) / sizeof(test_apps[0]);
}
const struct app_catalog_entry* app_catalog_at(size_t index) {
    return index < app_catalog_count() ? &test_apps[index] : NULL;
}
size_t app_process_count(void) { return 1; }
bool app_process_snapshot(size_t index, struct app_process_info* out_info) {
    if (index != 0 || out_info == NULL) return false;
    *out_info = test_process;
    return true;
}
enum app_runtime_launch_result app_runtime_run_catalog_id(
    const char* app_id) {
    if (strcmp(app_id, "hello") == 0) {
        return APP_RUNTIME_LAUNCH_OK;
    }
    if (strcmp(app_id, "fault-probe") == 0) {
        return APP_RUNTIME_LAUNCH_APP_FAULTED;
    }
    if (strcmp(app_id, "hang-probe") == 0) {
        return APP_RUNTIME_LAUNCH_DISPATCH_BUDGET;
    }
    return APP_RUNTIME_LAUNCH_NOT_FOUND;
}
const char* app_runtime_launch_result_text(
    enum app_runtime_launch_result result) {
    return result == APP_RUNTIME_LAUNCH_NOT_FOUND
        ? "application not found"
        : "test launch result";
}

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
fs_legacy_upgrade_status_t fs_legacy_upgrade_status(void) {
    return test_upgrade_status;
}
const char* fs_legacy_upgrade_status_text(void) {
    if (test_upgrade_status == FS_LEGACY_UPGRADE_AVAILABLE) {
        return "Legacy snapshots differ; explicit recovery upgrade available";
    }
    if (test_upgrade_status == FS_LEGACY_UPGRADE_OUTCOME_UNCERTAIN) {
        return "Legacy upgrade outcome is unverified; reboot before retrying";
    }
    return "No legacy recovery upgrade is available";
}
fs_legacy_upgrade_result_t fs_upgrade_legacy_snapshot(
    const char* confirmation) {
    test_upgrade_calls++;
    if (test_upgrade_status != FS_LEGACY_UPGRADE_AVAILABLE) {
        return FS_LEGACY_UPGRADE_NOT_AVAILABLE;
    }
    if (confirmation == NULL ||
        strcmp(confirmation, FS_LEGACY_UPGRADE_CONFIRMATION) != 0) {
        return FS_LEGACY_UPGRADE_CONFIRMATION_REQUIRED;
    }
    test_upgrade_status = FS_LEGACY_UPGRADE_UNAVAILABLE;
    return FS_LEGACY_UPGRADE_SUCCEEDED;
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
    assert(strstr(output, "fsupgrade") != NULL);
    assert(strstr(output, "List separate ELF applications") != NULL);
    assert(strstr(output, "Run an ELF app") != NULL);
    assert(strstr(output, "Stop a running ELF app") != NULL);
}

static void test_app_launcher_commands(void) {
    char output[2048];

    execute("apps", output, sizeof(output), NULL, NULL);
    assert(strstr(output, "ELF applications (3)") != NULL);
    assert(strstr(output, "hello - Hello Nostalux") != NULL);
    assert(strstr(output, "fault-probe - Fault Isolation Probe") != NULL);
    assert(strstr(output, "hang-probe - Preemption Hang Probe") != NULL);
    assert(strstr(output, "#7 hello: exited (code 0)") != NULL);

    execute("app hello", output, sizeof(output), NULL, NULL);
    assert(strcmp(output, "App 'hello' exited normally.\n") == 0);

    execute("app fault-probe", output, sizeof(output), NULL, NULL);
    assert(strstr(output, "faulted in ring 3 and was isolated") != NULL);
    assert(strstr(output, "OS is still running") != NULL);

    execute("app hang-probe", output, sizeof(output), NULL, NULL);
    assert(strstr(output, "still running in the background") != NULL);
    assert(strstr(output, "appkill <process-id>") != NULL);

    terminated_app_process_id = 0;
    execute("appkill 42", output, sizeof(output), NULL, NULL);
    assert(strcmp(output, "Stopped app process #42.\n") == 0);
    assert(terminated_app_process_id == 42);

    execute("appkill 7", output, sizeof(output), NULL, NULL);
    assert(strcmp(output, "No running app process #7.\n") == 0);
    assert(terminated_app_process_id == 7);

    execute("appkill 0", output, sizeof(output), NULL, NULL);
    assert(strcmp(output, "Usage: appkill <process-id>\n") == 0);

    execute("appkill 18446744073709551615",
            output, sizeof(output), NULL, NULL);
    assert(strcmp(
               output,
               "No running app process #18446744073709551615.\n") == 0);
    assert(terminated_app_process_id == UINT64_MAX);

    execute("appkill 18446744073709551616",
            output, sizeof(output), NULL, NULL);
    assert(strcmp(output, "Usage: appkill <process-id>\n") == 0);

    execute("app missing", output, sizeof(output), NULL, NULL);
    assert(strstr(output, "application not found") != NULL);

    execute("app", output, sizeof(output), NULL, NULL);
    assert(strcmp(output, "Usage: app <id>\n") == 0);
}

static void test_legacy_upgrade_confirmation(void) {
    char output[1024];
    test_upgrade_status = FS_LEGACY_UPGRADE_AVAILABLE;
    test_upgrade_calls = 0;

    execute("fsupgrade", output, sizeof(output), NULL, NULL);
    assert(strstr(
               output,
               "generation order is not integrity-protected") != NULL);
    assert(strstr(output,
                  "fsupgrade " FS_LEGACY_UPGRADE_CONFIRMATION) != NULL);
    assert(strstr(output, "no disk changes") != NULL);
    assert(test_upgrade_calls == 0u);

    execute("fsupgrade yes", output, sizeof(output), NULL, NULL);
    assert(strstr(output, "Confirmation did not match") != NULL);
    assert(strstr(output, "No disk changes were requested") != NULL);
    assert(test_upgrade_calls == 1u);
    assert(test_upgrade_status == FS_LEGACY_UPGRADE_AVAILABLE);

    execute("fsupgrade " FS_LEGACY_UPGRADE_CONFIRMATION,
            output, sizeof(output), NULL, NULL);
    assert(strstr(output, "committed and read back") != NULL);
    assert(test_upgrade_calls == 2u);
    assert(test_upgrade_status == FS_LEGACY_UPGRADE_UNAVAILABLE);
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
    test_app_launcher_commands();
    test_legacy_upgrade_confirmation();
    test_touch_reports_existing_file();
    test_bounds_and_aliasing();
    puts("shell capture tests passed");
    return 0;
}
