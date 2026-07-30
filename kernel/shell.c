#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "background.h"
#include "fs.h"
#ifndef NOSTALUX_HOST_TEST
#include "io.h"
#endif
#include "keyboard.h"
#include "kstring.h"
#include "memtest.h"
#include "os_info.h"
#include "shell.h"
#include "system.h"
#include "syslog.h"
#include "terminal.h"
#include "timer.h"
#include "snake.h"
#include "sound.h"
#include "kstdio.h" 
#include "ata.h"    
#include "banner.h"
#include "gui_demo.h" // Includes the GUI entry point
#include "heap.h"
#include "rtc.h"
#include "scheduler.h"
#include "app_catalog.h"
#include "app_process.h"
#include "app_runtime.h"
#include "network.h"

struct shell_command {
    const char* name;
    void (*handler)(const char* args);
    const char* description;
    unsigned int flags;
};

enum {
    SHELL_COMMAND_CONSOLE_ONLY = 1u << 0,
};

static void shell_print_banner(void);
static void shell_print_prompt(void);
static void command_help(const char* args);
static void command_about(const char* args);
static void command_clear(const char* args);
static void command_foreground(const char* args);
static void command_background(const char* args);
static void command_echo(const char* args);
static void command_ls(const char* args);
static void command_cat(const char* args);
static void command_hexdump(const char* args);
static void command_touch(const char* args);
static void command_write(const char* args);
static void command_append(const char* args);
static void command_rm(const char* args);
static void command_sysinfo(const char* args);
static void command_logs(const char* args);
static void command_memtest(const char* args);
static void command_reboot(const char* args);
static void command_shutdown(const char* args);
static void command_time(const char* args);
static void command_calc(const char* args);
static void command_history(const char* args);
static void command_uptime(const char* args);
static void command_sleep(const char* args);
static void command_snake(const char* args);
static void command_beep(const char* args);
static void command_disktest(const char* args);
static void command_fsupgrade(const char* args);
static void command_apps(const char* args);
static void command_app(const char* args);
static void command_appkill(const char* args);
static void command_banner(const char* args);
static void command_gui(const char* args);
static void command_palette(const char* args);
static void command_net(const char* args);
static void command_dhcp(const char* args);
static void command_dns(const char* args);
static void command_ping(const char* args);
static void command_http(const char* args);

static const struct shell_command COMMANDS[] = {
    {"help", command_help, "Show this help message", 0},
    {"about", command_about, "Learn more about " OS_NAME, 0},
    {"clear", command_clear, "Clear the screen", SHELL_COMMAND_CONSOLE_ONLY},
    {"banner", command_banner, "Show moving banner screensaver", SHELL_COMMAND_CONSOLE_ONLY},
    {"gui", command_gui, "Launch graphical desktop", SHELL_COMMAND_CONSOLE_ONLY},
    {"apps", command_apps, "List separate ELF applications and process history", 0},
    {"app", command_app, "Run an ELF app with an optional startup argument", 0},
    {"appkill", command_appkill, "Stop a running ELF app by process ID", 0},
    {"time", command_time, "Show current RTC time", 0},
    {"uptime", command_uptime, "Show time since boot", 0},
    {"sleep", command_sleep, "Pause for N seconds", SHELL_COMMAND_CONSOLE_ONLY},
    {"calc", command_calc, "Simple math (e.g. 'calc 10 + 5')", 0},
    {"foreground", command_foreground, "Set text color", SHELL_COMMAND_CONSOLE_ONLY},
    {"background", command_background, "Set background color", SHELL_COMMAND_CONSOLE_ONLY},
    {"palette", command_palette, "Preview IBM PC color swatches", 0},
    {"net", command_net, "Show real network link and IP status", 0},
    {"dhcp", command_dhcp, "Request a fresh DHCP configuration", 0},
    {"dns", command_dns, "Resolve a hostname (e.g. 'dns example.com')", 0},
    {"ping", command_ping, "Send an ICMP echo request", 0},
    {"http", command_http, "Fetch a cleartext HTTP URL", 0},
    {"ls", command_ls, "List filesystem files and sizes", 0},
    {"cat", command_cat, "Print a file's text content", 0},
    {"hexdump", command_hexdump, "View file content in hex", 0},
    {"touch", command_touch, "Create an empty file", 0},
    {"write", command_write, "Overwrite a file with new text", 0},
    {"append", command_append, "Append text to a file", 0},
    {"rm", command_rm, "Remove a file", 0},
    {"history", command_history, "Show recent commands", 0},
    {"sysinfo", command_sysinfo, "Display hardware info", 0},
    {"memtest", command_memtest, "Test a reserved 64 KB RAM sample", 0},
    {"logs", command_logs, "Show system logs", 0},
    {"echo", command_echo, "Display text back to you", 0},
    {"snake", command_snake, "Play the Snake game", SHELL_COMMAND_CONSOLE_ONLY},
    {"beep", command_beep, "Test PC Speaker", SHELL_COMMAND_CONSOLE_ONLY},
    {"disktest", command_disktest, "Detect the primary ATA drive", 0},
    {"fsupgrade", command_fsupgrade, "Inspect or confirm a legacy filesystem upgrade", 0},
    {"reboot", command_reboot, "Restart the system", SHELL_COMMAND_CONSOLE_ONLY},
    {"shutdown", command_shutdown, "Power off the system", SHELL_COMMAND_CONSOLE_ONLY},
};
#define COMMAND_COUNT (sizeof(COMMANDS) / sizeof(COMMANDS[0]))
#define INPUT_CAPACITY 128

