#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "background.h"
#include "fs.h"
#include "io.h"
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
#include "rtc.h"
#include "scheduler.h"

struct shell_command {
    const char* name;
    void (*handler)(const char* args);
    const char* description;
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
static void command_banner(const char* args);
static void command_gui(const char* args);

static const struct shell_command COMMANDS[] = {
    {"help", command_help, "Show this help message"},
    {"about", command_about, "Learn more about " OS_NAME},
    {"clear", command_clear, "Clear the screen"},
    {"banner", command_banner, "Show moving banner screensaver"},
    {"gui", command_gui, "Launch graphical desktop"},
    {"time", command_time, "Show current RTC date/time"},
    {"uptime", command_uptime, "Show time since boot"},  
    {"sleep", command_sleep, "Pause for N seconds"},     
    {"calc", command_calc, "Simple math (e.g. 'calc 10 + 5')"},
    {"foreground", command_foreground, "Set text color"},
    {"background", command_background, "Set background color"},
    {"ls", command_ls, "List files and usage stats"},
    {"cat", command_cat, "Print a file's text content"},
    {"hexdump", command_hexdump, "View file content in hex"},
    {"touch", command_touch, "Create an empty file"},
    {"write", command_write, "Overwrite a file with new text"},
    {"append", command_append, "Append text to a file"},
    {"rm", command_rm, "Remove a file"},
    {"history", command_history, "Show recent commands"},
    {"sysinfo", command_sysinfo, "Display hardware info"},
    {"memtest", command_memtest, "Run memory diagnostics"},
    {"logs", command_logs, "Show system logs"},
    {"echo", command_echo, "Display text back to you"},
    {"snake", command_snake, "Play the Snake game"},
    {"beep", command_beep, "Test PC Speaker"},
    {"disktest", command_disktest, "Detect the primary ATA drive"},
    {"reboot", command_reboot, "Restart the system"},
    {"shutdown", command_shutdown, "Power off the system"},
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
        kprintf("- %s\n", COMMANDS[i].description);
    }
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
    if (f) { terminal_write(f->data, f->size); terminal_newline(); }
    else kprintf("File not found.\n");
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
    if(fs_touch(name)) kprintf("Created %s\n", name);
    else kprintf("Failed.\n");
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
    kprintf("Res: %ux%u | Mem: %uKB\n", boot->width, boot->height, prof->memory_total_kb);
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
    if(ata_init()) kprintf("ATA Init OK.\n");
    else kprintf("ATA Init Failed.\n");
}

static void command_reboot(const char* args) {
    (void)args;
    outb(0x64, 0xFE);
}

static void command_shutdown(const char* args) {
    (void)args;
    kprintf("Shutting down...\n");
    outw(0x604, 0x2000); 
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    for(;;) __asm__ volatile ("cli; hlt");
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
            COMMANDS[j].handler(args);
            return;
        }
    }
    kprintf("Unknown command.\n");
}

void shell_run(void) {
    scheduler_set_current_name("kernel/shell");
    char input[INPUT_CAPACITY];
    sound_init();
    shell_print_banner();

    for (;;) {
        shell_print_prompt();
        keyboard_read_line_ex(input, sizeof(input), NULL);
        execute_command(input);
    }
}