static const char* COLOR_NAMES[16] = {
    "Black", "Blue", "Green", "Cyan", "Red", "Magenta", "Brown", "Light Grey",
    "Dark Grey", "Light Blue", "Light Green", "Light Cyan", "Light Red",
    "Light Magenta", "Yellow", "White",
};

static void command_time(const char* args) {
    (void)args;
    char time[9];
    if (rtc_format_time(time, sizeof(time))) kprintf("RTC Time: %s\n", time);
    else kprintf("RTC Time: unavailable\n");
}

static void command_uptime(const char* args) {
    (void)args;
    uint64_t seconds = timer_get_uptime();
    kprintf("System Uptime: %llu seconds (%llu ticks)\n",
            (unsigned long long)seconds,
            (unsigned long long)timer_get_ticks());
}

static void command_sleep(const char* args) {
    const char* ptr = kskip_spaces(args);
    unsigned int sec = 0;
    if (!kparse_uint(&ptr, &sec) || *kskip_spaces(ptr) != '\0' || sec > 3600u) {
        kprintf("Usage: sleep <seconds>\n");
        return;
    }
    kprintf("Sleeping for %u seconds...\n", sec);
    timer_wait((int)(sec * 100u));
    kprintf("Done.\n");
}

static void command_calc(const char* args) {
    const char* cursor = kskip_spaces(args);
    unsigned int a = 0, b = 0;
    if (!kparse_uint(&cursor, &a)) { kprintf("Usage: calc <num> <op> <num>\n"); return; }
    cursor = kskip_spaces(cursor);
    if (*cursor == '\0') { kprintf("Usage: calc <num> <op> <num>\n"); return; }
    char op = *cursor++;
    cursor = kskip_spaces(cursor);
    if (!kparse_uint(&cursor, &b)) { kprintf("Usage: calc <num> <op> <num>\n"); return; }
    if (*kskip_spaces(cursor) != '\0') { kprintf("Usage: calc <num> <op> <num>\n"); return; }

    int64_t result = 0;
    if (op == '+') result = (int64_t)a + (int64_t)b;
    else if (op == '-') result = (int64_t)a - (int64_t)b;
    else if (op == '*') {
        if (a != 0 && (uint64_t)b > (uint64_t)INT64_MAX / (uint64_t)a) {
            kprintf("Error: Result is too large\n");
            return;
        }
        result = (int64_t)((uint64_t)a * (uint64_t)b);
    }
    else if (op == '/') { if (b == 0) { kprintf("Error: Div by zero\n"); return; } result = (int64_t)a / b; }
    else { kprintf("Unknown operator\n"); return; }
    kprintf("Result: %lld\n", (long long)result);
}

static int resolve_color_name(const char* input, const char** end_ptr) {
    int best_match = -1;
    size_t best_len = 0;
    for (int i = 0; i < 16; i++) {
        const char* name = COLOR_NAMES[i];
        size_t name_len = kstrlen(name);
        if (kstrncmp(input, name, name_len) == 0) {
             if (name_len > best_len) { best_match = i; best_len = name_len; }
        }
    }
    if (best_match != -1) { if(end_ptr) *end_ptr = input + best_len; return best_match; }
    return -1;
}

static bool parse_color_arg(const char** cursor, int* out_color) {
    *cursor = kskip_spaces(*cursor);
    const char* tmp = *cursor;
    unsigned int val;
    if (kparse_uint(&tmp, &val) && val < 16) { *out_color = val; *cursor = tmp; return true; }
    int idx = resolve_color_name(*cursor, &tmp);
    if (idx != -1) { *out_color = idx; *cursor = tmp; return true; }
    return false;
}

static void command_foreground(const char* args) {
    int fg; const char* c = args;
    if (parse_color_arg(&c, &fg)) { 
        uint8_t ofg, obg; terminal_getcolors(&ofg, &obg);
        terminal_set_theme((uint8_t)fg, obg);
    } else kprintf("Usage: foreground <color>\n");
}

static void command_background(const char* args) {
    int bg; const char* c = args;
    if (parse_color_arg(&c, &bg)) { 
        uint8_t ofg, obg; terminal_getcolors(&ofg, &obg);
        terminal_set_theme(ofg, (uint8_t)bg);
    } else kprintf("Usage: background <color>\n");
}

static void shell_print_banner(void) {
    kprintf("%s\n%s\nType 'help' for commands.\n", OS_BANNER_LINE, OS_WELCOME_LINE);
}

static void shell_print_prompt(void) {
    terminal_newline();
    kprintf(OS_PROMPT_TEXT);
}

static void command_help(const char* args) {
    (void)args;
    kprintf("Available commands:\n");
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        kprintf("  %s", COMMANDS[i].name);
        size_t len = kstrlen(COMMANDS[i].name);
        while (len++ < 12) terminal_write_char(' ');
        kprintf("- %s", COMMANDS[i].description);
        if (terminal_capture_active() &&
            (COMMANDS[i].flags & SHELL_COMMAND_CONSOLE_ONLY) != 0) {
            kprintf(" [console only]");
        }
        terminal_newline();
    }
}

static void command_palette(const char* args) {
    if (*kskip_spaces(args) != '\0') {
        kprintf("Usage: palette\n");
        return;
    }

    uint8_t old_fg = 0;
    uint8_t old_bg = 0;
    terminal_getcolors(&old_fg, &old_bg);
    kprintf("IBM PC palette:\n");

    for (unsigned int i = 0; i < 16; i++) {
        if (terminal_capture_active()) {
            kprintf("[%2u] %s\n", i, COLOR_NAMES[i]);
            continue;
        }

        uint8_t swatch_fg = (i < 8) ? 15 : 0;
        terminal_setcolors(swatch_fg, (uint8_t)i);
        terminal_writestring("    ");
        terminal_setcolors(old_fg, old_bg);
        kprintf(" %2u - %s\n", i, COLOR_NAMES[i]);
    }
}

static void command_net(const char* args) {
    if (*kskip_spaces(args) != '\0') {
        kprintf("Usage: net\n");
        return;
    }
    network_poll();
    struct network_status status;
    network_get_status(&status);
    if (!status.device_present) {
        kprintf("Network: no compatible RTL8139 device detected.\n");
        return;
    }

    char mac[18];
    char address[16];
    char gateway[16];
    char dns[16];
    network_format_mac(status.mac, mac, sizeof(mac));
    network_format_ipv4(
        status.ipv4_address, address, sizeof(address));
    network_format_ipv4(status.gateway, gateway, sizeof(gateway));
    network_format_ipv4(
        status.dns_server, dns, sizeof(dns));
    kprintf("Device: RTL8139 (%s), MAC %s\n",
            status.device_ready ? "ready" : "not ready", mac);
    kprintf("Link: %s\n", status.link_up ? "up" : "down");
    if (status.configured) {
        kprintf("IPv4: %s  Gateway: %s  DNS: %s\n",
                address, gateway, dns);
    } else {
        kprintf("IPv4: %s\n",
                status.dhcp_in_progress
                    ? "DHCP configuration in progress"
                    : "not configured");
    }
    kprintf("Packets: RX %llu  TX %llu  dropped %llu\n",
            (unsigned long long)status.received_packets,
            (unsigned long long)status.transmitted_packets,
            (unsigned long long)status.dropped_packets);
}

static void command_dhcp(const char* args) {
    if (*kskip_spaces(args) != '\0') {
        kprintf("Usage: dhcp\n");
        return;
    }
    kprintf("Requesting an IPv4 configuration...\n");
    enum network_result result = network_renew_dhcp(5000u);
    if (result != NETWORK_OK) {
        kprintf("DHCP failed: %s.\n", network_result_text(result));
        return;
    }
    struct network_status status;
    char address[16];
    network_get_status(&status);
    network_format_ipv4(
        status.ipv4_address, address, sizeof(address));
    kprintf("DHCP configured IPv4 %s.\n", address);
    syslog_write("Network: DHCP command completed");
}

static void command_dns(const char* args) {
    const char* hostname = kskip_spaces(args);
    if (*hostname == '\0') {
        kprintf("Usage: dns <hostname>\n");
        return;
    }
    for (const char* cursor = hostname; *cursor != '\0'; cursor++) {
        if (*cursor == ' ' || *cursor == '\t') {
            kprintf("Usage: dns <hostname>\n");
            return;
        }
    }

    uint32_t address = 0;
    enum network_result result =
        network_resolve_ipv4(hostname, 4000u, &address);
    if (result != NETWORK_OK) {
        kprintf("DNS failed: %s.\n", network_result_text(result));
        return;
    }
    char formatted[16];
    network_format_ipv4(address, formatted, sizeof(formatted));
    kprintf("%s resolves to %s.\n", hostname, formatted);
    syslog_write("Network: DNS command completed");
}

static void command_ping(const char* args) {
    const char* host = kskip_spaces(args);
    if (*host == '\0') {
        kprintf("Usage: ping <IPv4-or-hostname>\n");
        return;
    }
    for (const char* cursor = host; *cursor != '\0'; cursor++) {
        if (*cursor == ' ' || *cursor == '\t') {
            kprintf("Usage: ping <IPv4-or-hostname>\n");
            return;
        }
    }

    uint32_t round_trip = 0;
    enum network_result result =
        network_ping(host, 3000u, &round_trip);
    if (result != NETWORK_OK) {
        kprintf("Ping failed: %s.\n", network_result_text(result));
        return;
    }
    kprintf("Reply from %s in %u ms.\n", host, round_trip);
    syslog_write("Network: ICMP echo command completed");
}

static void command_http(const char* args) {
    const char* url = kskip_spaces(args);
    if (*url == '\0') {
        kprintf("Usage: http <http://URL>\n");
        return;
    }
    for (const char* cursor = url; *cursor != '\0'; cursor++) {
        if (*cursor == ' ' || *cursor == '\t') {
            kprintf("Usage: http <http://URL>\n");
            return;
        }
    }

    const size_t capacity = 4096u;
    char* body = (char*)kmalloc(capacity);
    if (body == NULL) {
        kprintf("HTTP failed: no response memory.\n");
        return;
    }
    size_t length = 0;
    unsigned int status_code = 0;
    enum network_result result = network_http_get(
        url, body, capacity, &length, &status_code, 8000u);
    if (result == NETWORK_OK) {
        kprintf("HTTP %u (%zu bytes)\n", status_code, length);
        terminal_write(body, length);
        if (length == 0 || body[length - 1u] != '\n') {
            terminal_newline();
        }
        syslog_write("Network: HTTP command completed");
    } else {
        kprintf("HTTP failed: %s.\n", network_result_text(result));
    }
    kfree(body);
}

static void command_about(const char* args) {
    (void)args;
    kprintf("%s\n%s\n%s\n", OS_ABOUT_SUMMARY, OS_ABOUT_FOCUS, OS_ABOUT_FEATURES);
}

static void command_clear(const char* args) {
    (void)args;
    background_render();
    shell_print_banner();
}

static void command_history(const char* args) {
    (void)args;
    size_t count = keyboard_history_length();
    kprintf("History:\n");
    for (size_t i = 0; i < count; i++) {
        kprintf("%u. %s\n", (unsigned int)(i+1), keyboard_history_entry(i));
    }
}

static void command_ls(const char* args) {
    (void)args;
    size_t count = fs_file_count();
    kprintf("Files (%zu):\n", count);
    for (size_t i = 0; i < count; i++) {
        const struct fs_file* f = fs_file_at(i);
        if(f) kprintf("  %s (%zu bytes)\n", f->name, f->size);
    }
}

static void command_cat(const char* args) {
    const char* name = kskip_spaces(args);
    const struct fs_file* f = fs_find(name);
    if (!f) {
        kprintf("File not found.\n");
        return;
    }
    for (size_t i = 0; i < f->size; i++) {
        unsigned char c = (unsigned char)f->data[i];
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 32 || c > 126) {
            kprintf("Binary file; use hexdump %s instead.\n", f->name);
            return;
        }
    }
    terminal_write(f->data, f->size);
    terminal_newline();
}

static void command_hexdump(const char* args) {
    const char* name = kskip_spaces(args);
    const struct fs_file* f = fs_find(name);
    if (!f) { kprintf("File not found.\n"); return; }
    for(size_t i=0; i<f->size; i++) {
        if(i%16==0) kprintf("\n%04zx: ", i);
        kprintf("%02x ", (unsigned int)(unsigned char)f->data[i]);
    }
    terminal_newline();
}

static void command_touch(const char* args) {
    const char* name = kskip_spaces(args);
    bool existed = fs_find(name) != NULL;
    if (fs_touch(name)) {
        if (existed) kprintf("Already exists: %s\n", name);
        else kprintf("Created %s\n", name);
    } else {
        kprintf("Failed.\n");
    }
}

static void command_write(const char* args) {
    const char* cursor = kskip_spaces(args);
    char name[FS_MAX_FILENAME];
    size_t length = 0;
    while (cursor[length] != '\0' && cursor[length] != ' ' &&
           cursor[length] != '\t' && length + 1 < sizeof(name)) {
        name[length] = cursor[length];
        length++;
    }
    name[length] = '\0';
    if (length == 0 || (cursor[length] != '\0' && cursor[length] != ' ' &&
                        cursor[length] != '\t')) {
        kprintf("Usage: write <file> <content>\n");
        return;
    }
    const char* contents = kskip_spaces(cursor + length);
    if (*contents == '\0') {
        kprintf("Usage: write <file> <content>\n");
        return;
    }
    if (fs_write(name, contents)) kprintf("Wrote %s\n", name);
    else kprintf("Write failed.\n");
}

static void command_append(const char* args) {
    const char* cursor = kskip_spaces(args);
    char name[FS_MAX_FILENAME];
    size_t length = 0;
    while (cursor[length] != '\0' && cursor[length] != ' ' &&
           cursor[length] != '\t' && length + 1 < sizeof(name)) {
        name[length] = cursor[length];
        length++;
    }
    name[length] = '\0';
    if (length == 0 || (cursor[length] != '\0' && cursor[length] != ' ' &&
                        cursor[length] != '\t')) {
        kprintf("Usage: append <file> <content>\n");
        return;
    }
    const char* contents = kskip_spaces(cursor + length);
    if (*contents == '\0') {
        kprintf("Usage: append <file> <content>\n");
        return;
    }
    if (fs_append(name, contents)) kprintf("Appended %s\n", name);
    else kprintf("Append failed.\n");
}

static void command_rm(const char* args) {
    const char* name = kskip_spaces(args);
    if(fs_remove(name)) kprintf("Removed %s\n", name);
    else kprintf("Failed.\n");
}

static void command_sysinfo(const char* args) {
    (void)args;
    const struct BootInfo* boot = system_boot_info();
    const struct system_profile* prof = system_profile_info();
    kprintf("Resolution: %ux%u\n", boot->width, boot->height);
    kprintf("Usable physical RAM: %llu KB\n",
            (unsigned long long)prof->memory_total_kb);
    kprintf("Mapped usable RAM: %llu KB\n",
            (unsigned long long)prof->memory_mapped_kb);
    kprintf("Kernel committed RAM: %llu KB (%llu reserved + %llu heap)\n",
            (unsigned long long)prof->memory_used_kb,
            (unsigned long long)prof->memory_reserved_kb,
            (unsigned long long)prof->memory_heap_committed_kb);
    kprintf("Storage: %s\n", fs_backend_status_text());
}

static void command_memtest(const char* args) {
    (void)args;
    memtest_run_diagnostic();
}

static void command_logs(const char* args) {
    (void)args;
    size_t count = syslog_length();
    for(size_t i=0; i<count; i++) kprintf("[%zu] %s\n", i, syslog_entry(i));
}

static void command_snake(const char* args) {
    (void)args;
    timer_set_callback(NULL);
    snake_game_run();
    background_render();
    shell_print_banner();
    timer_set_callback(background_animate);
}

static void command_beep(const char* args) {
    (void)args;
    sound_beep(440, 20);
}

static void command_disktest(const char* args) {
    (void)args;
    ata_init_result_t result = ata_init();
    if (result == ATA_INIT_READY) {
        kprintf("ATA Init OK.\n");
    } else if (result == ATA_INIT_NO_DEVICE) {
        kprintf("ATA Init Failed: no ATA device.\n");
    } else {
        kprintf("ATA Init Failed: controller or device error.\n");
    }
}

static void command_fsupgrade(const char* args) {
    const char* confirmation = kskip_spaces(args);
    fs_legacy_upgrade_status_t status = fs_legacy_upgrade_status();

    if (*confirmation == '\0') {
        kprintf("Legacy filesystem upgrade: %s.\n",
                fs_legacy_upgrade_status_text());
        if (status == FS_LEGACY_UPGRADE_AVAILABLE) {
            kprintf(
                "Two legacy snapshots differ, so their generation order "
                "is not integrity-protected.\n"
                "The currently mounted recovery view was selected by the "
                "legacy generation field.\n"
                "Review its files first. The other snapshot will not be "
                "merged, and no disk changes\n"
                "have been made. To preserve this view as a verified v4 "
                "commit, run exactly:\n"
                "  fsupgrade " FS_LEGACY_UPGRADE_CONFIRMATION "\n");
        }
        return;
    }

    fs_legacy_upgrade_result_t result =
        fs_upgrade_legacy_snapshot(confirmation);
    switch (result) {
        case FS_LEGACY_UPGRADE_SUCCEEDED:
            kprintf(
                "Legacy recovery view committed and read back as filesystem "
                "v4.\n");
            break;
        case FS_LEGACY_UPGRADE_CONFIRMATION_REQUIRED:
            kprintf(
                "Confirmation did not match. No disk changes were requested.\n"
                "Run 'fsupgrade' to review the warning and exact command.\n");
            break;
        case FS_LEGACY_UPGRADE_FAILED:
            kprintf(
                "Legacy upgrade failed. The mounted view remains session-only; "
                "the selected\nlegacy snapshot was not replaced.\n");
            break;
        case FS_LEGACY_UPGRADE_UNCERTAIN:
            kprintf(
                "Legacy upgrade could not be verified. Do not retry in this "
                "session; reboot\n"
                "so Nostalux can inspect the on-disk commit safely.\n");
            break;
        case FS_LEGACY_UPGRADE_NOT_AVAILABLE:
        default:
            kprintf("No legacy recovery upgrade is currently available.\n");
            break;
    }
}

static const char* app_process_state_text(enum app_process_state state) {
    switch (state) {
        case APP_PROCESS_LOADED: return "loaded";
        case APP_PROCESS_STARTING: return "starting";
        case APP_PROCESS_RUNNING: return "running";
        case APP_PROCESS_EXITED: return "exited";
        case APP_PROCESS_FAULTED: return "faulted";
        case APP_PROCESS_UNUSED:
        default:
            return "unused";
    }
}

static void command_apps(const char* args) {
    if (*kskip_spaces(args) != '\0') {
        kprintf("Usage: apps\n");
        return;
    }

    size_t catalog_count = app_catalog_count();
    kprintf("ELF applications (%zu):\n", catalog_count);
    for (size_t index = 0; index < catalog_count; index++) {
        const struct app_catalog_entry* entry = app_catalog_at(index);
        if (entry == NULL) continue;
        kprintf("  %s - %s\n",
                entry->manifest.id,
                entry->manifest.display_name);
        kprintf("    %s\n", entry->manifest.description);
    }
    if (catalog_count == 0) {
        kprintf("  No validated embedded applications are available.\n");
    } else {
        kprintf("Run one with: app <id> [startup-argument]\n");
    }

    size_t process_count = app_process_count();
    kprintf("Process history (%zu):\n", process_count);
    for (size_t index = 0; index < process_count; index++) {
        struct app_process_info process;
        if (!app_process_snapshot(index, &process)) continue;
        kprintf("  #%llu %s: %s",
                (unsigned long long)process.process_id,
                process.app_id,
                app_process_state_text(process.state));
        if (process.state == APP_PROCESS_EXITED) {
            kprintf(" (code %lld)", (long long)process.exit_code);
        } else if (process.state == APP_PROCESS_FAULTED) {
            kprintf(" (vector %u at %llx)",
                    (unsigned int)process.fault.vector,
                    (unsigned long long)process.fault.instruction_pointer);
        }
        kprintf("\n");
    }
    if (process_count == 0) {
        kprintf("  No application processes have run.\n");
    }
}

static void command_app(const char* args) {
    const char* cursor = kskip_spaces(args);
    char app_id[APP_ID_CAPACITY];
    size_t length = 0;
    while (cursor[length] != '\0' &&
           cursor[length] != ' ' &&
           cursor[length] != '\t' &&
           length + 1u < sizeof(app_id)) {
        app_id[length] = cursor[length];
        length++;
    }
    app_id[length] = '\0';
    if (length == 0) {
        kprintf("Usage: app <id> [startup-argument]\n");
        return;
    }

    const char* startup_argument =
        kskip_spaces(cursor + length);
    enum app_runtime_launch_result result =
        *startup_argument == '\0'
            ? app_runtime_run_catalog_id(app_id)
            : app_runtime_run_catalog_id_with_argument(
                app_id, startup_argument);
    if (result == APP_RUNTIME_LAUNCH_OK) {
        kprintf("App '%s' exited normally.\n", app_id);
    } else if (result == APP_RUNTIME_LAUNCH_APP_FAULTED) {
        kprintf(
            "App '%s' faulted in ring 3 and was isolated; "
            "the OS is still running.\n",
            app_id);
    } else if (result == APP_RUNTIME_LAUNCH_DISPATCH_BUDGET) {
        kprintf(
            "App '%s' is still running in the background.\n"
            "Use 'apps' to find its process ID, then 'appkill <process-id>'.\n",
            app_id);
    } else {
        kprintf("App '%s' was not completed: %s.\n",
                app_id, app_runtime_launch_result_text(result));
    }
}

static bool parse_process_id(const char** cursor,
                             uint64_t* out_process_id) {
    if (cursor == NULL || *cursor == NULL ||
        out_process_id == NULL) {
        return false;
    }

    const char* input = kskip_spaces(*cursor);
    if (*input < '0' || *input > '9') return false;

    uint64_t value = 0;
    while (*input >= '0' && *input <= '9') {
        const uint64_t digit = (uint64_t)(*input - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        input++;
    }

    *cursor = input;
    *out_process_id = value;
    return value != 0;
}

static void command_appkill(const char* args) {
    const char* cursor = kskip_spaces(args);
    uint64_t process_id = 0;
    if (!parse_process_id(&cursor, &process_id) ||
        *kskip_spaces(cursor) != '\0') {
        kprintf("Usage: appkill <process-id>\n");
        return;
    }

    if (scheduler_terminate_app_process(process_id)) {
        kprintf("Stopped app process #%llu.\n",
                (unsigned long long)process_id);
    } else {
        kprintf("No running app process #%llu.\n",
                (unsigned long long)process_id);
    }
}

static void command_reboot(const char* args) {
    (void)args;
#ifndef NOSTALUX_HOST_TEST
    outb(0x64, 0xFE);
#endif
}

static void command_shutdown(const char* args) {
    (void)args;
#ifdef NOSTALUX_HOST_TEST
    kprintf("Shutdown is unavailable in a host-side test.\n");
#else
    kprintf("Shutting down...\n");
    outw(0x604, 0x2000); 
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    for(;;) __asm__ volatile ("cli; hlt");
#endif
}

static void command_banner(const char* args) {
    (void)args;
    timer_set_callback(NULL);
    banner_run();
    timer_set_callback(background_animate);
    shell_print_banner();
}

static void command_gui(const char* args) {
    (void)args;
    
    kprintf("Launching graphical desktop...\n");
    timer_set_callback(NULL); // Stop kernel background animation
    scheduler_set_current_name("kernel/desktop");

    // The desktop currently shares kernel graphics, input, heap, and filesystem
    // APIs, so run it synchronously until the user presses Escape.
    gui_demo_run();

    scheduler_set_current_name("kernel/shell");
    background_render();
    shell_print_banner();
    timer_set_callback(background_animate);
}

static void command_echo(const char* args) {
    kprintf("%s\n", kskip_spaces(args));
}

static void execute_command(const char* input) {
    const char* trimmed = kskip_spaces(input);
    if (*trimmed == '\0') return;
    keyboard_history_record(trimmed);

    char cmd_name[32];
    size_t i = 0;
    while(trimmed[i] && trimmed[i] != ' ' && trimmed[i] != '\t' && i < 31) {
        cmd_name[i] = trimmed[i];
        i++;
    }
    cmd_name[i] = '\0';
    
    const char* args = trimmed + i;

    for (size_t j = 0; j < COMMAND_COUNT; j++) {
        if (kstrcmp(cmd_name, COMMANDS[j].name) == 0) {
            if (terminal_capture_active() &&
                (COMMANDS[j].flags & SHELL_COMMAND_CONSOLE_ONLY) != 0) {
                kprintf("Command '%s' is unavailable in the desktop terminal; "
                        "it requires exclusive console control or would "
                        "block or stop the desktop.\n",
                        COMMANDS[j].name);
                return;
            }
            COMMANDS[j].handler(args);
            return;
        }
    }
    kprintf("Unknown command.\n");
}

bool shell_execute_capture(const char* command_line,
                           char* output,
                           size_t output_capacity,
                           size_t* output_length,
                           bool* truncated) {
    if (output_length != NULL) *output_length = 0;
    if (truncated != NULL) *truncated = false;
    if (command_line == NULL || output == NULL || output_capacity == 0) {
        return false;
    }

    /*
     * Match the interactive shell's input limit and copy before clearing the
     * output buffer, so callers may safely reuse one buffer for input/output.
     */
    char input[INPUT_CAPACITY];
    size_t input_length = 0;
    while (input_length + 1 < sizeof(input) &&
           command_line[input_length] != '\0') {
        input[input_length] = command_line[input_length];
        input_length++;
    }
    input[input_length] = '\0';
    bool input_too_long = command_line[input_length] != '\0';

    if (!terminal_capture_begin(output, output_capacity)) {
        output[0] = '\0';
        return false;
    }

    if (input_too_long) {
        kprintf("Command line is too long (maximum %u characters).\n",
                (unsigned int)(INPUT_CAPACITY - 1));
    } else {
        execute_command(input);
    }

    bool was_truncated = false;
    size_t length = terminal_capture_end(&was_truncated);
    if (output_length != NULL) *output_length = length;
    if (truncated != NULL) *truncated = was_truncated;
    return true;
}

static void shell_idle(void) {
    /*
     * Kernel tasks still yield cooperatively. Ring-3 apps receive a timer
     * quantum once dispatched, so even a non-yielding app returns here.
     */
    network_poll();
    schedule();
}

void shell_run(void) {
    scheduler_set_current_name("kernel/shell");
    char input[INPUT_CAPACITY];
    sound_init();
    shell_print_banner();

    for (;;) {
        shell_print_prompt();
        keyboard_read_line_ex(input, sizeof(input), shell_idle);
        execute_command(input);
    }
}
