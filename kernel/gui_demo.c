#include "gui_demo.h"
#include "graphics.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "syslog.h"
#include "kstring.h"
#include "kstdio.h"
#include "fs.h"
#include "system.h"
#include "heap.h"
#include "io.h"
#include "rtc.h"
#include "bmp.h"
#include "scheduler.h"
#include "shell.h"
#include "os_info.h"
#include <stdbool.h>
#include <limits.h>

// --- Kernel desktop services ---
// The desktop currently runs synchronously in kernel mode and shares these
// services directly. Keeping the boundary explicit makes a future user-mode
// port straightforward without pretending that isolation exists today.

static void desktop_yield(void) {
    /*
     * Render once per PIT tick instead of once per interrupt byte. Some host
     * frontends deliver the X and Y portions of one physical diagonal gesture
     * as adjacent events. Coalescing them into the same 100 Hz desktop frame
     * prevents a visible horizontal-then-vertical staircase.
     */
    schedule();
    uint64_t tick = timer_get_ticks();
    while (timer_get_ticks() == tick) {
        timer_idle_wait();
    }
}

static void desktop_get_mouse(MouseState* out) {
    if (out) *out = mouse_get_state();
}

static void* desktop_malloc(size_t size) {
    return kmalloc(size);
}

static void desktop_free(void* ptr) {
    kfree(ptr);
}

static void desktop_get_time(char* buf) {
    if (!rtc_format_time(buf, 9)) {
        buf[0] = '-'; buf[1] = '-'; buf[2] = ':';
        buf[3] = '-'; buf[4] = '-'; buf[5] = ':';
        buf[6] = '-'; buf[7] = '-'; buf[8] = '\0';
    }
}

static void desktop_log(const char* msg) {
    syslog_write(msg);
}

// --- Global State & Config ---
static volatile bool g_gui_running = false;

bool gui_is_running(void) { return g_gui_running; }
void gui_set_running(bool running) { g_gui_running = running; }

#define MAX_WINDOWS 16
#define WIN_CAPTION_H 28
#define TASKBAR_H 40
#define RESIZE_HANDLE 16
#define START_MENU_W 180
#define START_MENU_H 450
#define START_MENU_ITEM_H 28
#define START_MENU_ITEM_STEP 30
#define START_MENU_FIRST_Y 10
#define START_MENU_ITEM_COUNT 14
#define MINE_CELL_SIZE 18
#define MINE_GRID_PIXELS (MINE_GRID_W * MINE_CELL_SIZE)

// Colors
#define COL_TASKBAR     0xFF101010
#define COL_WIN_TITLE_1 0xFF003366
#define COL_WIN_TITLE_2 0xFF0055AA
#define COL_WIN_TEXT    0xFF000000
#define COL_BTN_FACE    0xFFDDDDDD
#define COL_BTN_SHADOW  0xFF555555
#define COL_BTN_HILIGHT 0xFFFFFFFF
#define COL_BLACK       0xFF000000
#define COL_WHITE       0xFFFFFFFF
#define COL_ACCENT      0xFF0078D7

// Theme Structure
typedef struct {
    uint32_t desktop;
    uint32_t taskbar;
    uint32_t win_body;
    uint32_t win_title_active;
    uint32_t win_title_inactive;
    uint32_t win_border;
    bool is_glass;
} Theme;

static Theme themes[] = {
    // 0: Ocean Glass
    { 0xFF004488, 0xAA101010, 0xFFF0F0F0, 0xFF003366, 0xFF505050, 0xFF000000, true },
    // 1: Retro Grey
    { 0xFF008080, 0xFFC0C0C0, 0xFFC0C0C0, 0xFF000080, 0xFF808080, 0xFFFFFFFF, false }
};
static int current_theme_idx = 0;
#define COL_DESKTOP  (themes[current_theme_idx].desktop)
#define COL_WIN_BODY (themes[current_theme_idx].win_body)

// --- App Types ---
typedef enum { 
    APP_NONE, APP_WELCOME, APP_NOTEPAD, APP_CALC, APP_FILES, 
    APP_SETTINGS, APP_TERMINAL, APP_BROWSER, APP_TASKMGR, APP_PAINT,
    APP_MINESWEEPER, APP_SYSMON, APP_RUN, APP_TICTACTOE, APP_IMAGEVIEW,
    APP_ABOUT, APP_ASSISTANT
} AppType;

// --- Context Menu Actions ---
enum {
    CTX_NONE,
    CTX_REFRESH,
    CTX_WALLPAPER,
    CTX_NEW_FILE,
    CTX_SYS_INFO,
    CTX_ABOUT
};

typedef struct {
    char label[32];
    int action_id;
} MenuItem;

typedef struct {
    bool active;
    int x, y, w, h;
    MenuItem items[8];
    int count;
} ContextMenu;

static ContextMenu g_ctx_menu;

// --- App States ---
#define CALC_ERROR_NONE 0
#define CALC_ERROR_DIV_ZERO 1
#define CALC_ERROR_OVERFLOW 2
typedef struct {
    int current_val;
    int accumulator;
    char op;
    bool new_entry;
    int error_code;
} CalcState;
typedef struct {
    char buffer[FS_MAX_FILE_SIZE];
    int length;
    char filename[FS_MAX_FILENAME];
    char status[48];
    bool dirty;
    uint64_t discard_deadline;
} NotepadState;
typedef struct {
    int selected_index;
    char selected_name[FS_MAX_FILENAME];
    int scroll_offset;
    bool renaming;
    bool rename_select_all;
    char rename_buffer[FS_MAX_FILENAME];
    int rename_length;
    char delete_name[FS_MAX_FILENAME];
    uint64_t delete_deadline;
    char status[48];
} FileManagerState;
typedef struct {
    bool wallpaper_enabled;
    int theme_id;
    char status[48];
} SettingsState;
#define GUI_TERM_LINES 20
#define GUI_TERM_LINE_LEN 80
typedef struct {
    char input[96];
    int input_len;
    char lines[GUI_TERM_LINES][GUI_TERM_LINE_LEN];
    int line_count;
    bool history_truncated;
} TerminalState;
typedef struct {
    char address[64];
    int address_len;
    char content[FS_MAX_FILE_SIZE];
    int content_len;
    char status[64];
    int scroll;
    uint64_t last_refresh_tick;
    bool address_select_all;
} BrowserState;
typedef struct {
    uint64_t selected_pid;
    size_t page_offset;
    char status[48];
} TaskMgrState;
#define PAINT_CANVAS_W 17
#define PAINT_CANVAS_H 17
typedef struct {
    uint32_t pixels[PAINT_CANVAS_W * PAINT_CANVAS_H];
    uint32_t current_color;
    char filename[FS_MAX_FILENAME];
    char status[48];
    bool dirty;
    uint64_t discard_deadline;
} PaintState;
typedef struct { char cmd[96]; int len; char status[48]; } RunState;

#define ASSISTANT_MAX_LINES 14
#define ASSISTANT_LINE_LEN 64
#define ASSISTANT_ROLE_AI 0
#define ASSISTANT_ROLE_USER 1
typedef struct {
    char input[80];
    int input_len;
    char lines[ASSISTANT_MAX_LINES][ASSISTANT_LINE_LEN];
    uint8_t roles[ASSISTANT_MAX_LINES];
    int line_count;
} AssistantState;

// Minesweeper
#define MINE_GRID_W 10
#define MINE_GRID_H 10
typedef struct {
    uint8_t grid[MINE_GRID_H][MINE_GRID_W]; // 9=mine, 0-8=neighbors
    uint8_t view[MINE_GRID_H][MINE_GRID_W]; // 0=covered, 1=revealed, 2=flag
    bool game_over;
    bool victory;
    int flags_placed;
} MineState;

// Tic Tac Toe
typedef struct {
    char board[3][3]; // 0=empty, 1=X, 2=O
    int turn; // 1=X, 2=O
    int winner; // 0=none, 1=X, 2=O, 3=Draw
} TicTacToeState;

// Image Viewer
typedef struct {
    char filename[FS_MAX_FILENAME];
    int file_index;
    bool fit_to_window;
    bool valid;
    struct bmp_image image;
    char status[48];
} ImageViewState;

// System Monitor
#define SYSMON_HIST 60
typedef struct {
    int cpu_hist[SYSMON_HIST];
    int mem_hist[SYSMON_HIST];
    int head;
    uint64_t last_sample_tick;
    struct timer_cpu_counters previous_cpu;
    uint64_t memory_used_kb;
    uint64_t memory_total_kb;
    uint64_t memory_mapped_kb;
    uint64_t memory_managed_kb;
    uint64_t memory_reserved_kb;
    uint64_t memory_heap_committed_kb;
    int cpu_percent;
    bool has_previous_cpu;
} SysMonState;

// About Window
typedef struct {
    int scroll_y;
} AboutState;

typedef struct {
    int id;
    AppType type;
    char title[32];
    int x, y, w, h;
    int min_w, min_h;
    bool visible, minimized, maximized, focused, dragging, resizing;
    int drag_off_x, drag_off_y;
    int restore_x, restore_y, restore_w, restore_h;
    union { 
        CalcState calc; NotepadState notepad; FileManagerState files; 
        SettingsState settings; TerminalState term; BrowserState browser;
        TaskMgrState taskmgr; PaintState paint; MineState mine;
        SysMonState sysmon; RunState run; TicTacToeState ttt;
        ImageViewState img; AboutState about; AssistantState assistant;
    } state;
} Window;

// Mouse Trails
#define TRAIL_LEN 10
typedef struct { int x, y; } Point;
static Point mouse_trail[TRAIL_LEN];
static int trail_head = 0;

static Window* windows[MAX_WINDOWS];
static bool start_menu_open = false;
static int screen_w, screen_h;
static MouseState mouse;
static MouseState prev_mouse;
static bool g_wallpaper_enabled = false;
static bool g_desktop_shown_mode = false;
static int g_wallpaper_seed = 1234;
static char g_desktop_notice[64];
static uint64_t g_desktop_notice_until = 0;
static uint64_t g_exit_discard_deadline = 0;
#define DISCARD_CONFIRM_TICKS 300u
static const AppType start_menu_apps[START_MENU_ITEM_COUNT] = {
    APP_ASSISTANT, APP_BROWSER, APP_TERMINAL, APP_PAINT,
    APP_FILES, APP_TASKMGR, APP_NOTEPAD, APP_CALC,
    APP_MINESWEEPER, APP_TICTACTOE, APP_IMAGEVIEW,
    APP_SYSMON, APP_RUN, APP_NONE
};
static const char* start_menu_labels[START_MENU_ITEM_COUNT] = {
    "AI Assistant", "Local Browser", "Terminal", "Paint",
    "Files", "Task Manager", "Notepad", "Calculator",
    "Minesweeper", "Tic-Tac-Toe", "Image Viewer",
    "Sys Monitor", "Run...", "Exit Desktop"
};

// --- Forward Declarations ---
static bool close_window(int index);
static int focus_window(int index);
static void toggle_maximize(Window* w);
static bool notepad_save(Window* w);
static bool paint_save(Window* w);
static void handle_paint_click(Window* w, int x, int y);
static void handle_notepad_click(Window* w, int x, int y);
static void handle_settings_click(Window* w, int x, int y);
static void handle_files_click(Window* w, int x, int y);
static void handle_taskmgr_click(Window* w, int x, int y);
static void handle_browser_click(Window* w, int x, int y);
static void handle_calc_logic(Window* w, int x, int y);
static void handle_terminal_input(Window* w, char c);
static void handle_browser_input(Window* w, char c);
static void handle_run_command(Window* w);
static void handle_assistant_input(Window* w, char c);
static void handle_assistant_click(Window* w, int x, int y);
static void handle_minesweeper(Window* w, int rx, int ry, bool right_click);
static void handle_tictactoe(Window* w, int x, int y);
static void handle_imageview(Window* w, int x, int y);
static void imageview_load(Window* w, int direction);
static bool imageview_open_named(Window* w, const char* name);
static void browser_load(Window* w);
static void terminal_execute(Window* w);
static void settings_load(void);
static bool settings_save(void);
static bool desktop_save_all(void);
static void desktop_notify(const char* message);
static bool open_file_has_unsaved_changes(const char* filename);
static void reconcile_filesystem_windows(const char* files_status);
static void desktop_refresh(void);
static void render_window(Window* w);
static void render_assistant_app(Window* w);
static void draw_wallpaper(void);
static void on_click(int x, int y);
static void on_right_click(int x, int y);
static void update_sysmon(Window* w);
static Window* create_window(AppType type, const char* title, int w, int h);
static bool launch_app(AppType type);
static Window* get_top_window(void);

// Context Menu
static void show_context_menu(int x, int y);
static void hide_context_menu(void);
static void handle_context_menu_click(int x, int y);
static void render_context_menu(void);

// Pseudo-random
static unsigned long rand_state = 1234;
static int fast_rand(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return (unsigned int)(rand_state / 65536) % 32768;
}

// --- Helpers ---
static int kstrlen_local(const char* s) { int l=0; while(s[l]) l++; return l; }
static void str_copy(char* d, int capacity, const char* s) {
    int i = 0;
    if (!d || capacity <= 0) return;
    while (s && s[i] && i + 1 < capacity) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}
static void desktop_notify(const char* message) {
    str_copy(g_desktop_notice, sizeof(g_desktop_notice),
             message ? message : "");
    g_desktop_notice_until = timer_get_ticks() + 500u;
}
static void cancel_exit_discard_confirmation(void) {
    if (g_exit_discard_deadline == 0) return;
    g_exit_discard_deadline = 0;
    g_desktop_notice[0] = 0;
    g_desktop_notice_until = 0;
}
static void mem_zero(void* ptr, size_t size) {
    uint8_t* bytes = (uint8_t*)ptr;
    for (size_t i = 0; i < size; i++) bytes[i] = 0;
}
static bool str_append(char* d, int capacity, const char* s) {
    int i = kstrlen_local(d);
    int j = 0;
    while (s[j] && i + 1 < capacity) d[i++] = s[j++];
    d[i] = 0;
    return s[j] == 0;
}
static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}
static bool text_contains_ci(const char* text, const char* needle) {
    if (!needle[0]) return true;
    for (int i = 0; text[i]; i++) {
        int j = 0;
        while (needle[j] && text[i+j] &&
               ascii_lower(text[i+j]) == ascii_lower(needle[j])) {
            j++;
        }
        if (!needle[j]) return true;
    }
    return false;
}
static bool text_equals_ci_trimmed(const char* text, const char* expected) {
    if (!text || !expected) return false;
    while (*text == ' ' || *text == '\t') text++;

    int text_length = kstrlen_local(text);
    while (text_length > 0 &&
           (text[text_length - 1] == ' ' ||
            text[text_length - 1] == '\t')) {
        text_length--;
    }
    int expected_length = kstrlen_local(expected);
    if (text_length != expected_length) return false;
    for (int i = 0; i < text_length; i++) {
        if (ascii_lower(text[i]) != ascii_lower(expected[i])) return false;
    }
    return true;
}
static bool text_equals_ci(const char* left, const char* right) {
    int i = 0;
    while (left[i] && right[i]) {
        if (ascii_lower(left[i]) != ascii_lower(right[i])) return false;
        i++;
    }
    return left[i] == 0 && right[i] == 0;
}
static bool rect_contains(int x, int y, int w, int h, int px, int py) {
    return (px >= x && px < x + w && py >= y && py < y + h);
}
static void int_to_str(int v, char* buf) {
    if(v==0){buf[0]='0';buf[1]=0;return;}
    bool n = v < 0;
    unsigned int magnitude = n ? (0U - (unsigned int)v) : (unsigned int)v;
    int i=0; char t[16]; while(magnitude>0){t[i++]='0'+(magnitude%10);magnitude/=10;}
    if(n)t[i++]='-';
    int j=0; while(i>0)buf[j++]=t[--i]; buf[j]=0;
}
static void uint64_to_str(uint64_t value, char* buf) {
    if (value == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    char reversed[24];
    int count = 0;
    while (value > 0 && count < (int)sizeof(reversed)) {
        reversed[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    int out = 0;
    while (count > 0) buf[out++] = reversed[--count];
    buf[out] = 0;
}

static int wrapped_text_line_count(const char* text, int max_columns) {
    int lines = 1;
    int column = 0;
    if (max_columns < 1) max_columns = 1;
    for (int i = 0; text && text[i]; i++) {
        char current = text[i];
        if (current == '\r') continue;
        if (current == '\n') {
            lines++;
            column = 0;
            continue;
        }
        if (column >= max_columns) {
            lines++;
            column = 0;
        }
        column++;
    }
    if (column >= max_columns) lines++;
    return lines;
}

static int wrapped_content_line_count(const char* text, int max_columns) {
    int lines = 1;
    int column = 0;
    if (max_columns < 1) max_columns = 1;
    for (int i = 0; text && text[i]; i++) {
        char current = text[i];
        if (current == '\r') continue;
        if (current == '\n') {
            lines++;
            column = 0;
            continue;
        }
        if (column >= max_columns) {
            lines++;
            column = 0;
        }
        column++;
    }
    return lines;
}

static void wrapped_text_cursor(const char* text, int max_columns,
                                int* output_row, int* output_column) {
    int row = 0;
    int column = 0;
    if (max_columns < 1) max_columns = 1;
    for (int i = 0; text && text[i]; i++) {
        char current = text[i];
        if (current == '\r') continue;
        if (current == '\n') {
            row++;
            column = 0;
            continue;
        }
        if (column >= max_columns) {
            row++;
            column = 0;
        }
        column++;
    }
    if (column >= max_columns) {
        row++;
        column = 0;
    }
    if (output_row) *output_row = row;
    if (output_column) *output_column = column;
}

static bool str_starts_with(const char* text, const char* prefix) {
    int i = 0;
    if (!text || !prefix) return false;
    while (prefix[i]) {
        if (text[i] != prefix[i]) return false;
        i++;
    }
    return true;
}

static bool str_ends_with_ci(const char* text, const char* suffix) {
    int text_length = kstrlen_local(text);
    int suffix_length = kstrlen_local(suffix);
    if (suffix_length > text_length) return false;
    int offset = text_length - suffix_length;
    for (int i = 0; i < suffix_length; i++) {
        if (ascii_lower(text[offset + i]) != ascii_lower(suffix[i])) return false;
    }
    return true;
}

static bool file_is_text(const struct fs_file* file) {
    if (!file) return false;
    for (size_t i = 0; i < file->size; i++) {
        unsigned char c = (unsigned char)file->data[i];
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 32 || c > 126) return false;
    }
    return true;
}

static const char* storage_short_label(void) {
    switch (fs_backend_status()) {
        case FS_BACKEND_PERSISTENT:
            return "persistent ATA";
        case FS_BACKEND_VOLATILE_NO_DRIVE:
            return "volatile (no ATA)";
        case FS_BACKEND_VOLATILE_CORRUPT:
            return "volatile (disk preserved)";
        case FS_BACKEND_VOLATILE_IO_ERROR:
            return "volatile (I/O error)";
        case FS_BACKEND_UNINITIALIZED:
        default:
            return "not initialized";
    }
}

static const char* storage_compact_label(void) {
    switch (fs_backend_status()) {
        case FS_BACKEND_PERSISTENT:
            return "ATA persistent";
        case FS_BACKEND_VOLATILE_NO_DRIVE:
            return "RAM only/no ATA";
        case FS_BACKEND_VOLATILE_CORRUPT:
            return "RAM/disk preserved";
        case FS_BACKEND_VOLATILE_IO_ERROR:
            return "RAM only/I-O error";
        case FS_BACKEND_UNINITIALIZED:
        default:
            return "not initialized";
    }
}

static bool browser_address_is_dynamic(const char* address) {
    return kstrcmp(address, "about:files") == 0 ||
           kstrcmp(address, "about:system") == 0 ||
           kstrcmp(address, "system.log") == 0 ||
           kstrcmp(address, "file:system.log") == 0;
}

static int file_index_by_name(const char* name) {
    size_t count = fs_file_count();
    for (size_t i = 0; i < count; i++) {
        const struct fs_file* file = fs_file_at(i);
        if (file && kstrcmp(file->name, name) == 0) return (int)i;
    }
    return -1;
}

static bool create_unique_file(const char* prefix, const char* extension,
                               char output[FS_MAX_FILENAME]) {
    for (int number = 1; number < 1000; number++) {
        char candidate[FS_MAX_FILENAME];
        char digits[16];
        str_copy(candidate, sizeof(candidate), prefix);
        int_to_str(number, digits);
        str_append(candidate, sizeof(candidate), digits);
        str_append(candidate, sizeof(candidate), extension);
        if (fs_find(candidate) == NULL && fs_touch(candidate)) {
            str_copy(output, FS_MAX_FILENAME, candidate);
            return true;
        }
    }
    output[0] = 0;
    return false;
}

static int config_read_int(const char* contents, const char* key, int fallback) {
    if (!contents || !key) return fallback;
    int key_length = kstrlen_local(key);
    for (int i = 0; contents[i]; i++) {
        bool at_line_start = i == 0 || contents[i - 1] == '\n';
        if (!at_line_start) continue;
        int j = 0;
        while (j < key_length && contents[i + j] == key[j]) j++;
        if (j != key_length || contents[i + j] != '=') continue;
        int value = 0;
        bool found_digit = false;
        j++;
        while (contents[i + j] >= '0' && contents[i + j] <= '9') {
            int digit = contents[i + j] - '0';
            found_digit = true;
            if (value > (INT_MAX - digit) / 10) return fallback;
            value = value * 10 + digit;
            j++;
        }
        if (!found_digit) return fallback;

        char terminator = contents[i + j];
        if (terminator == '\r') {
            char next = contents[i + j + 1];
            if (next != '\n' && next != '\0') return fallback;
        } else if (terminator != '\n' && terminator != '\0') {
            return fallback;
        }
        return value;
    }
    return fallback;
}

static void settings_load(void) {
    const struct fs_file* config = fs_find("desktop.cfg");
    if (!config || !file_is_text(config)) return;

    int theme = config_read_int(config->data, "theme", current_theme_idx);
    int wallpaper = config_read_int(config->data, "wallpaper",
                                    g_wallpaper_enabled ? 1 : 0);
    int seed = config_read_int(config->data, "seed", g_wallpaper_seed);
    if (theme >= 0 && theme < (int)(sizeof(themes) / sizeof(themes[0])))
        current_theme_idx = theme;
    if (wallpaper == 0 || wallpaper == 1)
        g_wallpaper_enabled = wallpaper == 1;
    if (seed >= 0) g_wallpaper_seed = seed;
}

static bool settings_save(void) {
    if (open_file_has_unsaved_changes("desktop.cfg")) {
        desktop_log("GUI: settings save blocked by modified desktop.cfg editor");
        return false;
    }

    char config[96] = "theme=";
    char number[16];
    int_to_str(current_theme_idx, number);
    str_append(config, sizeof(config), number);
    str_append(config, sizeof(config), "\nwallpaper=");
    str_append(config, sizeof(config), g_wallpaper_enabled ? "1" : "0");
    str_append(config, sizeof(config), "\nseed=");
    int_to_str(g_wallpaper_seed, number);
    str_append(config, sizeof(config), number);
    str_append(config, sizeof(config), "\n");
    if (!fs_write("desktop.cfg", config)) {
        desktop_log("GUI: failed to persist desktop settings");
        return false;
    }
    reconcile_filesystem_windows("desktop.cfg was updated.");
    return true;
}

// --- BITMAPS (Icons) ---
// Icons are 24x24 palette-indexed.

static const uint8_t CURSOR_BITMAP[19][12] = {
    {1,1,0,0,0,0,0,0,0,0,0,0}, {1,2,1,0,0,0,0,0,0,0,0,0}, {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0}, {1,2,2,2,2,1,0,0,0,0,0,0}, {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0}, {1,2,2,2,2,2,2,2,1,0,0,0}, {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0}, {1,2,2,2,2,2,1,1,1,1,1,1}, {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,1,1,2,2,1,0,0,0,0,0}, {1,1,0,1,2,2,1,0,0,0,0,0}, {0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0}, {0,0,0,0,0,1,2,2,1,0,0,0}, {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0}
};

static const uint8_t ICON_TERM[24][24] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,7,7,7,7,7,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,7,7,7,7,7,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
};

static const uint8_t ICON_PAINT[24][24] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,6,6,4,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,6,6,6,6,4,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,6,6,6,6,4,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,6,6,6,4,4,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,4,6,6,4,4,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,4,6,4,4,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,4,4,4,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,4,4,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,5,5,5,1,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,5,5,5,5,5,1,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,5,5,5,5,5,5,5,1,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,5,5,5,5,5,5,5,5,5,1,0,0,0,0,0,0,0,0,0},
    {0,0,0,1,5,5,5,5,5,5,5,5,5,5,5,1,0,0,0,0,0,0,0,0},
    {0,0,1,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0,0,0,0,0,0,0},
    {0,0,1,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0,0,0,0,0,0,0},
    {0,0,1,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0,0,0,0,0,0,0},
    {0,0,0,1,5,5,5,5,5,5,5,5,5,5,5,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
};

static const uint8_t ICON_BROWSER[24][24] = {
    {0,0,0,0,0,0,0,6,6,6,6,6,6,6,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,6,6,6,6,6,6,6,6,6,6,6,0,0,0,0,0,0,0,0},
    {0,0,0,0,6,6,6,6,6,6,4,4,6,6,6,6,6,0,0,0,0,0,0,0},
    {0,0,0,6,6,6,6,6,4,4,4,4,4,4,6,6,6,6,0,0,0,0,0,0},
    {0,0,6,6,6,6,6,4,4,4,4,4,4,4,4,6,6,6,6,0,0,0,0,0},
    {0,0,6,6,6,6,4,4,4,4,4,4,4,4,4,4,6,6,6,0,0,0,0,0},
    {0,6,6,6,6,4,4,4,4,4,4,4,4,4,4,4,4,6,6,6,0,0,0,0},
    {0,6,6,6,4,4,4,4,6,6,6,6,6,6,4,4,4,4,6,6,0,0,0,0},
    {6,6,6,4,4,4,6,6,6,6,6,6,6,6,6,6,4,4,4,6,6,0,0,0},
    {6,6,6,4,4,6,6,6,6,6,6,6,6,6,6,6,6,4,4,6,6,0,0,0},
    {6,6,6,4,6,6,6,6,6,6,6,6,6,6,6,6,6,6,4,6,6,0,0,0},
    {6,6,6,4,6,6,6,6,6,6,6,6,6,6,6,6,6,6,4,6,6,0,0,0},
    {6,6,6,4,6,6,6,6,6,6,6,6,6,6,6,6,6,6,4,6,6,0,0,0},
    {6,6,6,4,4,6,6,6,6,6,6,6,6,6,6,6,6,4,4,6,6,0,0,0},
    {6,6,6,4,4,4,6,6,6,6,6,6,6,6,6,6,4,4,4,6,6,0,0,0},
    {0,6,6,6,4,4,4,4,6,6,6,6,6,6,4,4,4,4,6,6,0,0,0,0},
    {0,6,6,6,6,4,4,4,4,4,4,4,4,4,4,4,4,6,6,6,0,0,0,0},
    {0,0,6,6,6,6,4,4,4,4,4,4,4,4,4,4,6,6,6,0,0,0,0,0},
    {0,0,6,6,6,6,6,4,4,4,4,4,4,4,4,6,6,6,6,0,0,0,0,0},
    {0,0,0,6,6,6,6,6,4,4,4,4,4,4,6,6,6,6,0,0,0,0,0,0},
    {0,0,0,0,6,6,6,6,6,6,4,4,6,6,6,6,6,0,0,0,0,0,0,0},
    {0,0,0,0,0,6,6,6,6,6,6,6,6,6,6,6,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,6,6,6,6,6,6,6,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
};

static const uint8_t ICON_TASKMGR[24][24] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,7,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,7,7,7,7,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,1,1,1,1,1,7,7,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,1,1,1,1,7,7,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,1,1,1,1,7,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,1,1,1,7,7,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,1,1,1,7,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,1,7,7,7,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,7,7,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,7,1,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,7,7,7,1,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,7,7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,7,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,7,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1},
    {1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
};

static const uint8_t ICON_FOLDER[24][24] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,5,5,5,5,5,5,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,0},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0}
};

static const uint8_t ICON_CALC[24][24] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {1,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1,0,0,0},
    {1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,0,0,0},
    {1,3,1,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,1,3,1,0,0,0},
    {1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,0,0,0},
    {1,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1,0,0,0},
    {1,3,1,1,1,3,1,1,1,3,1,1,1,3,1,1,1,3,3,3,1,0,0,0},
    {1,3,1,4,1,3,1,4,1,3,1,4,1,3,1,8,1,3,3,3,1,0,0,0},
    {1,3,1,1,1,3,1,1,1,3,1,1,1,3,1,1,1,3,3,3,1,0,0,0},
    {1,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1,0,0,0},
    {1,3,1,1,1,3,1,1,1,3,1,1,1,3,1,1,1,3,3,3,1,0,0,0},
    {1,3,1,4,1,3,1,4,1,3,1,4,1,3,1,4,1,3,3,3,1,0,0,0},
    {1,3,1,1,1,3,1,1,1,3,1,1,1,3,1,1,1,3,3,3,1,0,0,0},
    {1,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1,0,0,0},
    {1,3,1,1,1,3,1,1,1,3,1,1,1,3,1,1,1,3,3,3,1,0,0,0},
    {1,3,1,4,1,3,1,4,1,3,1,4,1,3,1,4,1,3,3,3,1,0,0,0},
    {1,3,1,1,1,3,1,1,1,3,1,1,1,3,1,1,1,3,3,3,1,0,0,0},
    {1,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1,0,0,0},
    {1,3,1,1,1,1,1,1,1,3,1,1,1,3,1,1,1,3,3,3,1,0,0,0},
    {1,3,1,4,4,4,4,4,1,3,1,4,1,3,1,4,1,3,3,3,1,0,0,0},
    {1,3,1,1,1,1,1,1,1,3,1,1,1,3,1,1,1,3,3,3,1,0,0,0},
    {1,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,1,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
};

static const uint8_t ICON_SET[24][24] = {
    {0,0,0,0,0,0,0,0,0,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,2,3,3,3,2,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,2,2,0,0,0,2,3,3,3,2,0,0,0,2,2,0,0,0,0,0},
    {0,0,0,2,3,2,0,0,2,2,3,3,3,2,2,0,0,2,3,2,0,0,0,0},
    {0,0,0,2,3,2,2,2,3,3,3,3,3,3,3,2,2,2,3,2,0,0,0,0},
    {0,0,0,0,2,3,3,3,3,3,3,3,3,3,3,3,3,3,2,0,0,0,0,0},
    {0,0,0,0,0,2,3,3,3,1,1,1,1,1,3,3,3,2,0,0,0,0,0,0},
    {0,0,0,0,0,2,3,3,1,4,4,4,4,4,1,3,3,2,0,0,0,0,0,0},
    {0,0,2,2,2,3,3,3,1,4,4,4,4,4,1,3,3,3,2,2,2,0,0,0},
    {0,2,3,3,3,3,3,3,1,4,4,4,4,4,1,3,3,3,3,3,3,2,0,0},
    {0,2,3,3,3,3,3,3,1,4,4,4,4,4,1,3,3,3,3,3,3,2,0,0},
    {0,2,3,3,3,3,3,3,1,4,4,4,4,4,1,3,3,3,3,3,3,2,0,0},
    {0,2,3,3,3,3,3,3,1,4,4,4,4,4,1,3,3,3,3,3,3,2,0,0},
    {0,2,3,3,3,3,3,3,1,4,4,4,4,4,1,3,3,3,3,3,3,2,0,0},
    {0,0,2,2,2,3,3,3,1,4,4,4,4,4,1,3,3,3,2,2,2,0,0,0},
    {0,0,0,0,0,2,3,3,1,4,4,4,4,4,1,3,3,2,0,0,0,0,0,0},
    {0,0,0,0,0,2,3,3,3,1,1,1,1,1,3,3,3,2,0,0,0,0,0,0},
    {0,0,0,0,2,3,3,3,3,3,3,3,3,3,3,3,3,3,2,0,0,0,0,0},
    {0,0,0,2,3,2,2,2,3,3,3,3,3,3,3,2,2,2,3,2,0,0,0,0},
    {0,0,0,2,3,2,0,0,2,2,3,3,3,2,2,0,0,2,3,2,0,0,0,0},
    {0,0,0,0,2,2,0,0,0,2,3,3,3,2,0,0,0,2,2,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,2,3,3,3,2,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,2,2,2,2,2,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
};

static const uint8_t ICON_GAME[24][24] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,1,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,1,0,0,0},
    {0,0,1,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,1,0,0,0},
    {0,0,1,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,1,0,0,0},
    {0,0,1,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,1,0,0,0},
    {0,0,1,8,8,1,1,1,8,8,8,8,8,8,4,4,8,8,8,8,1,0,0,0},
    {0,0,1,8,1,8,1,8,1,8,8,8,8,4,8,8,4,8,8,8,1,0,0,0},
    {0,0,1,8,1,1,1,1,1,8,8,8,8,4,8,8,4,8,8,8,1,0,0,0},
    {0,0,1,8,1,8,1,8,1,8,8,8,8,8,4,4,8,8,8,8,1,0,0,0},
    {0,0,1,8,8,1,1,1,8,8,8,8,8,8,8,8,8,8,8,8,1,0,0,0},
    {0,0,1,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,1,0,0,0},
    {0,0,1,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,1,0,0,0},
    {0,0,1,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,1,0,0,0},
    {0,0,1,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,1,0,0,0},
    {0,0,1,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,1,0,0,0},
    {0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
};

static const uint8_t ICON_IMAGE[24][24] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1,0,0},
    {0,1,4,4,5,5,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1,0,0},
    {0,1,4,5,5,5,5,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1,0,0},
    {0,1,4,4,5,5,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,4,4,4,4,7,7,7,4,4,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,4,4,4,4,7,7,7,7,4,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,4,4,4,7,7,7,7,7,7,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,4,4,7,7,7,7,7,7,7,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,4,7,7,7,7,6,6,7,7,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,4,7,7,7,7,6,6,6,6,7,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,4,7,7,7,7,6,6,6,6,6,6,4,1,0,0},
    {0,1,4,4,4,4,4,4,4,7,7,7,7,6,6,6,6,6,6,6,4,1,0,0},
    {0,1,4,4,4,4,4,4,7,7,7,7,6,6,6,6,6,6,6,6,4,1,0,0},
    {0,1,4,4,4,4,4,7,7,7,7,6,6,6,6,6,6,6,6,6,4,1,0,0},
    {0,1,4,4,4,4,7,7,7,7,6,6,6,6,6,6,6,6,6,6,4,1,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
};

static const uint8_t ICON_INFO[24][24] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,1,4,4,4,4,4,4,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,4,4,4,4,4,4,4,4,4,4,1,0,0,0,0,0,0},
    {0,0,0,0,0,1,4,4,4,6,6,6,6,4,4,4,4,4,1,0,0,0,0,0},
    {0,0,0,0,1,4,4,4,6,6,4,4,6,6,4,4,4,4,4,1,0,0,0,0},
    {0,0,0,0,1,4,4,4,6,6,4,4,6,6,4,4,4,4,4,1,0,0,0,0},
    {0,0,0,1,4,4,4,4,4,6,6,6,6,4,4,4,4,4,4,4,1,0,0,0},
    {0,0,0,1,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,1,0,0,0},
    {0,0,0,1,4,4,4,4,4,4,6,6,6,4,4,4,4,4,4,4,1,0,0,0},
    {0,0,0,1,4,4,4,4,4,4,6,6,6,4,4,4,4,4,4,4,1,0,0,0},
    {0,0,0,1,4,4,4,4,4,4,6,6,6,4,4,4,4,4,4,4,1,0,0,0},
    {0,0,0,1,4,4,4,4,4,4,6,6,6,4,4,4,4,4,4,4,1,0,0,0},
    {0,0,0,1,4,4,4,4,4,4,6,6,6,4,4,4,4,4,4,4,1,0,0,0},
    {0,0,0,1,4,4,4,4,4,4,6,6,6,4,4,4,4,4,4,4,1,0,0,0},
    {0,0,0,0,1,4,4,4,4,4,6,6,6,4,4,4,4,4,4,1,0,0,0,0},
    {0,0,0,0,1,4,4,4,4,4,6,6,6,4,4,4,4,4,4,1,0,0,0,0},
    {0,0,0,0,0,1,4,4,4,4,4,4,4,4,4,4,4,4,1,0,0,0,0,0},
    {0,0,0,0,0,0,1,4,4,4,4,4,4,4,4,4,4,1,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,1,4,4,4,4,4,4,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
};

// --- Window Management ---

static void focus_top_visible(void) {
    Window* target = NULL;
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        if (windows[i] && windows[i]->visible && !windows[i]->minimized) {
            target = windows[i];
            break;
        }
    }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i]) windows[i]->focused = (windows[i] == target);
    }
}

static void minimize_window(Window* w) {
    if (!w) return;
    bool was_focused = w->focused;
    w->minimized = true;
    w->focused = false;
    w->dragging = false;
    w->resizing = false;
    if (was_focused) focus_top_visible();
}

static bool close_window(int index) {
    if (index < 0 || index >= MAX_WINDOWS || windows[index] == NULL)
        return false;
    Window* w = windows[index];
    bool was_focused = w->focused;
    
    if (w->type == APP_NOTEPAD && w->state.notepad.dirty) {
        uint64_t now = timer_get_ticks();
        if (w->state.notepad.discard_deadline != 0 &&
            now <= w->state.notepad.discard_deadline) {
            w->state.notepad.dirty = false;
            w->state.notepad.discard_deadline = 0;
        } else if (!notepad_save(w)) {
            w->state.notepad.discard_deadline =
                timer_get_ticks() + DISCARD_CONFIRM_TICKS;
            str_copy(w->state.notepad.status,
                     sizeof(w->state.notepad.status),
                     "Save failed; click X again within 3s to discard.");
            return false;
        }
    }
    if (w->type == APP_PAINT && w->state.paint.dirty) {
        uint64_t now = timer_get_ticks();
        if (w->state.paint.discard_deadline != 0 &&
            now <= w->state.paint.discard_deadline) {
            w->state.paint.dirty = false;
            w->state.paint.discard_deadline = 0;
        } else if (!paint_save(w)) {
            w->state.paint.discard_deadline =
                timer_get_ticks() + DISCARD_CONFIRM_TICKS;
            str_copy(w->state.paint.status,
                     sizeof(w->state.paint.status),
                     "Save failed; click X again within 3s to discard.");
            return false;
        }
    }
    
    desktop_free(w);
    windows[index] = NULL;
    
    for (int i = index; i < MAX_WINDOWS - 1; i++) {
        windows[i] = windows[i+1];
        if (windows[i]) windows[i]->id = i; 
    }
    windows[MAX_WINDOWS - 1] = NULL;
    if (was_focused) focus_top_visible();
    return true;
}

static bool desktop_save_all(void) {
    uint64_t now = timer_get_ticks();
    if (g_exit_discard_deadline != 0 &&
        now <= g_exit_discard_deadline) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            Window* w = windows[i];
            if (!w) continue;
            if (w->type == APP_NOTEPAD) {
                w->state.notepad.dirty = false;
                w->state.notepad.discard_deadline = 0;
            }
            if (w->type == APP_PAINT) {
                w->state.paint.dirty = false;
                w->state.paint.discard_deadline = 0;
            }
        }
        g_exit_discard_deadline = 0;
        desktop_log("GUI: unsaved editor changes discarded by repeated exit");
        return true;
    }
    cancel_exit_discard_confirmation();

    bool success = true;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window* w = windows[i];
        if (!w) continue;
        if (w->type == APP_NOTEPAD && w->state.notepad.dirty &&
            !notepad_save(w))
            success = false;
        if (w->type == APP_PAINT && w->state.paint.dirty &&
            !paint_save(w))
            success = false;
    }
    if (!success) {
        g_exit_discard_deadline =
            timer_get_ticks() + DISCARD_CONFIRM_TICKS;
        desktop_notify("Save failed. Exit again within 3s to discard.");
        g_desktop_notice_until = g_exit_discard_deadline;
        desktop_log("GUI: exit blocked because an editor could not save");
    } else {
        g_exit_discard_deadline = 0;
    }
    return success;
}

static int focus_window(int index) {
    if (index < 0 || index >= MAX_WINDOWS || windows[index] == NULL) return -1;
    Window* target = windows[index];
    
    for (int i = index; i < MAX_WINDOWS - 1; i++) {
        windows[i] = windows[i+1];
        if (windows[i]) windows[i]->id = i;
    }
    windows[MAX_WINDOWS - 1] = NULL;
    
    int top_slot = 0;
    while (top_slot < MAX_WINDOWS && windows[top_slot] != NULL) top_slot++;
    if (top_slot >= MAX_WINDOWS) top_slot = MAX_WINDOWS - 1;

    windows[top_slot] = target; 
    target->id = top_slot;

    for(int j=0; j<MAX_WINDOWS; j++) {
        if(windows[j]) {
            windows[j]->focused = (windows[j] == target);
            if (windows[j]->focused) windows[j]->minimized = false;
        }
    }
    return target->id;
}

static Window* get_top_window(void) {
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) { 
        if (windows[i] != NULL && windows[i]->visible && !windows[i]->minimized) return windows[i]; 
    } 
    return NULL;
}

static int taskbar_tab_width(void) {
    int count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i] && windows[i]->visible) count++;
    }
    if (count == 0) return 100;
    int available = (screen_w - 95) - 70 - ((count - 1) * 5);
    int width = available / count;
    if (width > 100) width = 100;
    if (width < 20) width = 20;
    return width;
}

static void assistant_add_line(AssistantState* state, uint8_t role, const char* text) {
    if (state->line_count >= ASSISTANT_MAX_LINES) {
        for (int i = 0; i < ASSISTANT_MAX_LINES - 1; i++) {
            str_copy(state->lines[i], ASSISTANT_LINE_LEN, state->lines[i + 1]);
            state->roles[i] = state->roles[i + 1];
        }
        state->line_count = ASSISTANT_MAX_LINES - 1;
    }

    str_copy(state->lines[state->line_count], ASSISTANT_LINE_LEN, text);
    state->roles[state->line_count] = role;
    state->line_count++;
}

static void assistant_add_wrapped(AssistantState* state, uint8_t role, const char* text) {
    const int max_chars = 40;
    while (*text) {
        while (*text == ' ') text++;
        if (!*text) break;

        int length = 0;
        int last_space = -1;
        while (text[length] && length < max_chars) {
            if (text[length] == ' ') last_space = length;
            length++;
        }
        if (text[length] && last_space > 0) length = last_space;

        char line[ASSISTANT_LINE_LEN];
        int i = 0;
        while (i < length && i + 1 < ASSISTANT_LINE_LEN) {
            line[i] = text[i];
            i++;
        }
        while (i > 0 && line[i - 1] == ' ') i--;
        line[i] = 0;
        assistant_add_line(state, role, line);

        text += length;
        while (*text == ' ') text++;
    }
}

static void assistant_reset(AssistantState* state) {
    state->input[0] = 0;
    state->input_len = 0;
    state->line_count = 0;
    assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
        "Hello! I am AI Assistant, your offline NostaluxOS helper.");
    assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
        "Ask for help, list apps, system info, or say open calculator.");
}

static void minesweeper_reset(MineState* ms) {
    mem_zero(ms, sizeof(*ms));
    int placed = 0;
    while (placed < 15) {
        int r = fast_rand() % MINE_GRID_H;
        int c = fast_rand() % MINE_GRID_W;
        if (ms->grid[r][c] == 9) continue;
        ms->grid[r][c] = 9;
        placed++;
        for (int rr = r - 1; rr <= r + 1; rr++) {
            for (int cc = c - 1; cc <= c + 1; cc++) {
                if (rr >= 0 && rr < MINE_GRID_H &&
                    cc >= 0 && cc < MINE_GRID_W &&
                    ms->grid[rr][cc] != 9) {
                    ms->grid[rr][cc]++;
                }
            }
        }
    }
}

static bool notepad_save(Window* w) {
    if (!w || w->type != APP_NOTEPAD)
        return false;

    NotepadState* state = &w->state.notepad;
    state->discard_deadline = 0;
    cancel_exit_discard_confirmation();
    if (!state->filename[0]) {
        if (!create_unique_file("note", ".txt", state->filename)) {
            str_copy(state->status, sizeof(state->status),
                     "Save failed: no free filename.");
            return false;
        }
        str_copy(w->title, sizeof(w->title), state->filename);
    }
    if (!fs_write_bytes(state->filename, state->buffer, (size_t)state->length)) {
        str_copy(state->status, sizeof(state->status), "Save failed.");
        return false;
    }
    state->dirty = false;
    state->discard_deadline = 0;
    str_copy(state->status, sizeof(state->status),
             fs_backend_is_persistent()
                 ? "Saved to persistent storage."
                 : "Saved for this session only.");
    return true;
}

static bool paint_save(Window* w) {
    if (!w || w->type != APP_PAINT)
        return false;

    uint8_t encoded[FS_MAX_FILE_SIZE];
    size_t encoded_size = 0;
    PaintState* state = &w->state.paint;
    state->discard_deadline = 0;
    cancel_exit_discard_confirmation();
    if (!state->filename[0]) {
        if (!create_unique_file("painting", ".bmp", state->filename)) {
            str_copy(state->status, sizeof(state->status),
                     "Save failed: no free filename.");
            return false;
        }
        str_copy(w->title, sizeof(w->title), state->filename);
    }
    if (!bmp_encode_rgb24(state->pixels, PAINT_CANVAS_W, PAINT_CANVAS_H,
                          PAINT_CANVAS_W, encoded, sizeof(encoded),
                          &encoded_size) ||
        !fs_write_bytes(state->filename, encoded, encoded_size)) {
        str_copy(state->status, sizeof(state->status), "BMP save failed.");
        return false;
    }
    state->dirty = false;
    state->discard_deadline = 0;
    str_copy(state->status, sizeof(state->status),
             fs_backend_is_persistent()
                 ? "Saved persistent 17x17 BMP."
                 : "Saved 17x17 BMP for this session.");
    return true;
}

static bool paint_reload(Window* w) {
    if (!w || w->type != APP_PAINT || !w->state.paint.filename[0])
        return false;

    PaintState* state = &w->state.paint;
    const struct fs_file* file = fs_find(state->filename);
    struct bmp_image image;
    if (!file || !bmp_open(file->data, file->size, &image) ||
        image.width != PAINT_CANVAS_W ||
        image.height != PAINT_CANVAS_H) {
        state->filename[0] = 0;
        state->dirty = false;
        state->discard_deadline = 0;
        str_copy(w->title, sizeof(w->title), "Changed painting");
        str_copy(state->status, sizeof(state->status),
                 "Backing file changed; paint to save a new BMP.");
        return false;
    }

    for (uint32_t y = 0; y < image.height; y++) {
        for (uint32_t x = 0; x < image.width; x++) {
            uint32_t color = COL_WHITE;
            if (!bmp_get_pixel(&image, x, y, &color)) return false;
            state->pixels[y * PAINT_CANVAS_W + x] = color;
        }
    }
    state->dirty = false;
    state->discard_deadline = 0;
    str_copy(state->status, sizeof(state->status),
             "Reloaded from filesystem.");
    return true;
}

static Window* create_window(AppType type, const char* title, int w, int h) {
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) { if (windows[i] == NULL) { slot = i; break; } }

    Window* win = (Window*)desktop_malloc(sizeof(Window));
    if (!win) {
        desktop_notify("Not enough memory to open another window.");
        desktop_log("GUI: window allocation failed");
        return NULL;
    }

    if (slot == -1) {
        desktop_free(win);
        desktop_notify("Window limit reached; close a window first.");
        desktop_log("GUI: window creation refused at window limit");
        return NULL;
    }

    mem_zero(win, sizeof(Window));

    win->id = slot;
    win->type = type;
    str_copy(win->title, sizeof(win->title), title);
    win->w = w; win->h = h; win->min_w = 150; win->min_h = 100;
    win->visible = true; win->focused = true;
    win->minimized = false; win->maximized = false;
    win->dragging = false; win->resizing = false;

    // Initialize Apps
    if (type == APP_PAINT) {
        win->min_w = 330; win->min_h = 220;
        for (int i = 0; i < PAINT_CANVAS_W * PAINT_CANVAS_H; i++)
            win->state.paint.pixels[i] = COL_WHITE;
        win->state.paint.current_color = COL_BLACK;
        if (create_unique_file("painting", ".bmp", win->state.paint.filename)) {
            str_copy(win->title, sizeof(win->title), win->state.paint.filename);
            win->state.paint.dirty = true;
            paint_save(win);
        } else {
            str_copy(win->state.paint.status,
                     sizeof(win->state.paint.status), "No free filename.");
        }
    } else if (type == APP_TICTACTOE) {
        win->min_w = 220; win->min_h = 280;
        for(int r=0;r<3;r++) for(int c=0;c<3;c++) win->state.ttt.board[r][c] = 0;
        win->state.ttt.turn = 1; // X starts
        win->state.ttt.winner = 0;
    } else if (type == APP_IMAGEVIEW) {
        win->min_w = 320; win->min_h = 240;
        win->state.img.file_index = -1;
        win->state.img.fit_to_window = true;
        imageview_load(win, 1);
    } else if (type == APP_SYSMON) {
        win->min_w = 340; win->min_h = 240;
        for(int i=0; i<SYSMON_HIST; i++) {
            win->state.sysmon.cpu_hist[i] = 0;
            win->state.sysmon.mem_hist[i] = 0;
        }
        win->state.sysmon.head = 0;
        win->state.sysmon.last_sample_tick = 0;
        win->state.sysmon.has_previous_cpu = false;
        update_sysmon(win);
    } else if (type == APP_MINESWEEPER) {
        win->min_w = 220; win->min_h = 260;
        minesweeper_reset(&win->state.mine);
    } else if (type == APP_RUN) {
        win->min_w = 300; win->min_h = 140;
        win->state.run.cmd[0] = 0; win->state.run.len = 0;
    } else if (type == APP_TERMINAL) {
        win->min_w = 300; win->min_h = 150;
        str_copy(win->state.term.lines[0], GUI_TERM_LINE_LEN,
                 "Real shell commands are available. Type help.");
        win->state.term.line_count = 1;
    } else if (type == APP_SETTINGS) {
        win->min_w = 240; win->min_h = 210;
        win->state.settings.wallpaper_enabled = g_wallpaper_enabled;
        win->state.settings.theme_id = current_theme_idx;
        str_copy(win->state.settings.status,
                  sizeof(win->state.settings.status),
                  "Change wallpaper or theme.");
    } else if (type == APP_BROWSER) {
        win->min_w = 360; win->min_h = 180;
        str_copy(win->state.browser.address,
                 sizeof(win->state.browser.address), "about:home");
        win->state.browser.address_len = kstrlen_local("about:home");
        win->state.browser.address_select_all = true;
        browser_load(win);
    } else if (type == APP_TASKMGR) {
        win->min_w = 380; win->min_h = 240;
        win->state.taskmgr.selected_pid = 0;
        win->state.taskmgr.page_offset = 0;
        str_copy(win->state.taskmgr.status,
                  sizeof(win->state.taskmgr.status),
                  "Showing genuine scheduler tasks and isolated apps.");
    } else if (type == APP_FILES) {
        win->min_w = 310; win->min_h = 180;
        win->state.files.selected_index = -1;
        win->state.files.scroll_offset = 0;
        str_copy(win->state.files.status,
                  sizeof(win->state.files.status), "Ready.");
    } else if (type == APP_NOTEPAD) {
        win->min_w = 260; win->min_h = 180;
        str_copy(win->state.notepad.status,
                 sizeof(win->state.notepad.status), "No file loaded.");
    } else if (type == APP_CALC) {
        win->min_w = 190; win->min_h = 210;
        win->state.calc.new_entry = true;
    } else if (type == APP_WELCOME) {
        win->min_w = 420; win->min_h = 200;
    } else if (type == APP_ABOUT) {
        win->min_w = 320; win->min_h = 250;
        win->state.about.scroll_y = 0;
    } else if (type == APP_ASSISTANT) {
        win->min_w = 420; win->min_h = 300;
        assistant_reset(&win->state.assistant);
    }

    if (win->w < win->min_w) win->w = win->min_w;
    if (win->h < win->min_h) win->h = win->min_h;
    if (win->w > screen_w) win->w = screen_w;
    if (win->h > screen_h - TASKBAR_H) win->h = screen_h - TASKBAR_H;
    win->x = 40 + (slot * 20);
    win->y = 40 + (slot * 20);
    if (win->x + win->w > screen_w) win->x = 20;
    if (win->y + win->h > screen_h - TASKBAR_H) win->y = 20;

    windows[slot] = win;
    focus_window(slot);
    return win;
}

static Window* open_notepad_file(const char* filename) {
    const struct fs_file* file = fs_find(filename);
    if (!file || !file_is_text(file)) return NULL;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window* existing = windows[i];
        if (existing && existing->type == APP_NOTEPAD &&
            kstrcmp(existing->state.notepad.filename, filename) == 0) {
            existing->minimized = false;
            int focused_index = focus_window(i);
            return focused_index >= 0 ? windows[focused_index] : existing;
        }
    }

    Window* win = create_window(APP_NOTEPAD, filename, 440, 320);
    if (!win) return NULL;
    NotepadState* state = &win->state.notepad;
    size_t length = file->size;
    if (length >= sizeof(state->buffer)) length = sizeof(state->buffer) - 1;
    for (size_t i = 0; i < length; i++) state->buffer[i] = file->data[i];
    state->buffer[length] = 0;
    state->length = (int)length;
    str_copy(state->filename, sizeof(state->filename), filename);
    str_copy(state->status, sizeof(state->status), "Opened from filesystem.");
    state->dirty = false;
    return win;
}

static Window* create_notepad_file(void) {
    char filename[FS_MAX_FILENAME];
    if (!create_unique_file("note", ".txt", filename)) {
        desktop_notify("Could not create a note on the filesystem.");
        return NULL;
    }
    Window* note = open_notepad_file(filename);
    if (!note) {
        if (!fs_remove(filename))
            desktop_log("GUI: failed to roll back unopened note file");
        return NULL;
    }
    return note;
}

static Window* open_image_file(const char* filename) {
    const struct fs_file* file = fs_find(filename);
    struct bmp_image image;
    if (!file || !bmp_open(file->data, file->size, &image))
        return NULL;

    Window* win = create_window(APP_IMAGEVIEW, "Image Viewer", 440, 360);
    if (!win) return NULL;
    if (!imageview_open_named(win, filename)) {
        close_window(win->id);
        return NULL;
    }
    str_copy(win->title, sizeof(win->title), filename);
    return win;
}

static Window* open_browser_file(const char* filename) {
    char address[FS_MAX_FILENAME + 6] = "file:";
    str_append(address, sizeof(address), filename);

    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window* existing = windows[i];
        if (existing && existing->type == APP_BROWSER &&
            kstrcmp(existing->state.browser.address, address) == 0) {
            existing->minimized = false;
            browser_load(existing);
            int focused_index = focus_window(i);
            return focused_index >= 0 ? windows[focused_index] : existing;
        }
    }

    Window* win = create_window(APP_BROWSER, "Local Browser", 500, 400);
    if (!win) return NULL;
    str_copy(win->state.browser.address,
             sizeof(win->state.browser.address), address);
    win->state.browser.address_len =
        kstrlen_local(win->state.browser.address);
    win->state.browser.address_select_all = false;
    browser_load(win);
    return win;
}

static bool launch_app(AppType type) {
    Window* opened = NULL;
    switch (type) {
        case APP_ASSISTANT:
            opened = create_window(APP_ASSISTANT, "AI Assistant", 520, 380);
            break;
        case APP_BROWSER:
            opened = create_window(APP_BROWSER, "Local Browser", 500, 400);
            break;
        case APP_TERMINAL:
            opened = create_window(APP_TERMINAL, "Terminal", 400, 300);
            break;
        case APP_PAINT:
            opened = create_window(APP_PAINT, "Paint", 500, 400);
            break;
        case APP_FILES:
            opened = create_window(APP_FILES, "Files", 420, 320);
            break;
        case APP_TASKMGR:
            opened = create_window(
                APP_TASKMGR, "Kernel Task Manager", 460, 320);
            break;
        case APP_NOTEPAD:
            opened = create_notepad_file();
            break;
        case APP_CALC:
            opened = create_window(APP_CALC, "Calculator", 220, 300);
            break;
        case APP_MINESWEEPER:
            opened = create_window(APP_MINESWEEPER, "Minesweeper", 220, 260);
            break;
        case APP_TICTACTOE:
            opened = create_window(APP_TICTACTOE, "Tic-Tac-Toe", 220, 280);
            break;
        case APP_IMAGEVIEW:
            opened = create_window(APP_IMAGEVIEW, "Image Viewer", 420, 340);
            break;
        case APP_SYSMON:
            opened = create_window(APP_SYSMON, "System Monitor", 420, 300);
            break;
        case APP_SETTINGS:
            opened = create_window(APP_SETTINGS, "Settings", 360, 240);
            break;
        case APP_RUN:
            opened = create_window(APP_RUN, "Run", 360, 140);
            break;
        case APP_ABOUT:
            opened = create_window(APP_ABOUT, "About Nostalux", 340, 260);
            break;
        case APP_WELCOME:
            opened = create_window(APP_WELCOME, "Welcome", 420, 200);
            break;
        default:
            break;
    }
    return opened != NULL;
}

// --- Logic ---

static void toggle_maximize(Window* w) {
    if (w->maximized) { 
        w->x = w->restore_x; w->y = w->restore_y; 
        w->w = w->restore_w; w->h = w->restore_h; 
        w->maximized = false; 
    } else { 
        w->restore_x = w->x; w->restore_y = w->y; 
        w->restore_w = w->w; w->restore_h = w->h; 
        w->x = 0; w->y = 0; w->w = screen_w; w->h = screen_h - TASKBAR_H; 
        w->maximized = true; 
    }
}

static void handle_minesweeper(Window* w, int x, int y, bool right_click) {
    MineState* ms = &w->state.mine;
    if (ms->game_over || ms->victory) {
        if (!right_click) minesweeper_reset(ms);
        return;
    }

    int grid_x = w->x + (w->w - MINE_GRID_PIXELS) / 2;
    int grid_y = w->y + WIN_CAPTION_H + 34;
    if (!rect_contains(grid_x, grid_y, MINE_GRID_PIXELS, MINE_GRID_PIXELS, x, y)) return;

    int c = (x - grid_x) / MINE_CELL_SIZE;
    int r = (y - grid_y) / MINE_CELL_SIZE;

    if (right_click) {
        if (ms->view[r][c] == 0) { ms->view[r][c] = 2; ms->flags_placed++; }
        else if (ms->view[r][c] == 2) { ms->view[r][c] = 0; ms->flags_placed--; }
        return;
    }

    if (ms->view[r][c] != 0) return;
    if (ms->grid[r][c] == 9) {
        ms->game_over = true;
        for (int rr = 0; rr < MINE_GRID_H; rr++) {
            for (int cc = 0; cc < MINE_GRID_W; cc++) {
                if (ms->grid[rr][cc] == 9) ms->view[rr][cc] = 1;
            }
        }
        return;
    }

    int queue[MINE_GRID_W * MINE_GRID_H];
    int head = 0;
    int tail = 0;
    queue[tail++] = r * MINE_GRID_W + c;
    while (head < tail) {
        int cell = queue[head++];
        int rr = cell / MINE_GRID_W;
        int cc = cell % MINE_GRID_W;
        if (ms->view[rr][cc] != 0 || ms->grid[rr][cc] == 9) continue;
        ms->view[rr][cc] = 1;
        if (ms->grid[rr][cc] != 0) continue;
        for (int nr = rr - 1; nr <= rr + 1; nr++) {
            for (int nc = cc - 1; nc <= cc + 1; nc++) {
                if (nr >= 0 && nr < MINE_GRID_H &&
                    nc >= 0 && nc < MINE_GRID_W &&
                    ms->view[nr][nc] == 0 &&
                    ms->grid[nr][nc] != 9 &&
                    tail < MINE_GRID_W * MINE_GRID_H) {
                    bool already_queued = false;
                    int queued_cell = nr * MINE_GRID_W + nc;
                    for (int q = head; q < tail; q++) {
                        if (queue[q] == queued_cell) {
                            already_queued = true;
                            break;
                        }
                    }
                    if (!already_queued) queue[tail++] = queued_cell;
                }
            }
        }
    }

    int safe_hidden = 0;
    for (int rr = 0; rr < MINE_GRID_H; rr++) {
        for (int cc = 0; cc < MINE_GRID_W; cc++) {
            if (ms->grid[rr][cc] != 9 && ms->view[rr][cc] != 1) safe_hidden++;
        }
    }
    if (safe_hidden == 0) ms->victory = true;
}

static void handle_tictactoe(Window* w, int x, int y) {
    TicTacToeState* s = &w->state.ttt;
    int cx = w->x + 10;
    int cy = w->y + WIN_CAPTION_H + 10;
    
    // Check restart button
    if (s->winner != 0 && rect_contains(cx + 10, cy + 205, 100, 24, x, y)) {
        for(int i=0;i<3;i++) for(int j=0;j<3;j++) s->board[i][j]=0;
        s->turn = 1; s->winner = 0;
        return;
    }
    
    if (s->winner != 0) return;
    
    // Grid hit test
    for (int r=0; r<3; r++) {
        for (int c=0; c<3; c++) {
            int bx = cx + c*60;
            int by = cy + r*60;
            if (rect_contains(bx, by, 55, 55, x, y)) {
                if (s->board[r][c] == 0) {
                    s->board[r][c] = s->turn;
                    // Check win
                    bool win = false;
                    // Row/Col
                    for(int k=0;k<3;k++) {
                        if (s->board[r][k] != s->turn) break;
                        if (k==2) win=true;
                    }
                    if(!win) for(int k=0;k<3;k++) {
                        if (s->board[k][c] != s->turn) break;
                        if (k==2) win=true;
                    }
                    // Diag
                    if(!win && r==c) {
                        if(s->board[0][0]==s->turn && s->board[1][1]==s->turn && s->board[2][2]==s->turn) win=true;
                    }
                    if(!win && r+c==2) {
                        if(s->board[0][2]==s->turn && s->board[1][1]==s->turn && s->board[2][0]==s->turn) win=true;
                    }
                    
                    if(win) s->winner = s->turn;
                    else {
                        // Check draw
                        bool full=true;
                        for(int i=0;i<3;i++) for(int j=0;j<3;j++) if(s->board[i][j]==0) full=false;
                        if(full) s->winner=3;
                        else s->turn = (s->turn==1) ? 2 : 1;
                    }
                }
            }
        }
    }
}

static void browser_set_content(BrowserState* state, const char* text) {
    str_copy(state->content, sizeof(state->content), text ? text : "");
    state->content_len = kstrlen_local(state->content);
}

static bool browser_append_content(BrowserState* state, const char* text) {
    bool complete =
        str_append(state->content, sizeof(state->content), text ? text : "");
    state->content_len = kstrlen_local(state->content);
    return complete;
}

static int browser_max_columns(const Window* w) {
    int content_width = w->w - 4;
    int columns = (content_width - 20) / 8;
    return columns < 1 ? 1 : columns;
}

static int browser_visible_rows(const Window* w) {
    int content_height = w->h - WIN_CAPTION_H - 4;
    int browser_content_height = content_height - 60;
    int rows = (browser_content_height - 12) / 10;
    return rows < 1 ? 1 : rows;
}

static int browser_max_scroll(const Window* w, const BrowserState* state) {
    int logical_rows =
        wrapped_content_line_count(state->content, browser_max_columns(w));
    int visible_rows = browser_visible_rows(w);
    return logical_rows > visible_rows ? logical_rows - visible_rows : 0;
}

static void browser_load(Window* w) {
    BrowserState* state = &w->state.browser;
    const char* address = state->address;
    state->scroll = 0;
    state->content[0] = 0;
    state->content_len = 0;
    state->last_refresh_tick = timer_get_ticks();

    if (kstrcmp(address, "about:home") == 0 || address[0] == 0) {
        browser_set_content(state,
            "Nostalux Local Browser\n\n"
            "This browser opens real files from the Nostalux filesystem.\n"
            "Try file:readme.txt, file:motd.txt, about:files, or about:system.\n"
            "System, file-index, and system.log pages refresh automatically.\n"
            "Use Refresh to reload any page immediately.\n"
            "Internet URLs are rejected because no network driver is installed.");
        str_copy(state->status, sizeof(state->status), "Built-in home page.");
        return;
    }

    if (kstrcmp(address, "about:files") == 0) {
        browser_set_content(state, "Filesystem files\n\n");
        browser_append_content(state, "Storage: ");
        browser_append_content(state, fs_backend_status_text());
        browser_append_content(state, "\n\n");
        size_t count = fs_file_count();
        bool complete = true;
        for (size_t i = 0; i < count; i++) {
            const struct fs_file* file = fs_file_at(i);
            if (!file) continue;
            int required = 5 + kstrlen_local(file->name) + 1;
            int remaining = (int)sizeof(state->content) -
                            kstrlen_local(state->content) - 1;
            if (remaining < required + 32) {
                browser_append_content(
                    state, "[Additional files omitted.]\n");
                complete = false;
                break;
            }
            browser_append_content(state, "file:");
            browser_append_content(state, file->name);
            browser_append_content(state, "\n");
        }
        str_copy(state->status, sizeof(state->status),
                 complete ? "Auto-refreshing filesystem index."
                          : "Auto-refreshing shortened index.");
        return;
    }

    if (kstrcmp(address, "about:system") == 0) {
        const struct BootInfo* boot = system_boot_info();
        const struct system_profile* profile = system_profile_info();
        char number[24];
        browser_set_content(state, "NostaluxOS system page\n\nResolution: ");
        int_to_str((int)boot->width, number);
        browser_append_content(state, number);
        browser_append_content(state, "x");
        int_to_str((int)boot->height, number);
        browser_append_content(state, number);
        browser_append_content(state, "\nPhysical usable RAM: ");
        uint64_to_str(profile->memory_total_kb, number);
        browser_append_content(state, number);
        browser_append_content(state, " KB\nMapped usable RAM: ");
        uint64_to_str(profile->memory_mapped_kb, number);
        browser_append_content(state, number);
        browser_append_content(state, " KB\nKernel committed RAM: ");
        uint64_to_str(profile->memory_used_kb, number);
        browser_append_content(state, number);
        browser_append_content(state, " KB\n  Reserved low RAM: ");
        uint64_to_str(profile->memory_reserved_kb, number);
        browser_append_content(state, number);
        browser_append_content(state, " KB\n  Heap committed: ");
        uint64_to_str(profile->memory_heap_committed_kb, number);
        browser_append_content(state, number);
        browser_append_content(state, " KB\nUptime: ");
        uint64_to_str(timer_get_uptime(), number);
        browser_append_content(state, number);
        browser_append_content(state, " seconds\nStorage: ");
        browser_append_content(state, fs_backend_status_text());
        browser_append_content(state, "\n");
        str_copy(state->status, sizeof(state->status),
                 "Auto-refreshing kernel information.");
        return;
    }

    if (str_starts_with(address, "http:") ||
        str_starts_with(address, "https:") ||
        text_contains_ci(address, "://")) {
        browser_set_content(state,
            "Internet access is unavailable.\n\n"
            "NostaluxOS does not include a network driver or TCP/IP stack yet.\n"
            "Use file:<name> to open a real filesystem document.");
        str_copy(state->status, sizeof(state->status), "Network URL rejected honestly.");
        return;
    }

    const char* filename = str_starts_with(address, "file:")
                         ? address + 5 : address;
    const struct fs_file* file = fs_find(filename);
    if (!file) {
        browser_set_content(state, "The requested filesystem file does not exist.");
        str_copy(state->status, sizeof(state->status), "File not found.");
        return;
    }
    if (!file_is_text(file)) {
        browser_set_content(state,
            "This is a binary file.\n\n"
            "Open BMP images in Image Viewer, or use hexdump in Terminal.");
        str_copy(state->status, sizeof(state->status), "Binary file identified.");
        return;
    }

    size_t length = file->size;
    if (length >= sizeof(state->content)) length = sizeof(state->content) - 1;
    for (size_t i = 0; i < length; i++) state->content[i] = file->data[i];
    state->content[length] = 0;
    state->content_len = (int)length;
    char status[64] = "Opened ";
    char number[24];
    uint64_to_str(file->size, number);
    str_append(status, sizeof(status), number);
    str_append(status, sizeof(status), " filesystem bytes.");
    str_copy(state->status, sizeof(state->status), status);
}

static bool filename_is_bmp(const char* name) {
    return str_ends_with_ci(name, ".bmp");
}

static bool imageview_open_named(Window* w, const char* name) {
    ImageViewState* state = &w->state.img;
    int index = file_index_by_name(name);
    const struct fs_file* file =
        index >= 0 ? fs_file_at((size_t)index) : NULL;
    if (!file) return false;

    state->file_index = index;
    state->fit_to_window = true;
    state->valid = false;
    mem_zero(&state->image, sizeof(state->image));
    str_copy(state->filename, sizeof(state->filename), file->name);
    if (bmp_open(file->data, file->size, &state->image)) {
        state->valid = true;
        str_copy(state->status, sizeof(state->status),
                 "Loaded from the filesystem.");
    } else {
        str_copy(state->status, sizeof(state->status),
                 "Invalid or unsupported 24-bit BMP.");
    }
    return state->valid;
}

static void imageview_load(Window* w, int direction) {
    ImageViewState* state = &w->state.img;
    int count = (int)fs_file_count();
    int start = state->file_index;

    state->valid = false;
    mem_zero(&state->image, sizeof(state->image));
    if (count <= 0) {
        state->file_index = -1;
        state->filename[0] = 0;
        str_copy(state->status, sizeof(state->status), "No BMP files in the filesystem.");
        return;
    }

    for (int offset = 0; offset < count; offset++) {
        int index;
        if (start < 0) {
            index = direction < 0 ? count - 1 - offset : offset;
        } else {
            int step = offset + 1;
            index = direction < 0 ? start - step : start + step;
            while (index < 0) index += count;
            index %= count;
        }

        const struct fs_file* file = fs_file_at((size_t)index);
        if (!file || !filename_is_bmp(file->name)) continue;

        imageview_open_named(w, file->name);
        return;
    }

    state->file_index = -1;
    state->filename[0] = 0;
    str_copy(state->status, sizeof(state->status), "No BMP files in the filesystem.");
}

static void handle_imageview(Window* w, int x, int y) {
    int cx = w->x + 2;
    int cy = w->y + WIN_CAPTION_H + 2;
    int cw = w->w - 4;
    int ch = w->h - WIN_CAPTION_H - 4;
    int button_y = cy + ch - 30;

    if (rect_contains(cx + 8, button_y, 58, 22, x, y)) {
        imageview_load(w, -1);
    } else if (rect_contains(cx + 72, button_y, 58, 22, x, y)) {
        imageview_load(w, 1);
    } else if (rect_contains(cx + 136, button_y, 70, 22, x, y)) {
        w->state.img.fit_to_window = !w->state.img.fit_to_window;
    } else if (rect_contains(cx + 8, cy + 24, cw - 16, ch - 62, x, y)) {
        w->state.img.fit_to_window = !w->state.img.fit_to_window;
    }
}

static AppType assistant_requested_app(const char* prompt) {
    if (text_contains_ci(prompt, "assistant") || text_contains_ci(prompt, " ai")) return APP_ASSISTANT;
    if (text_contains_ci(prompt, "calculator") || text_contains_ci(prompt, "calc")) return APP_CALC;
    if (text_contains_ci(prompt, "terminal") || text_contains_ci(prompt, "console")) return APP_TERMINAL;
    if (text_contains_ci(prompt, "notepad") || text_contains_ci(prompt, "notes")) return APP_NOTEPAD;
    if (text_contains_ci(prompt, "file manager") || text_contains_ci(prompt, "files")) return APP_FILES;
    if (text_contains_ci(prompt, "paint") || text_contains_ci(prompt, "drawing")) return APP_PAINT;
    if (text_contains_ci(prompt, "browser") || text_contains_ci(prompt, "web")) return APP_BROWSER;
    if (text_contains_ci(prompt, "task manager")) return APP_TASKMGR;
    if (text_contains_ci(prompt, "system monitor") || text_contains_ci(prompt, "sysmon")) return APP_SYSMON;
    if (text_contains_ci(prompt, "settings")) return APP_SETTINGS;
    if (text_contains_ci(prompt, "minesweeper") || text_contains_ci(prompt, "mines")) return APP_MINESWEEPER;
    if (text_contains_ci(prompt, "tic-tac-toe") || text_contains_ci(prompt, "tic tac toe")) return APP_TICTACTOE;
    if (text_contains_ci(prompt, "image viewer") || text_contains_ci(prompt, "images")) return APP_IMAGEVIEW;
    if (text_contains_ci(prompt, "about")) return APP_ABOUT;
    return APP_NONE;
}

static const char* assistant_app_name(AppType type) {
    switch (type) {
        case APP_ASSISTANT: return "AI Assistant";
        case APP_CALC: return "Calculator";
        case APP_TERMINAL: return "Terminal";
        case APP_NOTEPAD: return "Notepad";
        case APP_FILES: return "Files";
        case APP_PAINT: return "Paint";
        case APP_BROWSER: return "Local Browser";
        case APP_TASKMGR: return "Task Manager";
        case APP_SYSMON: return "System Monitor";
        case APP_SETTINGS: return "Settings";
        case APP_MINESWEEPER: return "Minesweeper";
        case APP_TICTACTOE: return "Tic-Tac-Toe";
        case APP_IMAGEVIEW: return "Image Viewer";
        case APP_ABOUT: return "About";
        default: return "that app";
    }
}

static void assistant_submit(Window* w) {
    AssistantState* state = &w->state.assistant;
    if (state->input_len == 0) return;

    char prompt[ASSISTANT_LINE_LEN];
    str_copy(prompt, sizeof(prompt), state->input);
    assistant_add_wrapped(state, ASSISTANT_ROLE_USER, prompt);
    state->input[0] = 0;
    state->input_len = 0;

    bool wants_open = text_contains_ci(prompt, "open ") ||
                      text_contains_ci(prompt, "launch ") ||
                      text_contains_ci(prompt, "start ");
    if (wants_open) {
        AppType requested = assistant_requested_app(prompt);
        if (requested == APP_ASSISTANT) {
            assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
                "You are already talking to AI Assistant.");
        } else if (requested != APP_NONE) {
            bool opened = launch_app(requested);
            char reply[64];
            str_copy(reply, sizeof(reply),
                     opened ? "Opened " : "I could not open ");
            str_append(reply, sizeof(reply),
                       assistant_app_name(requested));
            str_append(reply, sizeof(reply),
                       opened ? "." : "; check the desktop notice.");
            assistant_add_wrapped(state, ASSISTANT_ROLE_AI, reply);
        } else {
            assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
                "I could not match that app. Say list apps to see my launchers.");
        }
        return;
    }

    if (text_contains_ci(prompt, "hello") || text_equals_ci(prompt, "hi") ||
        text_contains_ci(prompt, " hi") ||
        text_contains_ci(prompt, "hey")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "Hello! What would you like to do in NostaluxOS?");
    } else if (text_contains_ci(prompt, "thank")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "You are welcome. I am here whenever you need OS help.");
    } else if (text_contains_ci(prompt, "who are you") || text_contains_ci(prompt, "your name") ||
               text_contains_ci(prompt, "what are you")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "I am AI Assistant, a small offline intent-matching helper built into NostaluxOS.");
    } else if (text_contains_ci(prompt, "what can") || text_contains_ci(prompt, "help") ||
               text_contains_ci(prompt, "how do i")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "I can explain the OS, list apps and shell commands, report time or system info, and open apps.");
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "Try: list apps, system info, shell commands, or open paint.");
    } else if (text_contains_ci(prompt, "list apps") || text_contains_ci(prompt, "applications") ||
               text_contains_ci(prompt, "installed apps")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "Apps: Terminal, Files, Paint, Local Browser, Notepad, Calculator, Settings, monitors, games, and Image Viewer.");
    } else if (text_contains_ci(prompt, "time") || text_contains_ci(prompt, "clock")) {
        char now[16];
        char reply[48] = "The system clock says ";
        desktop_get_time(now);
        str_append(reply, sizeof(reply), now);
        str_append(reply, sizeof(reply), ".");
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI, reply);
    } else if (text_contains_ci(prompt, "system") || text_contains_ci(prompt, "memory") ||
               text_contains_ci(prompt, "resolution") || text_contains_ci(prompt, "operating system")) {
        const struct BootInfo* boot = system_boot_info();
        const struct system_profile* profile = system_profile_info();
        char details[64] = "x86-64 NostaluxOS | ";
        char number[16];
        int_to_str((int)boot->width, number);
        str_append(details, sizeof(details), number);
        str_append(details, sizeof(details), "x");
        int_to_str((int)boot->height, number);
        str_append(details, sizeof(details), number);
        str_append(details, sizeof(details), " | RAM ");
        uint64_to_str(profile->memory_total_kb / 1024u, number);
        str_append(details, sizeof(details), number);
        str_append(details, sizeof(details), " MB");
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI, details);
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "The OS is a freestanding hobby kernel with its own shell, GUI, hybrid scheduler, and filesystem.");
        char storage_reply[64] = "Current storage: ";
        str_append(storage_reply, sizeof(storage_reply), storage_short_label());
        str_append(storage_reply, sizeof(storage_reply), ".");
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI, storage_reply);
    } else if (text_contains_ci(prompt, "command") || text_contains_ci(prompt, "shell") ||
               text_contains_ci(prompt, "console")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "At the boot shell, type help. Useful commands include gui, ls, cat, write, calc, time, sysinfo, snake, and shutdown.");
    } else if (text_contains_ci(prompt, "file") || text_contains_ci(prompt, "save")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "Use Files to browse. Shell commands ls, cat, touch, write, append, and rm manage the flat filesystem.");
        assistant_add_wrapped(
            state, ASSISTANT_ROLE_AI,
            fs_backend_is_persistent()
                ? "Storage is persistent on the ATA disk."
                : "Storage is volatile; changes last for this session only.");
    } else if (text_contains_ci(prompt, "internet") || text_contains_ci(prompt, "online") ||
               text_contains_ci(prompt, "network")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "NostaluxOS has no network stack yet. Local Browser opens real filesystem and built-in pages offline.");
    } else if (text_contains_ci(prompt, "joke")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "Why did the kernel stay calm? It had complete control of its processes.");
    } else {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "I do not know that yet. Try asking for help, apps, commands, files, time, or system info.");
    }
}

static void handle_assistant_input(Window* w, char c) {
    AssistantState* state = &w->state.assistant;
    if (c == '\n') {
        assistant_submit(w);
    } else if (c == '\b') {
        if (state->input_len > 0) state->input[--state->input_len] = 0;
    } else if (c >= 32 && c <= 126 && state->input_len < (int)sizeof(state->input) - 1) {
        state->input[state->input_len++] = c;
        state->input[state->input_len] = 0;
    }
}

static void handle_assistant_click(Window* w, int x, int y) {
    int cx = w->x + 2;
    int cy = w->y + WIN_CAPTION_H + 2;
    int cw = w->w - 4;
    int ch = w->h - WIN_CAPTION_H - 4;

    if (rect_contains(cx + cw - 66, cy + 10, 54, 22, x, y)) {
        assistant_reset(&w->state.assistant);
    } else if (rect_contains(cx + cw - 70, cy + ch - 38, 60, 28, x, y)) {
        assistant_submit(w);
    }
}

static void handle_run_command(Window* w) {
    char command[sizeof(w->state.run.cmd)];
    str_copy(command, sizeof(command), w->state.run.cmd);
    if (text_equals_ci_trimmed(command, "")) {
        str_copy(w->state.run.status, sizeof(w->state.run.status),
                 "Enter an app name or shell command.");
        return;
    }

    AppType target = APP_NONE;
    if (text_equals_ci_trimmed(command, "calc") ||
        text_equals_ci_trimmed(command, "calculator"))
        target = APP_CALC;
    else if (text_equals_ci_trimmed(command, "term") ||
             text_equals_ci_trimmed(command, "terminal"))
        target = APP_TERMINAL;
    else if (text_equals_ci_trimmed(command, "paint"))
        target = APP_PAINT;
    else if (text_equals_ci_trimmed(command, "files") ||
             text_equals_ci_trimmed(command, "file manager"))
        target = APP_FILES;
    else if (text_equals_ci_trimmed(command, "notepad"))
        target = APP_NOTEPAD;
    else if (text_equals_ci_trimmed(command, "settings"))
        target = APP_SETTINGS;
    else if (text_equals_ci_trimmed(command, "tasks") ||
             text_equals_ci_trimmed(command, "task manager") ||
             text_equals_ci_trimmed(command, "taskmgr"))
        target = APP_TASKMGR;
    else if (text_equals_ci_trimmed(command, "sys") ||
             text_equals_ci_trimmed(command, "sysmon") ||
             text_equals_ci_trimmed(command, "system monitor"))
        target = APP_SYSMON;
    else if (text_equals_ci_trimmed(command, "mine") ||
             text_equals_ci_trimmed(command, "minesweeper"))
        target = APP_MINESWEEPER;
    else if (text_equals_ci_trimmed(command, "browser") ||
             text_equals_ci_trimmed(command, "local browser"))
        target = APP_BROWSER;
    else if (text_equals_ci_trimmed(command, "ttt") ||
             text_equals_ci_trimmed(command, "tic-tac-toe") ||
             text_equals_ci_trimmed(command, "tic tac toe"))
        target = APP_TICTACTOE;
    else if (text_equals_ci_trimmed(command, "img") ||
             text_equals_ci_trimmed(command, "images") ||
             text_equals_ci_trimmed(command, "image viewer"))
        target = APP_IMAGEVIEW;
    else if (text_equals_ci_trimmed(command, "about"))
        target = APP_ABOUT;
    else if (text_equals_ci_trimmed(command, "ai") ||
             text_equals_ci_trimmed(command, "assistant") ||
             text_equals_ci_trimmed(command, "ai assistant"))
        target = APP_ASSISTANT;

    int run_id = w->id;
    if (!close_window(run_id)) return;
    if (target != APP_NONE) {
        launch_app(target);
        return;
    }

    Window* terminal = create_window(APP_TERMINAL, "Terminal", 480, 340);
    if (terminal) {
        str_copy(terminal->state.term.input,
                 sizeof(terminal->state.term.input), command);
        terminal->state.term.input_len =
            kstrlen_local(terminal->state.term.input);
        terminal_execute(terminal);
    } else {
        desktop_notify("Could not open Terminal for the Run command.");
    }
}

static void paint_canvas_rect(Window* w, int* x, int* y, int* width, int* height) {
    int available_w = w->w - 16;
    int available_h = w->h - WIN_CAPTION_H - 60;
    int side = available_w < available_h ? available_w : available_h;
    if (side < PAINT_CANVAS_W) side = PAINT_CANVAS_W;
    *width = side;
    *height = side;
    *x = w->x + (w->w - side) / 2;
    *y = w->y + WIN_CAPTION_H + 48;
}

static void handle_paint_click(Window* w, int x, int y) {
    int toolbar_y = w->y + WIN_CAPTION_H + 11;
    if (y >= toolbar_y && y < toolbar_y + 25) {
        if (rect_contains(w->x + w->w - 70, toolbar_y, 58, 25, x, y)) {
            paint_save(w);
            return;
        }
        int palette_y = w->y + WIN_CAPTION_H + 11;
        int local_x = x - (w->x + 11);
        if (y >= palette_y && local_x >= 0) {
            int idx = local_x / 30;
            if (idx >= 0 && idx < 8) {
                uint32_t colors[] = {
                    COL_BLACK, COL_WHITE, 0xFFFF0000, 0xFF00FF00,
                    0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF
                };
                w->state.paint.current_color = colors[idx];
            }
        }
        return;
    }

    int canvas_x, canvas_y, canvas_w, canvas_h;
    paint_canvas_rect(w, &canvas_x, &canvas_y, &canvas_w, &canvas_h);
    if (rect_contains(canvas_x, canvas_y, canvas_w, canvas_h, x, y)) {
        int px = ((x - canvas_x) * PAINT_CANVAS_W) / canvas_w;
        int py = ((y - canvas_y) * PAINT_CANVAS_H) / canvas_h;
        if (px >= 0 && px < PAINT_CANVAS_W &&
            py >= 0 && py < PAINT_CANVAS_H) {
            w->state.paint.pixels[py * PAINT_CANVAS_W + px] =
                w->state.paint.current_color;
            w->state.paint.dirty = true;
            w->state.paint.discard_deadline = 0;
            cancel_exit_discard_confirmation();
            str_copy(w->state.paint.status,
                     sizeof(w->state.paint.status), "Unsaved changes.");
        }
    }
}

static void handle_notepad_click(Window* w, int x, int y) {
    int button_x = w->x + w->w - 70;
    int button_y = w->y + WIN_CAPTION_H + 5;
    if (rect_contains(button_x, button_y, 58, 22, x, y))
        notepad_save(w);
}

static void settings_report_result(bool saved) {
    bool editor_blocked =
        !saved && open_file_has_unsaved_changes("desktop.cfg");
    bool persistent = fs_backend_is_persistent();
    const char* status =
        saved ? (persistent ? "Saved to persistent desktop.cfg."
                            : "Saved for this session only.")
              : editor_blocked
                    ? "Session only: desktop.cfg has unsaved edits."
                    : "Session only: desktop.cfg save failed.";

    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window* settings = windows[i];
        if (!settings || settings->type != APP_SETTINGS) continue;
        settings->state.settings.wallpaper_enabled = g_wallpaper_enabled;
        settings->state.settings.theme_id = current_theme_idx;
        str_copy(settings->state.settings.status,
                 sizeof(settings->state.settings.status), status);
    }
    desktop_notify(
        saved ? (persistent ? "Desktop settings saved persistently."
                            : "Desktop settings saved for this session.")
              : editor_blocked
                    ? "Save or close desktop.cfg in Notepad first."
                    : "Desktop settings changed for this session only.");
}

static void handle_settings_click(Window* w, int x, int y) {
    int cx = w->x + 2;
    int cy = w->y + WIN_CAPTION_H + 2;
    if (rect_contains(cx + 10, cy + 30, 140, 30, x, y)) {
        g_wallpaper_enabled = !g_wallpaper_enabled;
        w->state.settings.wallpaper_enabled = g_wallpaper_enabled;
        settings_report_result(settings_save());
    } else if (rect_contains(cx + 10, cy + 100, 140, 30, x, y)) {
        current_theme_idx = (current_theme_idx + 1) % 2;
        w->state.settings.theme_id = current_theme_idx;
        settings_report_result(settings_save());
    }
}

static int files_visible_rows(const Window* w) {
    int content_height = w->h - WIN_CAPTION_H - 8;
    int rows = (content_height - 70) / 18;
    if (rows < 1) rows = 1;
    return rows;
}

static void files_normalize(Window* w) {
    int count = (int)fs_file_count();
    int rows = files_visible_rows(w);
    if (count <= 0) {
        w->state.files.selected_index = -1;
        w->state.files.selected_name[0] = 0;
        w->state.files.scroll_offset = 0;
        return;
    }
    if (w->state.files.selected_name[0]) {
        int resolved = file_index_by_name(w->state.files.selected_name);
        if (resolved >= 0) {
            w->state.files.selected_index = resolved;
        } else {
            w->state.files.selected_index = -1;
            w->state.files.selected_name[0] = 0;
        }
    } else {
        w->state.files.selected_index = -1;
    }
    if (w->state.files.selected_index >= 0) {
        if (w->state.files.selected_index < w->state.files.scroll_offset) {
            w->state.files.scroll_offset =
                w->state.files.selected_index;
        } else if (w->state.files.selected_index >=
                   w->state.files.scroll_offset + rows) {
            w->state.files.scroll_offset =
                w->state.files.selected_index - rows + 1;
        }
    }
    int max_offset = count > rows ? count - rows : 0;
    if (w->state.files.scroll_offset > max_offset)
        w->state.files.scroll_offset = max_offset;
    if (w->state.files.scroll_offset < 0)
        w->state.files.scroll_offset = 0;
}

static const struct fs_file* files_selected(Window* w) {
    files_normalize(w);
    if (w->state.files.selected_index < 0 ||
        !w->state.files.selected_name[0])
        return NULL;
    return fs_file_at((size_t)w->state.files.selected_index);
}

static void retarget_open_file(const char* old_name, const char* new_name) {
    char old_address[FS_MAX_FILENAME + 6] = "file:";
    char new_address[FS_MAX_FILENAME + 6] = "file:";
    str_append(old_address, sizeof(old_address), old_name);
    str_append(new_address, sizeof(new_address), new_name);

    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window* open = windows[i];
        if (!open) continue;
        if (open->type == APP_NOTEPAD &&
            kstrcmp(open->state.notepad.filename, old_name) == 0) {
            str_copy(open->state.notepad.filename,
                     sizeof(open->state.notepad.filename), new_name);
            str_copy(open->title, sizeof(open->title), new_name);
            str_copy(open->state.notepad.status,
                     sizeof(open->state.notepad.status),
                     "Backing file was renamed.");
        } else if (open->type == APP_PAINT &&
                   kstrcmp(open->state.paint.filename, old_name) == 0) {
            str_copy(open->state.paint.filename,
                     sizeof(open->state.paint.filename), new_name);
            str_copy(open->title, sizeof(open->title), new_name);
            str_copy(open->state.paint.status,
                     sizeof(open->state.paint.status),
                     "Backing file was renamed.");
        } else if (open->type == APP_IMAGEVIEW &&
                   kstrcmp(open->state.img.filename, old_name) == 0) {
            imageview_open_named(open, new_name);
            str_copy(open->title, sizeof(open->title), new_name);
        } else if (open->type == APP_BROWSER &&
                   (kstrcmp(open->state.browser.address, old_name) == 0 ||
                    kstrcmp(open->state.browser.address, old_address) == 0)) {
            str_copy(open->state.browser.address,
                     sizeof(open->state.browser.address), new_address);
            open->state.browser.address_len =
                kstrlen_local(open->state.browser.address);
            browser_load(open);
        } else if (open->type == APP_FILES &&
                   kstrcmp(open->state.files.selected_name, old_name) == 0) {
            str_copy(open->state.files.selected_name,
                     sizeof(open->state.files.selected_name), new_name);
            open->state.files.selected_index =
                file_index_by_name(new_name);
        }
    }
}

static void detach_deleted_file(const char* filename) {
    char file_address[FS_MAX_FILENAME + 6] = "file:";
    str_append(file_address, sizeof(file_address), filename);

    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window* open = windows[i];
        if (!open) continue;
        if (open->type == APP_NOTEPAD &&
            kstrcmp(open->state.notepad.filename, filename) == 0) {
            bool was_dirty = open->state.notepad.dirty;
            open->state.notepad.filename[0] = 0;
            open->state.notepad.dirty = was_dirty;
            open->state.notepad.discard_deadline = 0;
            str_copy(open->title, sizeof(open->title), "Deleted text file");
            str_copy(open->state.notepad.status,
                     sizeof(open->state.notepad.status),
                     was_dirty ? "Deleted externally; edits save as a new note."
                               : "Deleted; edit to save as a new note.");
        } else if (open->type == APP_PAINT &&
                   kstrcmp(open->state.paint.filename, filename) == 0) {
            bool was_dirty = open->state.paint.dirty;
            open->state.paint.filename[0] = 0;
            open->state.paint.dirty = was_dirty;
            open->state.paint.discard_deadline = 0;
            str_copy(open->title, sizeof(open->title), "Deleted painting");
            str_copy(open->state.paint.status,
                     sizeof(open->state.paint.status),
                     was_dirty ? "Deleted externally; edits save as a new BMP."
                               : "Deleted; paint to save as a new BMP.");
        } else if (open->type == APP_IMAGEVIEW &&
                   kstrcmp(open->state.img.filename, filename) == 0) {
            open->state.img.file_index = -1;
            open->state.img.filename[0] = 0;
            open->state.img.valid = false;
            imageview_load(open, 1);
        } else if (open->type == APP_BROWSER &&
                   (kstrcmp(open->state.browser.address, filename) == 0 ||
                    kstrcmp(open->state.browser.address, file_address) == 0)) {
            browser_load(open);
        } else if (open->type == APP_FILES &&
                   kstrcmp(open->state.files.selected_name, filename) == 0) {
            open->state.files.selected_name[0] = 0;
            open->state.files.selected_index = -1;
            open->state.files.delete_name[0] = 0;
            open->state.files.delete_deadline = 0;
        }
    }
}

static bool open_file_has_unsaved_changes(const char* filename) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window* open = windows[i];
        if (!open) continue;
        if (open->type == APP_NOTEPAD &&
            open->state.notepad.dirty &&
            kstrcmp(open->state.notepad.filename, filename) == 0)
            return true;
        if (open->type == APP_PAINT &&
            open->state.paint.dirty &&
            kstrcmp(open->state.paint.filename, filename) == 0)
            return true;
    }
    return false;
}

static void files_open_selected(Window* w) {
    const struct fs_file* file = files_selected(w);
    if (!file) {
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "Select a file first.");
        return;
    }

    char filename[FS_MAX_FILENAME];
    str_copy(filename, sizeof(filename), file->name);
    if (kstrcmp(filename, "system.log") == 0) {
        if (open_browser_file(filename)) {
            str_copy(w->state.files.status, sizeof(w->state.files.status),
                     "Opened live read-only system.log.");
        } else {
            str_copy(w->state.files.status, sizeof(w->state.files.status),
                     "Local Browser could not be opened.");
        }
    } else if (filename_is_bmp(filename)) {
        struct bmp_image image;
        if (!bmp_open(file->data, file->size, &image)) {
            str_copy(w->state.files.status, sizeof(w->state.files.status),
                     "Invalid or unsupported 24-bit BMP.");
        } else if (open_image_file(filename)) {
            str_copy(w->state.files.status, sizeof(w->state.files.status),
                     "Opened in Image Viewer.");
        } else {
            str_copy(w->state.files.status, sizeof(w->state.files.status),
                     "Image Viewer could not be opened.");
        }
    } else if (file_is_text(file)) {
        if (open_notepad_file(filename))
            str_copy(w->state.files.status, sizeof(w->state.files.status),
                     "Opened in Notepad.");
        else
            str_copy(w->state.files.status, sizeof(w->state.files.status),
                     "Text file could not be opened.");
    } else {
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "Binary file: inspect it with Terminal hexdump.");
    }
}

static void files_create_text(Window* w) {
    Window* note = create_notepad_file();
    if (!note) {
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "Could not create a text file.");
        return;
    }
    w->state.files.selected_index =
        file_index_by_name(note->state.notepad.filename);
    str_copy(w->state.files.selected_name,
             sizeof(w->state.files.selected_name),
             note->state.notepad.filename);
    str_copy(w->state.files.status, sizeof(w->state.files.status),
             fs_backend_is_persistent()
                 ? "Created in persistent storage."
                 : "Created for this session only.");
    files_normalize(w);
}

static void files_cancel_delete_confirmation(FileManagerState* state) {
    state->delete_name[0] = 0;
    state->delete_deadline = 0;
}

static void files_delete_selected(Window* w) {
    const struct fs_file* file = files_selected(w);
    if (!file) {
        files_cancel_delete_confirmation(&w->state.files);
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "Select a file first.");
        return;
    }
    char filename[FS_MAX_FILENAME];
    str_copy(filename, sizeof(filename), file->name);
    if (kstrcmp(filename, "system.log") == 0) {
        files_cancel_delete_confirmation(&w->state.files);
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "system.log is live and read-only.");
        return;
    }
    if (open_file_has_unsaved_changes(filename)) {
        files_cancel_delete_confirmation(&w->state.files);
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "Save or close the modified editor first.");
        return;
    }

    uint64_t now = timer_get_ticks();
    if (w->state.files.delete_deadline == 0 ||
        now > w->state.files.delete_deadline ||
        kstrcmp(w->state.files.delete_name, filename) != 0) {
        str_copy(w->state.files.delete_name,
                 sizeof(w->state.files.delete_name), filename);
        w->state.files.delete_deadline =
            now + DISCARD_CONFIRM_TICKS;
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "Click Delete again within 3s to confirm.");
        return;
    }

    files_cancel_delete_confirmation(&w->state.files);
    if (fs_remove(filename)) {
        detach_deleted_file(filename);
        w->state.files.selected_index = -1;
        w->state.files.selected_name[0] = 0;
        w->state.files.renaming = false;
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 fs_backend_is_persistent()
                     ? "Deleted from persistent storage."
                     : "Deleted for this session only.");
        files_normalize(w);
    } else {
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "Delete failed.");
    }
}

static void files_begin_rename(Window* w) {
    const struct fs_file* file = files_selected(w);
    if (!file) {
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "Select a file first.");
        return;
    }
    if (kstrcmp(file->name, "system.log") == 0) {
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "system.log is live and read-only.");
        return;
    }
    str_copy(w->state.files.rename_buffer,
             sizeof(w->state.files.rename_buffer), file->name);
    w->state.files.rename_length =
        kstrlen_local(w->state.files.rename_buffer);
    w->state.files.renaming = true;
    w->state.files.rename_select_all = true;
    str_copy(w->state.files.status, sizeof(w->state.files.status),
             "Type a new name and press Enter.");
}

static void files_commit_rename(Window* w) {
    const struct fs_file* file = files_selected(w);
    if (!file || !w->state.files.rename_buffer[0]) {
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "Rename cancelled: invalid name.");
        w->state.files.renaming = false;
        return;
    }
    char old_name[FS_MAX_FILENAME];
    char new_name[FS_MAX_FILENAME];
    str_copy(old_name, sizeof(old_name), file->name);
    str_copy(new_name, sizeof(new_name), w->state.files.rename_buffer);
    if (fs_rename(old_name, new_name)) {
        retarget_open_file(old_name, new_name);
        w->state.files.selected_index = file_index_by_name(new_name);
        str_copy(w->state.files.selected_name,
                 sizeof(w->state.files.selected_name), new_name);
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 fs_backend_is_persistent()
                     ? "Renamed on persistent storage."
                     : "Renamed for this session only.");
    } else {
        str_copy(w->state.files.status, sizeof(w->state.files.status),
                 "Rename failed: name may be invalid or in use.");
    }
    w->state.files.renaming = false;
    w->state.files.rename_select_all = false;
}

static void handle_files_click(Window* w, int x, int y) {
    int cx = w->x + 4;
    int cy = w->y + WIN_CAPTION_H + 4;
    int rows = files_visible_rows(w);
    int count = (int)fs_file_count();
    files_normalize(w);

    if (rect_contains(cx + 4, cy + 2, 38, 22, x, y)) {
        files_cancel_delete_confirmation(&w->state.files);
        files_create_text(w);
        return;
    }
    if (rect_contains(cx + 46, cy + 2, 42, 22, x, y)) {
        files_cancel_delete_confirmation(&w->state.files);
        files_open_selected(w);
        return;
    }
    if (rect_contains(cx + 92, cy + 2, 54, 22, x, y)) {
        files_cancel_delete_confirmation(&w->state.files);
        files_begin_rename(w);
        return;
    }
    if (rect_contains(cx + 150, cy + 2, 50, 22, x, y)) {
        files_delete_selected(w);
        return;
    }
    if (rect_contains(cx + 204, cy + 2, 32, 22, x, y)) {
        files_cancel_delete_confirmation(&w->state.files);
        w->state.files.scroll_offset -= rows;
        w->state.files.selected_index = -1;
        w->state.files.selected_name[0] = 0;
        files_normalize(w);
        return;
    }
    if (rect_contains(cx + 240, cy + 2, 42, 22, x, y)) {
        files_cancel_delete_confirmation(&w->state.files);
        if (w->state.files.scroll_offset + rows < count)
            w->state.files.scroll_offset += rows;
        w->state.files.selected_index = -1;
        w->state.files.selected_name[0] = 0;
        files_normalize(w);
        return;
    }

    for (int row = 0; row < rows; row++) {
        int index = w->state.files.scroll_offset + row;
        if (index >= count) break;
        int ry = cy + 50 + row * 18;
        if (rect_contains(cx + 2, ry, w->w - 12, 18, x, y)) {
            files_cancel_delete_confirmation(&w->state.files);
            w->state.files.selected_index = index;
            const struct fs_file* selected = fs_file_at((size_t)index);
            str_copy(w->state.files.selected_name,
                     sizeof(w->state.files.selected_name),
                     selected ? selected->name : "");
            w->state.files.renaming = false;
            w->state.files.rename_select_all = false;
            return;
        }
    }
}

#define TASKMGR_PAGE_CAPACITY 16

static int taskmgr_visible_rows(const Window* w) {
    int content_height = w->h - WIN_CAPTION_H - 4;
    int rows = (content_height - 96) / 20;
    if (rows < 1) rows = 1;
    if (rows > TASKMGR_PAGE_CAPACITY) rows = TASKMGR_PAGE_CAPACITY;
    return rows;
}

static void taskmgr_normalize_page(Window* w, size_t total, int rows) {
    if (total == 0) {
        w->state.taskmgr.page_offset = 0;
        return;
    }

    if (w->state.taskmgr.page_offset >= total) {
        w->state.taskmgr.page_offset =
            ((total - 1) / (size_t)rows) * (size_t)rows;
    }
}

static void handle_taskmgr_click(Window* w, int x, int y) {
    int cx = w->x + 2;
    int cy = w->y + WIN_CAPTION_H + 2;
    int ch = w->h - WIN_CAPTION_H - 4;
    int rows = taskmgr_visible_rows(w);
    size_t total = scheduler_task_count();
    taskmgr_normalize_page(w, total, rows);

    int button_y = cy + ch - 24;
    if (rect_contains(cx + 10, button_y, 44, 20, x, y)) {
        size_t step = (size_t)rows;
        if (w->state.taskmgr.page_offset > step) {
            w->state.taskmgr.page_offset -= step;
        } else {
            w->state.taskmgr.page_offset = 0;
        }
        w->state.taskmgr.selected_pid = 0;
        return;
    }
    if (rect_contains(cx + 60, button_y, 44, 20, x, y)) {
        size_t next = w->state.taskmgr.page_offset + (size_t)rows;
        if (next < total) w->state.taskmgr.page_offset = next;
        w->state.taskmgr.selected_pid = 0;
        return;
    }
    if (rect_contains(cx + 110, button_y, 70, 20, x, y)) {
        uint64_t selected = w->state.taskmgr.selected_pid;
        struct scheduler_task_info visible[TASKMGR_PAGE_CAPACITY];
        size_t visible_count = scheduler_snapshot_tasks_from(
            w->state.taskmgr.page_offset, visible, (size_t)rows);
        bool selected_is_visible = false;
        for (size_t i = 0; i < visible_count; i++) {
            if (visible[i].id == selected) {
                selected_is_visible = true;
                break;
            }
        }
        if (selected_is_visible &&
            selected != 0 &&
            scheduler_terminate_task(selected)) {
            w->state.taskmgr.selected_pid = 0;
            total = scheduler_task_count();
            taskmgr_normalize_page(w, total, rows);
            str_copy(w->state.taskmgr.status,
                     sizeof(w->state.taskmgr.status), "Selected task ended.");
        } else {
            str_copy(w->state.taskmgr.status,
                     sizeof(w->state.taskmgr.status),
                     "Cannot end current or missing task.");
        }
        return;
    }

    struct scheduler_task_info tasks[TASKMGR_PAGE_CAPACITY];
    size_t count = scheduler_snapshot_tasks_from(
        w->state.taskmgr.page_offset, tasks, (size_t)rows);
    int list_y = cy + 36;
    for (size_t i = 0; i < count; i++) {
        if (rect_contains(cx + 8, list_y - 2, w->w - 20, 18, x, y)) {
            w->state.taskmgr.selected_pid = tasks[i].id;
            return;
        }
        list_y += 20;
    }
}

static void handle_browser_click(Window* w, int x, int y) {
    int cx = w->x + 2;
    int cy = w->y + WIN_CAPTION_H + 2;
    int cw = w->w - 4;
    int ch = w->h - WIN_CAPTION_H - 4;
    int button_y = cy + ch - 24;
    BrowserState* state = &w->state.browser;

    if (rect_contains(cx + 2, cy + 2, cw - 44, 24, x, y)) {
        state->address_select_all = true;
        str_copy(state->status, sizeof(state->status),
                 "Type an address and press Enter.");
    } else if (rect_contains(cx + cw - 38, cy + 2, 34, 24, x, y)) {
        state->address_select_all = false;
        browser_load(w);
    } else if (rect_contains(cx + 8, button_y, 42, 20, x, y)) {
        if (state->scroll > 0) state->scroll--;
    } else if (rect_contains(cx + 56, button_y, 50, 20, x, y)) {
        int max_scroll = browser_max_scroll(w, state);
        if (state->scroll < max_scroll) state->scroll++;
    } else if (rect_contains(cx + 112, button_y, 66, 20, x, y)) {
        state->address_select_all = false;
        browser_load(w);
    }
}

static void calc_button_rect(Window* w, int button, int* x, int* y) {
    int cx = w->x + 2;
    int cy = w->y + WIN_CAPTION_H + 2;
    *x = cx + 10 + (button % 4) * 40;
    *y = cy + 45 + (button / 4) * 30;
}

static void calc_store_result(CalcState* state, long long value) {
    if (value > INT_MAX || value < INT_MIN) {
        state->error_code = CALC_ERROR_OVERFLOW;
        return;
    }
    state->current_val = (int)value;
}

static void handle_calc_logic(Window* w, int x, int y) {
    const char* btns = "789/456*123-C0=+";
    for(int b=0; b<16; b++) {
        int bx, by;
        calc_button_rect(w, b, &bx, &by);
        if (rect_contains(bx, by, 35, 25, x, y)) {
            char c = btns[b]; 
            CalcState* s = &w->state.calc;
            if (c >= '0' && c <= '9') {
                if (s->error_code != CALC_ERROR_NONE) {
                    s->current_val = 0;
                    s->accumulator = 0;
                    s->op = 0;
                    s->new_entry = true;
                    s->error_code = CALC_ERROR_NONE;
                }
                int d = c - '0';
                if (s->new_entry) { s->current_val = d; s->new_entry = false; }
                else if (s->current_val >= 0 && s->current_val <= (INT_MAX - d) / 10)
                    s->current_val = s->current_val * 10 + d;
                else {
                    s->error_code = CALC_ERROR_OVERFLOW;
                    s->op = 0;
                    s->new_entry = true;
                }
            }
            else if (c == 'C') {
                s->current_val = 0;
                s->accumulator = 0;
                s->op = 0;
                s->new_entry = true;
                s->error_code = CALC_ERROR_NONE;
            }
            else if (s->error_code == CALC_ERROR_NONE &&
                     (c == '+' || c == '-' || c == '*' || c == '/')) {
                s->accumulator = s->current_val;
                s->op = c;
                s->new_entry = true;
            }
            else if (s->error_code == CALC_ERROR_NONE && c == '=') {
                long long left = s->accumulator;
                long long right = s->current_val;
                if (s->op == '+') calc_store_result(s, left + right);
                else if (s->op == '-') calc_store_result(s, left - right);
                else if (s->op == '*') calc_store_result(s, left * right);
                else if (s->op == '/') {
                    if (right == 0)
                        s->error_code = CALC_ERROR_DIV_ZERO;
                    else
                        calc_store_result(s, left / right);
                }
                s->op = 0; s->new_entry = true;
            }
            return;
        }
    }
}

static void gui_terminal_add_line(TerminalState* state, const char* line) {
    if (state->line_count >= GUI_TERM_LINES) {
        for (int i = 1; i < GUI_TERM_LINES; i++) {
            str_copy(state->lines[i - 1], GUI_TERM_LINE_LEN, state->lines[i]);
        }
        state->line_count = GUI_TERM_LINES - 1;
        state->history_truncated = true;
    }
    str_copy(state->lines[state->line_count], GUI_TERM_LINE_LEN, line);
    state->line_count++;
}

static void gui_terminal_add_output(TerminalState* state, const char* output,
                                    size_t output_length) {
    char line[GUI_TERM_LINE_LEN];
    int length = 0;
    bool wrote_anything = false;
    bool replaced_binary = false;

    for (size_t i = 0; output && i < output_length; i++) {
        char current = output[i];
        if (current == '\r') continue;
        if (current == '\n') {
            line[length] = 0;
            gui_terminal_add_line(state, line);
            length = 0;
            wrote_anything = true;
            continue;
        }
        if (current == '\t') current = ' ';
        if ((unsigned char)current < 32 ||
            (unsigned char)current > 126) {
            current = '?';
            replaced_binary = true;
        }
        line[length++] = current;
        if (length + 1 >= GUI_TERM_LINE_LEN) {
            line[length] = 0;
            gui_terminal_add_line(state, line);
            length = 0;
            wrote_anything = true;
        }
    }
    if (length > 0) {
        line[length] = 0;
        gui_terminal_add_line(state, line);
        wrote_anything = true;
    }
    if (!wrote_anything)
        gui_terminal_add_line(state, "(command completed with no output)");
    if (replaced_binary)
        gui_terminal_add_line(state, "[non-text output bytes shown as ?]");
}

enum gui_fs_mutation {
    GUI_FS_MUTATION_NONE,
    GUI_FS_MUTATION_TOUCH,
    GUI_FS_MUTATION_MODIFY,
    GUI_FS_MUTATION_REMOVE
};

static enum gui_fs_mutation gui_fs_command_target(
    const char* command_line, char target[FS_MAX_FILENAME]) {
    const char* cursor = kskip_spaces(command_line);
    char command[16];
    size_t command_length = 0;
    target[0] = 0;

    while (cursor[command_length] &&
           cursor[command_length] != ' ' &&
           cursor[command_length] != '\t') {
        if (command_length + 1 >= sizeof(command))
            return GUI_FS_MUTATION_NONE;
        command[command_length] = cursor[command_length];
        command_length++;
    }
    command[command_length] = 0;
    cursor = kskip_spaces(cursor + command_length);

    size_t target_length = 0;
    while (cursor[target_length] &&
           cursor[target_length] != ' ' &&
           cursor[target_length] != '\t') {
        if (target_length + 1 >= FS_MAX_FILENAME) {
            target[0] = 0;
            return GUI_FS_MUTATION_NONE;
        }
        target[target_length] = cursor[target_length];
        target_length++;
    }
    target[target_length] = 0;
    if (target_length == 0) return GUI_FS_MUTATION_NONE;

    if (kstrcmp(command, "touch") == 0)
        return cursor[target_length] == 0
             ? GUI_FS_MUTATION_TOUCH : GUI_FS_MUTATION_NONE;
    if (kstrcmp(command, "write") == 0 ||
        kstrcmp(command, "append") == 0) {
        if (*kskip_spaces(cursor + target_length) == 0) {
            target[0] = 0;
            return GUI_FS_MUTATION_NONE;
        }
        return GUI_FS_MUTATION_MODIFY;
    }
    if (kstrcmp(command, "rm") == 0)
        return cursor[target_length] == 0
             ? GUI_FS_MUTATION_REMOVE : GUI_FS_MUTATION_NONE;
    target[0] = 0;
    return GUI_FS_MUTATION_NONE;
}

static uint32_t gui_file_hash(const struct fs_file* file) {
    uint32_t hash = 2166136261u;
    if (!file) return 0;
    for (size_t i = 0; i < file->size; i++) {
        hash ^= (uint8_t)file->data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void terminal_execute(Window* w) {
    TerminalState* state = &w->state.term;
    char command[sizeof(state->input)];
    str_copy(command, sizeof(command), state->input);
    state->input[0] = 0;
    state->input_len = 0;

    if (command[0] == 0) return;
    if (kstrcmp(command, "exit") == 0) {
        close_window(w->id);
        return;
    }
    if (kstrcmp(command, "cls") == 0 || kstrcmp(command, "clear") == 0) {
        for (int i = 0; i < GUI_TERM_LINES; i++) state->lines[i][0] = 0;
        state->line_count = 0;
        state->history_truncated = false;
        return;
    }

    char prompt_line[GUI_TERM_LINE_LEN] = "$ ";
    str_append(prompt_line, sizeof(prompt_line), command);
    gui_terminal_add_line(state, prompt_line);

    char mutation_target[FS_MAX_FILENAME];
    enum gui_fs_mutation mutation =
        gui_fs_command_target(command, mutation_target);
    if ((mutation == GUI_FS_MUTATION_MODIFY ||
         mutation == GUI_FS_MUTATION_REMOVE) &&
        open_file_has_unsaved_changes(mutation_target)) {
        gui_terminal_add_line(
            state, "Blocked: save or close the modified editor first.");
        return;
    }

    const struct fs_file* before_file =
        mutation != GUI_FS_MUTATION_NONE ? fs_find(mutation_target) : NULL;
    bool before_exists = before_file != NULL;
    size_t before_size = before_file ? before_file->size : 0;
    uint32_t before_hash = gui_file_hash(before_file);

    char output[FS_MAX_FILE_SIZE];
    size_t output_length = 0;
    bool truncated = false;
    if (!shell_execute_capture(command, output, sizeof(output),
                               &output_length, &truncated)) {
        gui_terminal_add_line(state, "Terminal capture is busy or unavailable.");
        return;
    }
    gui_terminal_add_output(state, output, output_length);
    if (truncated)
        gui_terminal_add_line(state, "[output truncated to terminal capacity]");
    const struct fs_file* after_file =
        mutation != GUI_FS_MUTATION_NONE ? fs_find(mutation_target) : NULL;
    bool filesystem_changed =
        mutation != GUI_FS_MUTATION_NONE &&
        (before_exists != (after_file != NULL) ||
         before_size != (after_file ? after_file->size : 0) ||
         before_hash != gui_file_hash(after_file));
    if (filesystem_changed)
        reconcile_filesystem_windows("Filesystem changed by Terminal.");
}

static void handle_terminal_input(Window* w, char c) {
    TerminalState* state = &w->state.term;
    if (c == '\n') {
        terminal_execute(w);
    } else if (c == '\b') {
        if (state->input_len > 0) state->input[--state->input_len] = 0;
    } else if (c >= 32 && c <= 126 &&
               state->input_len < (int)sizeof(state->input) - 1) {
        state->input[state->input_len++] = c;
        state->input[state->input_len] = 0;
    }
}

static void handle_browser_input(Window* w, char c) {
    BrowserState* state = &w->state.browser;
    if (c == '\b') {
        if (state->address_select_all) {
            state->address[0] = 0;
            state->address_len = 0;
            state->address_select_all = false;
        } else if (state->address_len > 0) {
            state->address[--state->address_len] = 0;
        }
    } else if (c == '\n') {
        state->address_select_all = false;
        browser_load(w);
    } else if (c >= 32 && c <= 126) {
        if (state->address_select_all) {
            state->address[0] = 0;
            state->address_len = 0;
            state->address_select_all = false;
        }
        if (state->address_len < (int)sizeof(state->address) - 1) {
            state->address[state->address_len++] = c;
            state->address[state->address_len] = 0;
        }
    }
}

static void update_sysmon(Window* w) {
    SysMonState* s = &w->state.sysmon;
    uint64_t now_tick = timer_get_ticks();
    if (s->has_previous_cpu && now_tick - s->last_sample_tick < 25u) return;

    struct timer_cpu_counters current;
    timer_read_cpu_counters(&current);

    int cpu_percent = 0;
    if (s->has_previous_cpu) {
        uint64_t total_delta = current.total_cycles - s->previous_cpu.total_cycles;
        uint64_t idle_delta = current.idle_cycles - s->previous_cpu.idle_cycles;
        if (idle_delta > total_delta) idle_delta = total_delta;
        if (total_delta > 0) {
            uint64_t busy_delta = total_delta - idle_delta;
            cpu_percent = (int)((busy_delta * 100u) / total_delta);
            if (cpu_percent > 100) cpu_percent = 100;
        }
        s->head = (s->head + 1) % SYSMON_HIST;
    }

    const struct system_profile* profile = system_profile_info();
    uint64_t total_kb = profile->memory_total_kb;
    uint64_t mapped_kb = profile->memory_mapped_kb;
    uint64_t used_kb = profile->memory_used_kb;
    int memory_percent = 0;
    if (mapped_kb > 0) {
        uint64_t scaled = (uint64_t)used_kb * 100u;
        memory_percent = (int)(scaled / mapped_kb);
        if (memory_percent > 100) memory_percent = 100;
    }

    s->cpu_percent = cpu_percent;
    s->memory_used_kb = used_kb;
    s->memory_total_kb = total_kb;
    s->memory_mapped_kb = mapped_kb;
    s->memory_managed_kb = profile->memory_managed_kb;
    s->memory_reserved_kb = profile->memory_reserved_kb;
    s->memory_heap_committed_kb = profile->memory_heap_committed_kb;
    s->cpu_hist[s->head] = cpu_percent;
    s->mem_hist[s->head] = memory_percent;
    s->previous_cpu = current;
    s->last_sample_tick = now_tick;
    s->has_previous_cpu = true;
}

static void reconcile_filesystem_windows(const char* files_status) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window* w = windows[i];
        if (!w) continue;

        if (w->type == APP_FILES) {
            files_cancel_delete_confirmation(&w->state.files);
            files_normalize(w);
            str_copy(w->state.files.status, sizeof(w->state.files.status),
                     files_status ? files_status
                                  : "Refreshed current filesystem listing.");
        } else if (w->type == APP_BROWSER) {
            browser_load(w);
        } else if (w->type == APP_IMAGEVIEW) {
            char filename[FS_MAX_FILENAME];
            str_copy(filename, sizeof(filename), w->state.img.filename);
            if (!filename[0] || !imageview_open_named(w, filename))
                imageview_load(w, 1);
        } else if (w->type == APP_NOTEPAD &&
                   !w->state.notepad.dirty &&
                   w->state.notepad.filename[0]) {
            const struct fs_file* file =
                fs_find(w->state.notepad.filename);
            if (file && file_is_text(file)) {
                size_t length = file->size;
                if (length >= sizeof(w->state.notepad.buffer))
                    length = sizeof(w->state.notepad.buffer) - 1;
                for (size_t j = 0; j < length; j++)
                    w->state.notepad.buffer[j] = file->data[j];
                w->state.notepad.buffer[length] = 0;
                w->state.notepad.length = (int)length;
                str_copy(w->state.notepad.status,
                         sizeof(w->state.notepad.status),
                         "Reloaded from filesystem.");
            } else if (!file) {
                char missing[FS_MAX_FILENAME];
                str_copy(missing, sizeof(missing),
                         w->state.notepad.filename);
                detach_deleted_file(missing);
            } else {
                w->state.notepad.filename[0] = 0;
                w->state.notepad.dirty = false;
                w->state.notepad.discard_deadline = 0;
                str_copy(w->title, sizeof(w->title), "Changed text file");
                str_copy(w->state.notepad.status,
                         sizeof(w->state.notepad.status),
                         "Backing file became binary; edit to save a new note.");
            }
        } else if (w->type == APP_PAINT &&
                   !w->state.paint.dirty &&
                   w->state.paint.filename[0]) {
            char filename[FS_MAX_FILENAME];
            str_copy(filename, sizeof(filename),
                     w->state.paint.filename);
            if (fs_find(filename))
                paint_reload(w);
            else
                detach_deleted_file(filename);
        }
    }
}

static void desktop_refresh(void) {
    settings_load();
    reconcile_filesystem_windows("Refreshed current filesystem listing.");
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window* w = windows[i];
        if (!w) continue;
        if (w->type == APP_TASKMGR) {
            int rows = taskmgr_visible_rows(w);
            taskmgr_normalize_page(w, scheduler_task_count(), rows);
            str_copy(w->state.taskmgr.status,
                     sizeof(w->state.taskmgr.status),
                     "Refreshed live scheduler snapshot.");
        } else if (w->type == APP_SYSMON) {
            w->state.sysmon.has_previous_cpu = false;
            update_sysmon(w);
        }
    }
    desktop_log("GUI: desktop state refreshed");
}

// --- Context Menu Functions ---
static void show_context_menu(int x, int y) {
    g_ctx_menu.active = true;
    g_ctx_menu.w = 120;
    
    // Define items
    str_copy(g_ctx_menu.items[0].label, sizeof(g_ctx_menu.items[0].label), "Refresh");
    g_ctx_menu.items[0].action_id = CTX_REFRESH;
    
    str_copy(g_ctx_menu.items[1].label, sizeof(g_ctx_menu.items[1].label), "Next Wallpaper");
    g_ctx_menu.items[1].action_id = CTX_WALLPAPER;
    
    str_copy(g_ctx_menu.items[2].label, sizeof(g_ctx_menu.items[2].label), "New File");
    g_ctx_menu.items[2].action_id = CTX_NEW_FILE;
    
    str_copy(g_ctx_menu.items[3].label, sizeof(g_ctx_menu.items[3].label), "System Info");
    g_ctx_menu.items[3].action_id = CTX_SYS_INFO;
    
    str_copy(g_ctx_menu.items[4].label, sizeof(g_ctx_menu.items[4].label), "About");
    g_ctx_menu.items[4].action_id = CTX_ABOUT;
    
    g_ctx_menu.count = 5;
    g_ctx_menu.h = g_ctx_menu.count * 20 + 4;
    if (x + g_ctx_menu.w > screen_w) x = screen_w - g_ctx_menu.w;
    if (y + g_ctx_menu.h > screen_h - TASKBAR_H) y = screen_h - TASKBAR_H - g_ctx_menu.h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_ctx_menu.x = x;
    g_ctx_menu.y = y;
}

static void hide_context_menu(void) {
    g_ctx_menu.active = false;
}

static void handle_context_menu_click(int x, int y) {
    if (!g_ctx_menu.active) return;
    
    // Check bounds
    if (x >= g_ctx_menu.x && x < g_ctx_menu.x + g_ctx_menu.w &&
        y >= g_ctx_menu.y && y < g_ctx_menu.y + g_ctx_menu.h) {
        
        int idx = (y - g_ctx_menu.y - 2) / 20;
        if (idx >= 0 && idx < g_ctx_menu.count) {
            int action = g_ctx_menu.items[idx].action_id;
            switch(action) {
                case CTX_REFRESH:
                    desktop_refresh();
                    break;
                case CTX_WALLPAPER: {
                    g_wallpaper_enabled = true;
                    g_wallpaper_seed = fast_rand() % 1000;
                    bool saved = settings_save();
                    settings_report_result(saved);
                    break;
                }
                case CTX_NEW_FILE:
                    create_notepad_file();
                    break;
                case CTX_SYS_INFO:
                    create_window(APP_SYSMON, "System Monitor", 300, 200);
                    break;
                case CTX_ABOUT:
                    create_window(APP_ABOUT, "About Nostalux", 320, 240);
                    break;
            }
        }
    }
    
    hide_context_menu();
}

static void render_context_menu(void) {
    if (!g_ctx_menu.active) return;
    
    // Shadow
    graphics_fill_rect(g_ctx_menu.x + 4, g_ctx_menu.y + 4, g_ctx_menu.w, g_ctx_menu.h, 0x60000000);
    // Body
    graphics_fill_rect(g_ctx_menu.x, g_ctx_menu.y, g_ctx_menu.w, g_ctx_menu.h, COL_WIN_BODY);
    // Border
    graphics_fill_rect(g_ctx_menu.x, g_ctx_menu.y, g_ctx_menu.w, 1, 0xFF808080);
    graphics_fill_rect(g_ctx_menu.x, g_ctx_menu.y, 1, g_ctx_menu.h, 0xFF808080);
    graphics_fill_rect(g_ctx_menu.x, g_ctx_menu.y + g_ctx_menu.h - 1, g_ctx_menu.w, 1, 0xFF202020);
    graphics_fill_rect(g_ctx_menu.x + g_ctx_menu.w - 1, g_ctx_menu.y, 1, g_ctx_menu.h, 0xFF202020);
    
    for(int i=0; i<g_ctx_menu.count; i++) {
        int iy = g_ctx_menu.y + 2 + (i * 20);
        bool hover = rect_contains(g_ctx_menu.x, iy, g_ctx_menu.w, 20, mouse.x, mouse.y);
        if (hover) graphics_fill_rect(g_ctx_menu.x + 2, iy, g_ctx_menu.w - 4, 20, COL_ACCENT);
        
        graphics_draw_string_scaled(g_ctx_menu.x + 10, iy + 6, g_ctx_menu.items[i].label, 
            hover ? COL_WHITE : COL_BLACK, hover ? COL_ACCENT : COL_WIN_BODY, 1);
    }
}

// --- Drawing ---

static void graphics_fill_rect_gradient(int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
    for (int i = 0; i < h; i++) {
        graphics_fill_rect(x, y+i, w, 1, (i < h/2) ? c1 : c2);
    }
}

static void draw_bevel_box(int x, int y, int w, int h, bool sunk) {
    graphics_fill_rect(x, y, w, h, COL_BTN_FACE);
    uint32_t tl = sunk ? COL_BTN_SHADOW : COL_BTN_HILIGHT;
    uint32_t br = sunk ? COL_BTN_HILIGHT : COL_BTN_SHADOW;
    graphics_fill_rect(x, y, w, 1, tl);
    graphics_fill_rect(x, y, 1, h, tl);
    graphics_fill_rect(x, y+h-1, w, 1, br);
    graphics_fill_rect(x+w-1, y, 1, h, br);
}

static void draw_string_bounded(int x, int y, int max_width, const char* text,
                                uint32_t fg, uint32_t bg, int scale) {
    if (!text || max_width <= 0 || scale <= 0) return;
    int max_chars = max_width / (8 * scale);
    if (max_chars <= 0) return;
    char clipped[64];
    int i = 0;
    while (text[i] && i < max_chars && i + 1 < (int)sizeof(clipped)) {
        clipped[i] = text[i];
        i++;
    }
    clipped[i] = 0;
    graphics_draw_string_scaled(x, y, clipped, fg, bg, scale);
}

static void draw_window_border(int x, int y, int w, int h) {
    graphics_fill_rect(x, y, w, h, COL_WIN_BODY);
    graphics_fill_rect(x, y, w, 1, 0xFF808080);
    graphics_fill_rect(x, y, 1, h, 0xFF808080);
    graphics_fill_rect(x, y+h-1, w, 1, 0xFF202020);
    graphics_fill_rect(x+w-1, y, 1, h, 0xFF202020);
}

static void draw_icon_bitmap(int x, int y, const uint8_t bitmap[24][24]) {
    for (int ry=0; ry<24; ry++) {
        for (int rx=0; rx<24; rx++) {
            uint8_t c = bitmap[ry][rx];
            if (c != 0) {
                uint32_t col = 0;
                switch(c) {
                    case 1: col = 0xFF000000; break;
                    case 2: col = 0xFF444444; break;
                    case 3: col = 0xFF888888; break;
                    case 4: col = 0xFFFFFFFF; break;
                    case 5: col = 0xFFFFCC00; break;
                    case 6: col = 0xFF0000AA; break;
                    case 7: col = 0xFF00AA00; break;
                    case 8: col = 0xFFAA0000; break;
                }
                graphics_put_pixel(x+rx, y+ry, col);
            }
        }
    }
}

static void draw_wallpaper(void) {
    for (int y = 0; y < screen_h; y++) {
        int r = 0;
        int g = 20 + (y * 80 / screen_h);
        int b = 60 + (y * 140 / screen_h);
        uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;
        graphics_fill_rect(0, y, screen_w, 1, color);
    }
    graphics_fill_rect(0, screen_h - 100, screen_w, 100, 0xFFD2B48C);
    
    rand_state = g_wallpaper_seed;
    for (int i = 0; i < 15; i++) {
        int cx = fast_rand() % screen_w;
        int ch = 30 + (fast_rand() % 50);
        int cw = 10 + (fast_rand() % 30);
        int cy = screen_h - 100 - ch + 10;
        uint32_t ccol = (fast_rand() % 2) ? 0xFFFF7F50 : 0xFFFF69B4; 
        graphics_fill_rect(cx, cy, cw, ch, ccol);
    }
    
    rand_state = (timer_get_ticks() / 10) + 100;
    for (int i = 0; i < 15; i++) {
        int bx = fast_rand() % screen_w;
        int by = fast_rand() % (screen_h - 100);
        graphics_fill_rect(bx, by, 4, 4, 0x80FFFFFF); 
    }
}

static void render_paint_app(Window* w) {
    int cx = w->x + 6;
    int cy = w->y + WIN_CAPTION_H + 6;
    int cw = w->w - 12;

    int tool_h = 40;
    graphics_fill_rect(cx, cy, cw, tool_h, 0xFFE0E0E0);

    uint32_t colors[] = {
        COL_BLACK, COL_WHITE, 0xFFFF0000, 0xFF00FF00,
        0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF
    };
    for (int i = 0; i < 8; i++) {
        int px = cx + 5 + i * 30;
        int py = cy + 5;
        graphics_fill_rect(px, py, 25, 25, colors[i]);
        if (w->state.paint.current_color == colors[i]) {
            graphics_fill_rect(px, py + 26, 25, 3, COL_BLACK);
        }
    }

    int save_x = w->x + w->w - 70;
    int save_y = w->y + WIN_CAPTION_H + 11;
    draw_bevel_box(save_x, save_y, 58, 25, false);
    graphics_draw_string_scaled(save_x + 12, save_y + 8, "SAVE",
                                COL_BLACK, COL_BTN_FACE, 1);

    draw_string_bounded(cx + 5, cy + 34, cw - 10,
                        w->state.paint.status,
                        w->state.paint.dirty ? 0xFFAA5500 : 0xFF226622,
                        COL_WIN_BODY, 1);

    int canvas_x, canvas_y, canvas_w, canvas_h;
    paint_canvas_rect(w, &canvas_x, &canvas_y, &canvas_w, &canvas_h);
    graphics_fill_rect(canvas_x - 2, canvas_y - 2,
                       canvas_w + 4, canvas_h + 4, 0xFF555555);
    for (int py = 0; py < PAINT_CANVAS_H; py++) {
        int y0 = canvas_y + (py * canvas_h) / PAINT_CANVAS_H;
        int y1 = canvas_y + ((py + 1) * canvas_h) / PAINT_CANVAS_H;
        for (int px = 0; px < PAINT_CANVAS_W; px++) {
            int x0 = canvas_x + (px * canvas_w) / PAINT_CANVAS_W;
            int x1 = canvas_x + ((px + 1) * canvas_w) / PAINT_CANVAS_W;
            graphics_fill_rect(x0, y0, x1 - x0, y1 - y0,
                               w->state.paint.pixels[
                                   py * PAINT_CANVAS_W + px]);
            if (canvas_w >= PAINT_CANVAS_W * 5) {
                graphics_fill_rect(x0, y0, x1 - x0, 1, 0xFFDDDDDD);
                graphics_fill_rect(x0, y0, 1, y1 - y0, 0xFFDDDDDD);
            }
        }
    }
}

static void render_assistant_app(Window* w) {
    AssistantState* state = &w->state.assistant;
    int cx = w->x + 2;
    int cy = w->y + WIN_CAPTION_H + 2;
    int cw = w->w - 4;
    int ch = w->h - WIN_CAPTION_H - 4;

    uint32_t header = 0xFF102A43;
    graphics_fill_rect(cx + 2, cy + 2, cw - 4, 40, header);
    draw_icon_bitmap(cx + 8, cy + 10, ICON_INFO);
    graphics_draw_string_scaled(cx + 40, cy + 8, "AI Assistant", COL_WHITE, header, 1);
    graphics_draw_string_scaled(cx + 40, cy + 22, "OFFLINE BASIC AI", 0xFF66D9EF, header, 1);
    graphics_fill_rect(cx + cw - 80, cy + 17, 6, 6, 0xFF38D66B);

    draw_bevel_box(cx + cw - 66, cy + 10, 54, 22, false);
    graphics_draw_string_scaled(cx + cw - 59, cy + 17, "CLEAR", COL_BLACK, COL_BTN_FACE, 1);

    int chat_x = cx + 8;
    int chat_y = cy + 48;
    int chat_w = cw - 16;
    int chat_h = ch - 94;
    draw_bevel_box(chat_x, chat_y, chat_w, chat_h, true);
    graphics_fill_rect(chat_x + 2, chat_y + 2, chat_w - 4, chat_h - 4, COL_WHITE);

    int visible_lines = (chat_h - 8) / 18;
    int start = state->line_count - visible_lines;
    if (start < 0) start = 0;
    int row = 0;
    for (int i = start; i < state->line_count; i++, row++) {
        bool user = state->roles[i] == ASSISTANT_ROLE_USER;
        uint32_t bg = user ? 0xFFE7F0FF : 0xFFE8FFF0;
        uint32_t label = user ? 0xFF1959A6 : 0xFF087A3E;
        int line_y = chat_y + 4 + row * 18;
        graphics_fill_rect(chat_x + 4, line_y, chat_w - 8, 16, bg);
        graphics_draw_string_scaled(chat_x + 8, line_y + 4, user ? "YOU" : "AI", label, bg, 1);
        draw_string_bounded(chat_x + 42, line_y + 4, chat_w - 50,
                            state->lines[i], COL_BLACK, bg, 1);
    }

    int input_y = cy + ch - 38;
    int input_w = cw - 90;
    draw_bevel_box(cx + 10, input_y, input_w, 28, true);
    graphics_fill_rect(cx + 12, input_y + 2, input_w - 4, 24, COL_WHITE);

    int max_chars = (input_w - 12) / 8;
    int offset = state->input_len > max_chars ? state->input_len - max_chars : 0;
    const char* visible_input = state->input + offset;
    if (state->input_len == 0) {
        graphics_draw_string_scaled(cx + 16, input_y + 10, "Ask me something...", 0xFF777777, COL_WHITE, 1);
    } else {
        draw_string_bounded(cx + 16, input_y + 10, input_w - 12,
                            visible_input, COL_BLACK, COL_WHITE, 1);
    }
    if ((timer_get_ticks() / 15) % 2) {
        int visible_len = state->input_len - offset;
        graphics_fill_rect(cx + 16 + visible_len * 8, input_y + 9, 2, 10, COL_ACCENT);
    }

    bool send_pressed = rect_contains(cx + cw - 70, input_y, 60, 28, mouse.x, mouse.y) &&
                        mouse.left_button;
    draw_bevel_box(cx + cw - 70, input_y, 60, 28, send_pressed);
    graphics_draw_string_scaled(cx + cw - 58, input_y + 10, "SEND", COL_BLACK, COL_BTN_FACE, 1);
}

static void render_window(Window* w) {
    Theme* t = &themes[current_theme_idx];
    if (!w || !w->visible || w->minimized) return;

    if (!w->maximized) graphics_fill_rect_alpha(w->x+6, w->y+6, w->w, w->h, 0x000000, 60);

    if (t->is_glass) {
        graphics_fill_rect_alpha(w->x-2, w->y-2, w->w+4, w->h+4, t->win_border, 100);
        graphics_fill_rect(w->x, w->y, w->w, w->h, t->win_body);
    } else {
        draw_window_border(w->x, w->y, w->w, w->h);
    }

    uint32_t tc = w->focused ? t->win_title_active : t->win_title_inactive;
    if (t->is_glass) {
        graphics_fill_rect_gradient(w->x, w->y, w->w, WIN_CAPTION_H, tc, tc + 0x00202020);
    } else {
        graphics_fill_rect(w->x+2, w->y+2, w->w-4, WIN_CAPTION_H-2, tc);
    }
    draw_string_bounded(w->x+8, w->y+6, w->w-82, w->title, COL_WHITE, tc, 1);

    int bx = w->x + w->w - 24;
    draw_bevel_box(bx, w->y+4, 18, 18, false);
    graphics_draw_char(bx+5, w->y+9, 'X', COL_BLACK, COL_BTN_FACE);

    int mx = bx - 22;
    draw_bevel_box(mx, w->y+4, 18, 18, false);
    graphics_draw_char(mx+5, w->y+9, '#', COL_BLACK, COL_BTN_FACE);

    int mn = mx - 22;
    draw_bevel_box(mn, w->y+4, 18, 18, false);
    graphics_draw_char(mn+5, w->y+9, '_', COL_BLACK, COL_BTN_FACE);

    int cx = w->x+2; int cy = w->y+WIN_CAPTION_H+2;
    int cw = w->w-4; int ch = w->h-WIN_CAPTION_H-4;
    graphics_fill_rect(cx, cy, cw, ch, COL_WIN_BODY);

    if (w->type == APP_ASSISTANT) {
        render_assistant_app(w);
    }
    else if (w->type == APP_WELCOME) {
        graphics_draw_string_scaled(cx+18, cy+16, "Welcome to NostaluxOS", COL_ACCENT, COL_WIN_BODY, 2);
        graphics_draw_string_scaled(cx+20, cy+48, "A handmade x86-64 desktop.", COL_BLACK, COL_WIN_BODY, 1);
        graphics_draw_string_scaled(cx+20, cy+66, "Move windows, explore apps, and build.", COL_BLACK, COL_WIN_BODY, 1);
        graphics_draw_string_scaled(cx+20, cy+90, "Need a hand? Meet your offline helper.", 0xFF555555, COL_WIN_BODY, 1);
        draw_bevel_box(cx+20, cy+118, cw-40, 28, false);
        graphics_draw_string_scaled(cx+66, cy+128, "OPEN AI ASSISTANT", COL_BLACK, COL_BTN_FACE, 1);
    }
    else if (w->type == APP_NOTEPAD) {
        NotepadState* state = &w->state.notepad;
        graphics_fill_rect(cx + 2, cy + 2, cw - 4, 38, 0xFFE0E0E0);
        draw_string_bounded(cx + 8, cy + 10, cw - 160,
                            state->filename,
                            COL_BLACK, 0xFFE0E0E0, 1);
        draw_string_bounded(
            cx + 8, cy + 25, cw - 16, state->status,
            text_contains_ci(state->status, "failed")
                ? 0xFFAA0000
                : state->dirty ? 0xFFAA5500 : 0xFF226622,
            0xFFE0E0E0, 1);
        draw_bevel_box(w->x + w->w - 70,
                       w->y + WIN_CAPTION_H + 5, 58, 22, false);
        graphics_draw_string_scaled(w->x + w->w - 58,
                                    w->y + WIN_CAPTION_H + 12,
                                    "SAVE", COL_BLACK, COL_BTN_FACE, 1);

        int editor_y = cy + 42;
        int editor_h = ch - 44;
        draw_bevel_box(cx + 2, editor_y, cw - 4, editor_h, true);
        graphics_fill_rect(cx + 4, editor_y + 2, cw - 8, editor_h - 4,
                           COL_WHITE);
        int max_cols = (cw - 12) / 8;
        int max_rows = (editor_h - 8) / 10;
        if (max_cols < 1) max_cols = 1;
        if (max_rows < 1) max_rows = 1;
        int total_rows = wrapped_text_line_count(state->buffer, max_cols);
        int first_row = total_rows > max_rows ? total_rows - max_rows : 0;
        int col = 0;
        int row = 0;
        for (int i = 0; i < state->length; i++) {
            char c = state->buffer[i];
            if (c == '\r') continue;
            if (c == '\n') {
                row++;
                col = 0;
                continue;
            }
            if (col >= max_cols) {
                row++;
                col = 0;
            }
            if (row >= first_row && row < first_row + max_rows) {
                graphics_draw_char(cx + 6 + col * 8,
                                   editor_y + 4 + (row - first_row) * 10,
                                   c, COL_BLACK, COL_WHITE);
            }
            col++;
        }
        wrapped_text_cursor(state->buffer, max_cols, &row, &col);
        if ((timer_get_ticks() / 15) % 2) {
            if (row >= first_row && row < first_row + max_rows)
                graphics_fill_rect(cx + 6 + col * 8,
                                   editor_y + 4 + (row - first_row) * 10,
                                   2, 10, COL_BLACK);
        }
    } 
    else if (w->type == APP_PAINT) {
        render_paint_app(w);
    }
    else if (w->type == APP_ABOUT) {
        graphics_draw_string_scaled(cx+20, cy+20, "NostaluxOS", COL_ACCENT, COL_WIN_BODY, 3);
        graphics_draw_string_scaled(cx+20, cy+50, "Development build", COL_BLACK, COL_WIN_BODY, 1);
        graphics_draw_string_scaled(cx+20, cy+70, "Built by NostaluxOS contributors", COL_BLACK, COL_WIN_BODY, 1);
        
        draw_bevel_box(cx+10, cy+100, cw-20, 100, true);
        graphics_fill_rect(cx+12, cy+102, cw-24, 96, COL_WHITE);
        
        char storage_credit[64] = "Storage: ";
        str_append(storage_credit, sizeof(storage_credit),
                   storage_short_label());
        const char* credits[] = {
            "Kernel: freestanding x86-64",
            storage_credit,
            "Tasks: kernel cooperative, apps preemptive",
            "Desktop: kernel-mode GUI",
            "Status: active development"
        };
        for(int i=0; i<5; i++) {
            graphics_draw_string_scaled(cx+16, cy+106 + (i*14), credits[i], COL_BLACK, COL_WHITE, 1);
        }
    }
    else if (w->type == APP_MINESWEEPER) {
        MineState* ms = &w->state.mine;
        int grid_x = w->x + (w->w - MINE_GRID_PIXELS) / 2;
        int grid_y = w->y + WIN_CAPTION_H + 34;
        char flags[24] = "Flags: ";
        char count[12];
        int_to_str(ms->flags_placed, count);
        str_append(flags, sizeof(flags), count);
        draw_string_bounded(cx + 10, cy + 10, cw - 20, flags,
                            COL_BLACK, COL_WIN_BODY, 1);
        for (int r = 0; r < MINE_GRID_H; r++) {
            for (int c = 0; c < MINE_GRID_W; c++) {
                int cell_x = grid_x + c * MINE_CELL_SIZE;
                int cell_y = grid_y + r * MINE_CELL_SIZE;
                uint8_t view = ms->view[r][c];
                if (view == 0 || view == 2) {
                    draw_bevel_box(cell_x, cell_y, MINE_CELL_SIZE, MINE_CELL_SIZE, false);
                    if (view == 2)
                        graphics_draw_char(cell_x + 5, cell_y + 5, 'F',
                                           0xFFCC0000, COL_BTN_FACE);
                } else {
                    graphics_fill_rect(cell_x, cell_y, MINE_CELL_SIZE,
                                       MINE_CELL_SIZE, 0xFFDDDDDD);
                    graphics_fill_rect(cell_x, cell_y, MINE_CELL_SIZE, 1, 0xFF888888);
                    graphics_fill_rect(cell_x, cell_y, 1, MINE_CELL_SIZE, 0xFF888888);
                    if (ms->grid[r][c] == 9) {
                        graphics_draw_char(cell_x + 5, cell_y + 5, '*',
                                           0xFFCC0000, 0xFFDDDDDD);
                    } else if (ms->grid[r][c] > 0) {
                        char number = (char)('0' + ms->grid[r][c]);
                        graphics_draw_char(cell_x + 5, cell_y + 5, number,
                                           0xFF0000AA, 0xFFDDDDDD);
                    }
                }
            }
        }
        if (ms->game_over || ms->victory) {
            const char* result = ms->victory ? "You win! Click to restart."
                                             : "Mine hit! Click to restart.";
            draw_string_bounded(cx + 10, grid_y + MINE_GRID_PIXELS + 8,
                                cw - 20, result,
                                ms->victory ? 0xFF008800 : 0xFFCC0000,
                                COL_WIN_BODY, 1);
        }
    }
    else if (w->type == APP_TICTACTOE) {
        TicTacToeState* s = &w->state.ttt;
        int gx = cx + 8; int gy = cy + 8;
        // Draw grid
        for (int i=1;i<3;i++) graphics_fill_rect(gx + i*60, gy, 4, 180, COL_BLACK);
        for (int i=1;i<3;i++) graphics_fill_rect(gx, gy + i*60, 180, 4, COL_BLACK);
        
        for (int r=0;r<3;r++) for(int c=0;c<3;c++) {
            int cell_x = gx + c*60 + 20;
            int cell_y = gy + r*60 + 15;
            if (s->board[r][c] == 1) graphics_draw_string_scaled(cell_x, cell_y, "X", 0xFF0000AA, COL_WIN_BODY, 4);
            else if (s->board[r][c] == 2) graphics_draw_string_scaled(cell_x, cell_y, "O", 0xFF00AA00, COL_WIN_BODY, 4);
        }
        
        if (s->winner != 0) {
            const char* msg = (s->winner==1)?"X Wins!":(s->winner==2)?"O Wins!":"Draw!";
            graphics_draw_string_scaled(gx, gy+185, msg, 0xFFFF0000, COL_WIN_BODY, 2);
            draw_bevel_box(gx+10, gy+205, 100, 24, false);
            graphics_draw_string_scaled(gx+25, gy+210, "Restart", COL_BLACK, COL_BTN_FACE, 1);
        } else {
            graphics_draw_string_scaled(gx, gy+185, (s->turn==1)?"Turn: X":"Turn: O", COL_BLACK, COL_WIN_BODY, 1);
        }
    }
    else if (w->type == APP_IMAGEVIEW) {
        ImageViewState* state = &w->state.img;
        int ix = cx + 8;
        int iy = cy + 24;
        int iw = cw - 16;
        int ih = ch - 62;
        int button_y = cy + ch - 30;

        graphics_fill_rect(ix, iy, iw, ih, 0xFF202020);

        char image_label[64];
        image_label[0] = 0;
        if (state->filename[0]) {
            str_copy(image_label, sizeof(image_label), state->filename);
            if (state->valid) {
                char number[16];
                str_append(image_label, sizeof(image_label), "  ");
                int_to_str((int)state->image.width, number);
                str_append(image_label, sizeof(image_label), number);
                str_append(image_label, sizeof(image_label), "x");
                int_to_str((int)state->image.height, number);
                str_append(image_label, sizeof(image_label), number);
            }
        } else {
            str_copy(image_label, sizeof(image_label), "Filesystem BMP viewer");
        }
        draw_string_bounded(ix, cy + 8, iw, image_label, COL_BLACK, COL_WIN_BODY, 1);

        if (state->valid && state->image.width > 0 && state->image.height > 0) {
            int draw_w = (int)state->image.width;
            int draw_h = (int)state->image.height;
            if (state->fit_to_window) {
                if ((uint64_t)iw * state->image.height <=
                    (uint64_t)ih * state->image.width) {
                    draw_w = iw;
                    draw_h = (int)(((uint64_t)state->image.height * (uint32_t)iw) /
                                   state->image.width);
                } else {
                    draw_h = ih;
                    draw_w = (int)(((uint64_t)state->image.width * (uint32_t)ih) /
                                   state->image.height);
                }
                if (draw_w < 1) draw_w = 1;
                if (draw_h < 1) draw_h = 1;
            }

            int draw_x = ix + (iw - draw_w) / 2;
            int draw_y = iy + (ih - draw_h) / 2;
            for (uint32_t source_y = 0; source_y < state->image.height; source_y++) {
                int y0 = draw_y + (int)(((uint64_t)source_y * (uint32_t)draw_h) /
                                        state->image.height);
                int y1 = draw_y + (int)(((uint64_t)(source_y + 1u) * (uint32_t)draw_h) /
                                        state->image.height);
                int clipped_y0 = y0 < iy ? iy : y0;
                int clipped_y1 = y1 > iy + ih ? iy + ih : y1;
                if (clipped_y1 <= clipped_y0) continue;
                for (uint32_t source_x = 0; source_x < state->image.width; source_x++) {
                    uint32_t color;
                    if (!bmp_get_pixel(&state->image, source_x, source_y, &color)) continue;
                    int x0 = draw_x + (int)(((uint64_t)source_x * (uint32_t)draw_w) /
                                            state->image.width);
                    int x1 = draw_x + (int)(((uint64_t)(source_x + 1u) * (uint32_t)draw_w) /
                                            state->image.width);
                    int clipped_x0 = x0 < ix ? ix : x0;
                    int clipped_x1 = x1 > ix + iw ? ix + iw : x1;
                    if (clipped_x1 > clipped_x0) {
                        graphics_fill_rect(clipped_x0, clipped_y0,
                                           clipped_x1 - clipped_x0,
                                           clipped_y1 - clipped_y0, color);
                    }
                }
            }
        } else {
            draw_string_bounded(ix + 10, iy + ih / 2 - 4, iw - 20,
                                state->status, COL_WHITE, 0xFF202020, 1);
        }

        draw_bevel_box(cx + 8, button_y, 58, 22, false);
        graphics_draw_string_scaled(cx + 20, button_y + 7, "Prev", COL_BLACK, COL_BTN_FACE, 1);
        draw_bevel_box(cx + 72, button_y, 58, 22, false);
        graphics_draw_string_scaled(cx + 84, button_y + 7, "Next", COL_BLACK, COL_BTN_FACE, 1);
        draw_bevel_box(cx + 136, button_y, 70, 22, state->fit_to_window);
        graphics_draw_string_scaled(cx + 150, button_y + 7,
                                    state->fit_to_window ? "Fit" : "1:1",
                                    COL_BLACK, COL_BTN_FACE, 1);
        draw_string_bounded(cx + 214, button_y + 7, cw - 222,
                            state->status, 0xFF555555, COL_WIN_BODY, 1);
    }
    else if (w->type == APP_BROWSER) {
        BrowserState* state = &w->state.browser;
        uint64_t browser_now = timer_get_ticks();
        if (browser_address_is_dynamic(state->address) &&
            browser_now - state->last_refresh_tick >= 100u) {
            int previous_scroll = state->scroll;
            browser_load(w);
            int max_scroll = browser_max_scroll(w, state);
            if (previous_scroll < 0) previous_scroll = 0;
            state->scroll =
                previous_scroll > max_scroll ? max_scroll : previous_scroll;
        }
        int field_w = cw - 44;
        uint32_t field_bg =
            state->address_select_all ? 0xFF2255AA : COL_WHITE;
        uint32_t field_fg =
            state->address_select_all ? COL_WHITE : COL_BLACK;
        draw_bevel_box(cx + 2, cy + 2, field_w, 24, true);
        graphics_fill_rect(cx + 4, cy + 4, field_w - 4, 20, field_bg);
        int address_chars = (field_w - 10) / 8;
        int address_offset = state->address_len > address_chars
                           ? state->address_len - address_chars : 0;
        draw_string_bounded(cx + 7, cy + 9, field_w - 10,
                            state->address + address_offset,
                            field_fg, field_bg, 1);
        if ((timer_get_ticks() / 15) % 2) {
            int shown = state->address_len - address_offset;
            graphics_fill_rect(cx + 7 + shown * 8, cy + 8,
                               2, 11, field_fg);
        }
        draw_bevel_box(cx + cw - 38, cy + 2, 34, 24, false);
        graphics_draw_string_scaled(cx + cw - 31, cy + 9, "GO",
                                    COL_BLACK, COL_BTN_FACE, 1);

        int content_y = cy + 30;
        int content_h = ch - 60;
        draw_bevel_box(cx + 2, content_y, cw - 4, content_h, true);
        graphics_fill_rect(cx + 4, content_y + 2, cw - 8,
                           content_h - 4, COL_WHITE);
        int max_cols = browser_max_columns(w);
        int visible_rows = browser_visible_rows(w);
        int row = 0;
        int column = 0;
        for (int i = 0; state->content[i]; i++) {
            char current = state->content[i];
            if (current == '\r') continue;
            if (current == '\n') {
                row++;
                column = 0;
                continue;
            }
            if (column >= max_cols) {
                row++;
                column = 0;
            }
            if (row >= state->scroll &&
                row < state->scroll + visible_rows) {
                graphics_draw_char(cx + 10 + column * 8,
                                   content_y + 6 +
                                       (row - state->scroll) * 10,
                                   current, COL_BLACK, COL_WHITE);
            }
            column++;
        }

        int button_y = cy + ch - 24;
        draw_bevel_box(cx + 8, button_y, 42, 20, false);
        graphics_draw_string_scaled(cx + 17, button_y + 6, "UP",
                                    COL_BLACK, COL_BTN_FACE, 1);
        draw_bevel_box(cx + 56, button_y, 50, 20, false);
        graphics_draw_string_scaled(cx + 63, button_y + 6, "DOWN",
                                    COL_BLACK, COL_BTN_FACE, 1);
        draw_bevel_box(cx + 112, button_y, 66, 20, false);
        graphics_draw_string_scaled(cx + 117, button_y + 6, "REFRESH",
                                    COL_BLACK, COL_BTN_FACE, 1);
        draw_string_bounded(cx + 184, button_y + 6, cw - 192,
                            state->status, 0xFF226622, COL_WIN_BODY, 1);
    }
    else if (w->type == APP_TASKMGR) {
        int rows = taskmgr_visible_rows(w);
        size_t total = scheduler_task_count();
        taskmgr_normalize_page(w, total, rows);
        struct scheduler_task_info tasks[TASKMGR_PAGE_CAPACITY];
        size_t count = scheduler_snapshot_tasks_from(
            w->state.taskmgr.page_offset, tasks, (size_t)rows);
        graphics_draw_string_scaled(cx + 10, cy + 10, "PID", COL_BLACK, COL_WIN_BODY, 1);
        graphics_draw_string_scaled(cx + 70, cy + 10, "Name", COL_BLACK, COL_WIN_BODY, 1);
        graphics_draw_string_scaled(cx + cw - 150, cy + 10, "Mode", COL_BLACK, COL_WIN_BODY, 1);
        graphics_draw_string_scaled(cx + cw - 82, cy + 10, "State", COL_BLACK, COL_WIN_BODY, 1);
        graphics_fill_rect(cx + 10, cy + 25, cw - 20, 1, 0xFF888888);

        int list_y = cy + 36;
        for (size_t i = 0; i < count; i++) {
            bool selected = w->state.taskmgr.selected_pid == tasks[i].id;
            uint32_t row_bg = selected ? 0xFFCCCCFF : COL_WIN_BODY;
            if (selected) graphics_fill_rect(cx + 8, list_y - 2, cw - 16, 18, row_bg);

            char pid_string[24];
            uint64_to_str(tasks[i].id, pid_string);
            draw_string_bounded(cx + 10, list_y, 52, pid_string,
                                COL_BLACK, row_bg, 1);
            draw_string_bounded(cx + 70, list_y, cw - 228, tasks[i].name,
                                COL_BLACK, row_bg, 1);
            draw_string_bounded(cx + cw - 150, list_y, 60,
                                tasks[i].is_user ? "User" : "Kernel",
                                COL_BLACK, row_bg, 1);
            const char* task_state = tasks[i].state == TASK_DEAD
                                   ? "Dead"
                                   : (tasks[i].is_current ? "Running" : "Ready");
            draw_string_bounded(cx + cw - 82, list_y, 72, task_state,
                                COL_BLACK, row_bg, 1);
            list_y += 20;
        }

        draw_string_bounded(cx + 10, cy + ch - 52, cw - 20,
                            total <= 1
                                ? "No background tasks; current kernel task only."
                                : "Desktop apps share the kernel/desktop task.",
                            0xFF555555, COL_WIN_BODY, 1);
        draw_string_bounded(cx + 10, cy + ch - 38, cw - 20,
                            w->state.taskmgr.status,
                            0xFF225588, COL_WIN_BODY, 1);

        int button_y = cy + ch - 24;
        draw_bevel_box(cx + 10, button_y, 44, 20, false);
        graphics_draw_string_scaled(cx + 16, button_y + 6, "Prev",
                                    COL_BLACK, COL_BTN_FACE, 1);
        draw_bevel_box(cx + 60, button_y, 44, 20, false);
        graphics_draw_string_scaled(cx + 66, button_y + 6, "Next",
                                    COL_BLACK, COL_BTN_FACE, 1);
        draw_bevel_box(cx + 110, button_y, 70, 20, false);
        graphics_draw_string_scaled(cx + 116, button_y + 6, "End Task",
                                    COL_BLACK, COL_BTN_FACE, 1);

        char task_footer[64] = "Tasks ";
        char task_number[24];
        size_t first = total == 0 ? 0 : w->state.taskmgr.page_offset + 1;
        size_t last = w->state.taskmgr.page_offset + count;
        uint64_to_str((uint64_t)first, task_number);
        str_append(task_footer, sizeof(task_footer), task_number);
        str_append(task_footer, sizeof(task_footer), "-");
        uint64_to_str((uint64_t)last, task_number);
        str_append(task_footer, sizeof(task_footer), task_number);
        str_append(task_footer, sizeof(task_footer), " of ");
        uint64_to_str((uint64_t)total, task_number);
        str_append(task_footer, sizeof(task_footer), task_number);
        draw_string_bounded(cx + 190, button_y + 6, cw - 200, task_footer,
                            0xFF555555, COL_WIN_BODY, 1);
    }
    else if (w->type == APP_SYSMON) {
        SysMonState* state = &w->state.sysmon;
        char cpu_label[48] = "Measured CPU busy time: ";
        char number[24];
        int_to_str(state->cpu_percent, number);
        str_append(cpu_label, sizeof(cpu_label), number);
        str_append(cpu_label, sizeof(cpu_label), "%");
        graphics_draw_string_scaled(cx + 10, cy + 8, cpu_label, COL_BLACK, COL_WIN_BODY, 1);

        int graph_x = cx + 10;
        int graph_y = cy + 24;
        int graph_w = cw - 20;
        int graph_h = 60;
        graphics_fill_rect(graph_x, graph_y, graph_w, graph_h, COL_BLACK);
        int graph_step = graph_w / SYSMON_HIST;
        if (graph_step < 2) graph_step = 2;
        for (int i = 0; i < SYSMON_HIST; i++) {
            int idx = (state->head - i + SYSMON_HIST) % SYSMON_HIST;
            int val = state->cpu_hist[idx];
            if (val > 100) val = 100;
            int bar_h = (val * graph_h) / 100;
            if (val > 0 && bar_h == 0) bar_h = 1;
            int bx = graph_x + graph_w - graph_step * (i + 1);
            if (bx >= graph_x) {
                graphics_fill_rect(bx, graph_y + graph_h - bar_h,
                                   graph_step - 1, bar_h, 0xFF00FF00);
            }
        }

        char physical_label[48] = "Physical usable: ";
        uint64_to_str((uint64_t)state->memory_total_kb, number);
        str_append(physical_label, sizeof(physical_label), number);
        str_append(physical_label, sizeof(physical_label), " KB");
        draw_string_bounded(cx + 10, cy + 92, cw - 20, physical_label,
                            0xFF555555, COL_WIN_BODY, 1);

        char reserved_label[48] = "Fixed reserved: ";
        uint64_to_str((uint64_t)state->memory_reserved_kb, number);
        str_append(reserved_label, sizeof(reserved_label), number);
        str_append(reserved_label, sizeof(reserved_label), " KB");
        draw_string_bounded(cx + 10, cy + 106, cw - 20, reserved_label,
                            0xFF555555, COL_WIN_BODY, 1);

        char heap_capacity_label[48] = "Heap capacity: ";
        uint64_to_str((uint64_t)state->memory_managed_kb, number);
        str_append(heap_capacity_label, sizeof(heap_capacity_label), number);
        str_append(heap_capacity_label, sizeof(heap_capacity_label), " KB");
        draw_string_bounded(cx + 10, cy + 120, cw - 20,
                            heap_capacity_label,
                            0xFF555555, COL_WIN_BODY, 1);

        int heap_percent = 0;
        if (state->memory_managed_kb > 0) {
            heap_percent = (int)(
                ((uint64_t)state->memory_heap_committed_kb * 100u) /
                state->memory_managed_kb);
            if (heap_percent > 100) heap_percent = 100;
        }
        char heap_commit_label[64] = "Heap commit: ";
        uint64_to_str((uint64_t)state->memory_heap_committed_kb, number);
        str_append(heap_commit_label, sizeof(heap_commit_label), number);
        str_append(heap_commit_label, sizeof(heap_commit_label), " KB (");
        int_to_str(heap_percent, number);
        str_append(heap_commit_label, sizeof(heap_commit_label), number);
        str_append(heap_commit_label, sizeof(heap_commit_label), "% of cap)");
        draw_string_bounded(cx + 10, cy + 134, cw - 20,
                            heap_commit_label,
                            0xFF555555, COL_WIN_BODY, 1);

        char mapped_label[64] = "Mapped commit: ";
        uint64_to_str((uint64_t)state->memory_used_kb, number);
        str_append(mapped_label, sizeof(mapped_label), number);
        str_append(mapped_label, sizeof(mapped_label), "/");
        uint64_to_str((uint64_t)state->memory_mapped_kb, number);
        str_append(mapped_label, sizeof(mapped_label), number);
        str_append(mapped_label, sizeof(mapped_label), " KB ");
        int_to_str(state->mem_hist[state->head], number);
        str_append(mapped_label, sizeof(mapped_label), number);
        str_append(mapped_label, sizeof(mapped_label), "%");
        draw_string_bounded(cx + 10, cy + 148, cw - 20, mapped_label,
                            COL_BLACK, COL_WIN_BODY, 1);

        int memory_graph_y = cy + 162;
        int memory_graph_h = ch - 190;
        if (memory_graph_h > 70) memory_graph_h = 70;
        if (memory_graph_h < 18) memory_graph_h = 18;
        graphics_fill_rect(graph_x, memory_graph_y, graph_w,
                           memory_graph_h, COL_BLACK);
        for (int i = 0; i < SYSMON_HIST; i++) {
            int idx = (state->head - i + SYSMON_HIST) % SYSMON_HIST;
            int val = state->mem_hist[idx];
            if (val > 100) val = 100;
            int bar_h = (val * memory_graph_h) / 100;
            if (val > 0 && bar_h == 0) bar_h = 1;
            int bx = graph_x + graph_w - graph_step * (i + 1);
            if (bx >= graph_x) {
                graphics_fill_rect(bx,
                                   memory_graph_y + memory_graph_h - bar_h,
                                   graph_step - 1, bar_h, 0xFF3399FF);
            }
        }

        char uptime_label[48] = "Uptime: ";
        uint64_to_str(timer_get_uptime(), number);
        str_append(uptime_label, sizeof(uptime_label), number);
        str_append(uptime_label, sizeof(uptime_label), " seconds");
        draw_string_bounded(cx + 10, cy + ch - 18, cw - 20, uptime_label,
                            0xFF555555, COL_WIN_BODY, 1);
    }
    else if (w->type == APP_SETTINGS) {
        w->state.settings.wallpaper_enabled = g_wallpaper_enabled;
        w->state.settings.theme_id = current_theme_idx;
        graphics_draw_string_scaled(cx+10, cy+10, "Desktop Wallpaper:", COL_BLACK, COL_WIN_BODY, 1);
        bool on = w->state.settings.wallpaper_enabled;
        draw_bevel_box(cx+10, cy+30, 140, 30, on);
        const char* lbl = on ? "Enabled (Random)" : "Disabled (Theme)";
        graphics_draw_string_scaled(cx+20, cy+40, lbl, COL_BLACK, COL_BTN_FACE, 1);
        graphics_draw_string_scaled(cx+10, cy+80, "System Theme:", COL_BLACK, COL_WIN_BODY, 1);
        draw_bevel_box(cx+10, cy+100, 140, 30, false);
        graphics_draw_string_scaled(cx+20, cy+110,
                                    current_theme_idx == 0 ? "Ocean Glass" : "Retro Grey",
                                    COL_BLACK, COL_BTN_FACE, 1);
        char storage_label[48] = "Storage: ";
        str_append(storage_label, sizeof(storage_label),
                   storage_compact_label());
        draw_string_bounded(cx + 10, cy + 136, cw - 20,
                            storage_label,
                            fs_backend_is_persistent()
                                ? 0xFF226622 : 0xFFAA0000,
                            COL_WIN_BODY, 1);
        draw_string_bounded(cx + 10, cy + 154, cw - 20,
                            w->state.settings.status,
                            (text_contains_ci(w->state.settings.status, "failed") ||
                             text_contains_ci(w->state.settings.status,
                                              "session only"))
                                ? 0xFFAA0000 : 0xFF226622,
                            COL_WIN_BODY, 1);
    }
    else if (w->type == APP_RUN) {
        graphics_draw_string_scaled(cx+12, cy+12, "App or shell command:", COL_BLACK, COL_WIN_BODY, 1);
        draw_bevel_box(cx+12, cy+32, cw-90, 26, true);
        graphics_fill_rect(cx+14, cy+34, cw-94, 22, COL_WHITE);
        draw_string_bounded(cx+18, cy+40, cw-104, w->state.run.cmd,
                            COL_BLACK, COL_WHITE, 1);
        draw_bevel_box(cx+cw-66, cy+32, 54, 26, false);
        graphics_draw_string_scaled(cx+cw-53, cy+41, "RUN", COL_BLACK, COL_BTN_FACE, 1);
        draw_string_bounded(cx+12, cy+70, cw-24,
                            w->state.run.status[0]
                                ? w->state.run.status
                                : "Apps launch directly; other input runs in Terminal.",
                            0xFF555555, COL_WIN_BODY, 1);
    }
    else if (w->type == APP_TERMINAL) {
        TerminalState* state = &w->state.term;
        draw_bevel_box(cx+2, cy+2, cw-4, ch-4, true);
        graphics_fill_rect(cx+4, cy+4, cw-8, ch-8, COL_BLACK);
        int history_y = cy + 6;
        int base_visible_lines = (ch - 26) / 10;
        if (base_visible_lines < 1) base_visible_lines = 1;
        if (base_visible_lines > GUI_TERM_LINES)
            base_visible_lines = GUI_TERM_LINES;
        int base_first = state->line_count > base_visible_lines
                       ? state->line_count - base_visible_lines : 0;
        bool has_hidden_history =
            state->history_truncated || base_first > 0;
        int history_notice_h = has_hidden_history ? 10 : 0;
        if (has_hidden_history) {
            draw_string_bounded(cx + 6, history_y, cw - 12,
                                state->history_truncated
                                    ? "[older terminal lines discarded]"
                                    : "[more terminal lines above]",
                                0xFFFFAA00, COL_BLACK, 1);
        }
        int visible_lines = (ch - 26 - history_notice_h) / 10;
        if (visible_lines < 1) visible_lines = 1;
        if (visible_lines > GUI_TERM_LINES) visible_lines = GUI_TERM_LINES;
        int first = state->line_count > visible_lines
                  ? state->line_count - visible_lines : 0;
        int row = 0;
        for (int i = first; i < state->line_count; i++, row++) {
            draw_string_bounded(cx + 6,
                                history_y + history_notice_h + row * 10,
                                cw - 12,
                                state->lines[i], 0xFF00FF00, COL_BLACK, 1);
        }
        int input_y = cy + ch - 16;
        graphics_draw_string_scaled(cx+6, input_y, "$ ", 0xFF00FF00, COL_BLACK, 1);
        int pw = 16;
        int input_chars = (cw - 12 - pw) / 8;
        if (input_chars < 1) input_chars = 1;
        int input_offset = state->input_len > input_chars
                         ? state->input_len - input_chars : 0;
        draw_string_bounded(cx+6+pw, input_y, cw-12-pw,
                            state->input + input_offset,
                            COL_WHITE, COL_BLACK, 1);
        if ((timer_get_ticks()/15)%2) {
            int shown = state->input_len - input_offset;
            graphics_fill_rect(cx+6+pw+(shown*8), input_y, 2, 8, 0xFF00FF00);
        }
    }
    else if (w->type == APP_CALC) {
        char buf[16];
        if (w->state.calc.error_code == CALC_ERROR_DIV_ZERO)
            str_copy(buf, sizeof(buf), "DIV/0");
        else if (w->state.calc.error_code == CALC_ERROR_OVERFLOW)
            str_copy(buf, sizeof(buf), "OVERFLOW");
        else
            int_to_str(w->state.calc.current_val, buf);
        draw_bevel_box(cx+10, cy+10, cw-20, 24, true);
        graphics_fill_rect(cx+12, cy+12, cw-24, 20, COL_WHITE);
        graphics_draw_string_scaled(cx+cw-14-(kstrlen_local(buf)*8), cy+16, buf, COL_BLACK, COL_WHITE, 1);
        const char* btns[] = {"7","8","9","/", "4","5","6","*", "1","2","3","-", "C","0","=","+"};
        for(int i=0; i<16; i++) {
            int bx_pos, by_pos;
            calc_button_rect(w, i, &bx_pos, &by_pos);
            bool h = rect_contains(bx_pos, by_pos, 35, 25, mouse.x, mouse.y);
            draw_bevel_box(bx_pos, by_pos, 35, 25, h&&mouse.left_button);
            graphics_draw_char(bx_pos+12, by_pos+8, btns[i][0], COL_BLACK, COL_BTN_FACE);
        }
    }
    else if (w->type == APP_FILES) {
        FileManagerState* state = &w->state.files;
        files_normalize(w);
        draw_bevel_box(cx+2, cy+2, cw-4, ch-4, true);
        graphics_fill_rect(cx+4, cy+4, cw-8, ch-8, COL_WHITE);
        const char* toolbar_labels[] = {
            "New", "Open", "Rename", "Delete", "Up", "Down"
        };
        int toolbar_x[] = {
            cx + 6, cx + 48, cx + 94, cx + 152, cx + 206, cx + 242
        };
        int toolbar_w[] = {38, 42, 54, 50, 32, 42};
        for (int i = 0; i < 6; i++) {
            draw_bevel_box(toolbar_x[i], cy + 4, toolbar_w[i], 22, false);
            graphics_draw_string_scaled(toolbar_x[i] + 5, cy + 11,
                                        toolbar_labels[i],
                                        COL_BLACK, COL_BTN_FACE, 1);
        }

        graphics_fill_rect(cx + 4, cy + 32, cw - 8, 18, 0xFFCCCCCC);
        graphics_draw_string_scaled(cx + 8, cy + 37, "Name",
                                    COL_BLACK, 0xFFCCCCCC, 1);
        draw_string_bounded(cx + 64, cy + 37, cw - 130,
                            storage_compact_label(),
                            fs_backend_is_persistent()
                                ? 0xFF226622 : 0xFFAA0000,
                            0xFFCCCCCC, 1);
        graphics_draw_string_scaled(cx + cw - 58, cy + 37, "Bytes",
                                    COL_BLACK, 0xFFCCCCCC, 1);
        size_t count = fs_file_count();
        int rows = files_visible_rows(w);
        for (int row_index = 0; row_index < rows; row_index++) {
            int file_index = state->scroll_offset + row_index;
            if (file_index >= (int)count) break;
            const struct fs_file* file = fs_file_at((size_t)file_index);
            if (!file) continue;
            int row_y = cy + 52 + row_index * 18;
            bool selected = file_index == state->selected_index;
            uint32_t row_bg = selected ? 0xFF000080 : COL_WHITE;
            if (selected)
                graphics_fill_rect(cx + 4, row_y, cw - 8, 18, row_bg);
            draw_string_bounded(cx + 10, row_y + 5, cw - 82, file->name,
                                selected ? COL_WHITE : COL_BLACK,
                                row_bg, 1);
            char size_label[16];
            int_to_str((int)file->size, size_label);
            draw_string_bounded(cx + cw - 58, row_y + 5, 50, size_label,
                                selected ? COL_WHITE : COL_BLACK,
                                row_bg, 1);
        }

        int footer_y = cy + ch - 24;
        if (state->renaming) {
            uint32_t rename_bg =
                state->rename_select_all ? 0xFF2255AA : COL_WHITE;
            uint32_t rename_fg =
                state->rename_select_all ? COL_WHITE : COL_BLACK;
            draw_bevel_box(cx + 6, footer_y, 170, 20, true);
            graphics_fill_rect(cx + 8, footer_y + 2, 166, 16, rename_bg);
            draw_string_bounded(cx + 10, footer_y + 6, 160,
                                state->rename_buffer,
                                rename_fg, rename_bg, 1);
            draw_string_bounded(cx + 184, footer_y + 6, cw - 192,
                                "Enter saves; Esc cancels",
                                0xFF555555, COL_WHITE, 1);
        } else {
            draw_string_bounded(cx + 8, footer_y + 6, cw - 80,
                                state->status,
                                0xFF225588, COL_WHITE, 1);
            char page_label[48] = "Files ";
            char number[16];
            int_to_str((int)count, number);
            str_append(page_label, sizeof(page_label), number);
            draw_string_bounded(cx + cw - 72, footer_y + 6, 64,
                                page_label, 0xFF555555, COL_WHITE, 1);
        }
    }

    if (!w->maximized)
        graphics_fill_rect(w->x + w->w - RESIZE_HANDLE, w->y + w->h - RESIZE_HANDLE,
                           RESIZE_HANDLE, RESIZE_HANDLE, 0xFF888888);
}

static void render_taskbar(void) {
    Theme* t = &themes[current_theme_idx];
    int ty = screen_h - TASKBAR_H;
    
    if (t->is_glass) graphics_fill_rect_alpha(0, ty, screen_w, TASKBAR_H, t->taskbar, 200);
    else graphics_fill_rect(0, ty, screen_w, TASKBAR_H, t->taskbar);
    
    // Start Button with Hover
    uint32_t sb = start_menu_open ? 0xFF004400 : 0xFF006600;
    if (rect_contains(2, ty+2, 60, TASKBAR_H-4, mouse.x, mouse.y)) {
        sb = 0xFF008800; // Hover green
    }
    
    graphics_fill_rect(2, ty+2, 60, TASKBAR_H-4, sb);
    graphics_draw_string_scaled(10, ty+12, "START", COL_WHITE, sb, 1);
    
    int tx = 70;
    int tab_w = taskbar_tab_width();
    for(int i=0; i<MAX_WINDOWS; i++) {
        if (windows[i] && windows[i]->visible) {
            bool active = windows[i]->focused && !windows[i]->minimized;
            uint32_t bg = active ? 0xFF505050 : 0xFF303030;
            if (!t->is_glass && active) bg = 0xFFFFFFFF;
            if (!t->is_glass && !active) bg = 0xFFC0C0C0;
            
            // Hover effect on taskbar items
            if (rect_contains(tx, ty+2, tab_w, TASKBAR_H-4, mouse.x, mouse.y)) {
                 bg = active ? 0xFF606060 : 0xFF404040;
            }
            
            graphics_fill_rect(tx, ty+2, tab_w, TASKBAR_H-4, bg);
            uint32_t tc = t->is_glass ? COL_WHITE : COL_BLACK;
            draw_string_bounded(tx+5, ty+12, tab_w-10, windows[i]->title, tc, bg, 1);
            
            if (active) graphics_fill_rect(tx, ty+TASKBAR_H-2, tab_w, 2, COL_ACCENT);
            tx += tab_w + 5;
        }
    }
    
    // Show Desktop Button (Far Right)
    int sd_x = screen_w - 20;
    graphics_fill_rect(sd_x, ty+2, 18, TASKBAR_H-4, 0xFF444444);
    
    char time[16]; desktop_get_time(time);
    graphics_draw_string_scaled(screen_w-90, ty+12, time, COL_WHITE, t->taskbar, 1);
}

static void render_desktop(void) {
    if (g_wallpaper_enabled) {
        draw_wallpaper();
    } else {
        graphics_fill_rect(0, 0, screen_w, screen_h, COL_DESKTOP);
    }
    
    struct { int x, y; const char* lbl; const uint8_t (*bmp)[24]; AppType app; } icons[] = {
        {20, 20, "Terminal", ICON_TERM, APP_TERMINAL},
        {20, 90, "Files", ICON_FOLDER, APP_FILES},
        {20, 160, "Paint", ICON_PAINT, APP_PAINT},
        {20, 230, "Browser", ICON_BROWSER, APP_BROWSER},
        {20, 300, "Calc", ICON_CALC, APP_CALC},
        {20, 370, "Task Mgr", ICON_TASKMGR, APP_TASKMGR},
        {20, 440, "Settings", ICON_SET, APP_SETTINGS},
        {100, 20, "Game", ICON_GAME, APP_TICTACTOE},
        {100, 90, "Images", ICON_IMAGE, APP_IMAGEVIEW},
        {100, 160, "About", ICON_INFO, APP_ABOUT},
        {100, 230, "AI Assist", ICON_INFO, APP_ASSISTANT},
    };
    
    for (int i=0; i<11; i++) {
        bool h = rect_contains(icons[i].x, icons[i].y, 64, 60, mouse.x, mouse.y);
        if (h) graphics_fill_rect(icons[i].x-5, icons[i].y-5, 50, 50, 0x40FFFFFF);
        draw_icon_bitmap(icons[i].x + 8, icons[i].y, icons[i].bmp);
        graphics_draw_string_scaled(icons[i].x+2, icons[i].y+36, icons[i].lbl, COL_BLACK, 0, 1);
        graphics_draw_string_scaled(icons[i].x+1, icons[i].y+35, icons[i].lbl, COL_WHITE, 0, 1);
    }

    for (int i=0; i<MAX_WINDOWS; i++) {
        if(windows[i]) render_window(windows[i]);
    }

    render_taskbar();

    uint64_t notice_now = timer_get_ticks();
    if (g_desktop_notice[0] && notice_now < g_desktop_notice_until) {
        int max_notice_w = screen_w - 20;
        if (max_notice_w >= 24) {
            int notice_w = kstrlen_local(g_desktop_notice) * 8 + 20;
            if (notice_w > max_notice_w) notice_w = max_notice_w;
            if (notice_w < 80 && max_notice_w >= 80) notice_w = 80;
            int notice_x = (screen_w - notice_w) / 2;
            int notice_y = screen_h - TASKBAR_H - 32;
            if (notice_y < 2) notice_y = 2;
            graphics_fill_rect(notice_x + 2, notice_y + 2,
                               notice_w, 24, 0xFF101010);
            graphics_fill_rect(notice_x, notice_y,
                               notice_w, 24, 0xFF303030);
            graphics_fill_rect(notice_x, notice_y,
                               notice_w, 2, COL_ACCENT);
            draw_string_bounded(notice_x + 10, notice_y + 8,
                                notice_w - 20, g_desktop_notice,
                                COL_WHITE, 0xFF303030, 1);
        }
    } else if (g_desktop_notice[0]) {
        g_desktop_notice[0] = 0;
    }
    
    // Draw Context Menu on top of everything
    render_context_menu();

    char mouse_pos[16];
    int_to_str(mouse.x, mouse_pos);
    int len = kstrlen_local(mouse_pos);
    mouse_pos[len] = ',';
    int_to_str(mouse.y, mouse_pos+len+1);
    graphics_draw_string_scaled(screen_w-150, screen_h-TASKBAR_H+12, mouse_pos, 0xFF888888, themes[current_theme_idx].taskbar, 1);

    if (start_menu_open) {
        int w = START_MENU_W;
        int h = START_MENU_H;
        int y = screen_h - TASKBAR_H - h;
        graphics_fill_rect_alpha(0, y, w, h, 0xFF1F1F1F, 240);
        graphics_fill_rect(0, y, w, 1, 0xFF404040);
        for(int i=0; i<START_MENU_ITEM_COUNT; i++) {
            int iy = y + START_MENU_FIRST_Y + i * START_MENU_ITEM_STEP;
            bool hover = rect_contains(0, iy, w, START_MENU_ITEM_H, mouse.x, mouse.y);
            if(hover) graphics_fill_rect(0, iy, w, START_MENU_ITEM_H, 0xFF404040);
            graphics_draw_string_scaled(20, iy+8, start_menu_labels[i],
                                        COL_WHITE, hover?0xFF404040:0xFF1F1F1F, 1);
        }
    }

    mouse_trail[trail_head].x = mouse.x;
    mouse_trail[trail_head].y = mouse.y;
    trail_head = (trail_head + 1) % TRAIL_LEN;
    for(int i=0; i<TRAIL_LEN; i++) {
        int idx = (trail_head + i) % TRAIL_LEN;
        if (mouse_trail[idx].x != 0)
            graphics_put_pixel(mouse_trail[idx].x, mouse_trail[idx].y, 0xFF00FFFF);
    }

    int mx = mouse.x;
    int my_pos = mouse.y;
    
    for(int y=0; y<19; y++) {
        for(int x=0; x<12; x++) {
            if(CURSOR_BITMAP[y][x] == 1) graphics_put_pixel(mx+x, my_pos+y, COL_BLACK);
            else if(CURSOR_BITMAP[y][x] == 2) graphics_put_pixel(mx+x, my_pos+y, COL_WHITE);
        }
    }
}

static void on_right_click(int x, int y) {
    if (start_menu_open) start_menu_open = false;
    hide_context_menu();
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        Window* w = windows[i];
        if (w && w->visible && !w->minimized &&
            rect_contains(w->x, w->y, w->w, w->h, x, y)) {
            int focused_index = focus_window(i);
            w = focused_index >= 0 ? windows[focused_index] : NULL;
            if (w && w->type == APP_MINESWEEPER &&
                y >= w->y + WIN_CAPTION_H) {
                handle_minesweeper(w, x, y, true);
            }
            return;
        }
    }
    if (y >= screen_h - TASKBAR_H) return;
    show_context_menu(x, y);
}

static void on_click(int x, int y) {
    if (g_ctx_menu.active) {
        handle_context_menu_click(x, y);
        return;
    }

    int ty = screen_h - TASKBAR_H;
    
    if (start_menu_open) {
        int menu_y = ty - START_MENU_H;
        if (x >= 0 && x < START_MENU_W && y >= menu_y && y < ty) {
            for (int item = 0; item < START_MENU_ITEM_COUNT; item++) {
                int item_y = menu_y + START_MENU_FIRST_Y + item * START_MENU_ITEM_STEP;
                if (rect_contains(0, item_y, START_MENU_W, START_MENU_ITEM_H, x, y)) {
                    if (start_menu_apps[item] == APP_NONE) g_gui_running = false;
                    else launch_app(start_menu_apps[item]);
                    break;
                }
            }
            start_menu_open = false;
            return;
        }
        start_menu_open = false;
    }

    if (y >= ty) {
        // Show Desktop Button
        if (x > screen_w - 20) {
            g_desktop_shown_mode = !g_desktop_shown_mode;
            for(int i=0; i<MAX_WINDOWS; i++) if(windows[i]) {
                windows[i]->minimized = g_desktop_shown_mode;
                windows[i]->focused = false;
            }
            if (!g_desktop_shown_mode) focus_top_visible();
            return;
        }

        if (x < 70) { start_menu_open = !start_menu_open; return; }
        int tx = 70;
        int tab_w = taskbar_tab_width();
        for(int i=0; i<MAX_WINDOWS; i++) {
            if(windows[i] && windows[i]->visible) {
                if(x >= tx && x < tx+tab_w) {
                    if (windows[i]->focused && !windows[i]->minimized) minimize_window(windows[i]);
                    else { windows[i]->minimized = false; focus_window(i); }
                    return;
                }
                tx += tab_w + 5;
            }
        }
        return;
    }

    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        Window* w = windows[i];
        if (w && w->visible && !w->minimized) {
            if (rect_contains(w->x, w->y, w->w, w->h, x, y)) {
                int idx = focus_window(i); w = windows[idx];
                
                if (!w->maximized &&
                    rect_contains(w->x + w->w - RESIZE_HANDLE,
                                  w->y + w->h - RESIZE_HANDLE,
                                  RESIZE_HANDLE, RESIZE_HANDLE, x, y)) {
                    w->resizing = true;
                    w->drag_off_x = x - w->w;
                    w->drag_off_y = y - w->h;
                    return;
                }

                if (y < w->y + WIN_CAPTION_H) {
                    int bx = w->x + w->w - 24;
                    if (rect_contains(bx, w->y+4, 18, 18, x, y)) { close_window(idx); return; }
                    int mx = bx - 22;
                    if (rect_contains(mx, w->y+4, 18, 18, x, y)) { toggle_maximize(w); return; }
                    int mn = mx - 22;
                    if (rect_contains(mn, w->y+4, 18, 18, x, y)) { minimize_window(w); return; }
                    if (w->maximized) return;
                    w->dragging = true; w->drag_off_x = x - w->x; w->drag_off_y = y - w->y;
                    return;
                }
                if (w->type == APP_PAINT) handle_paint_click(w, x, y);
                if (w->type == APP_NOTEPAD) handle_notepad_click(w, x, y);
                if (w->type == APP_SETTINGS) handle_settings_click(w, x, y);
                if (w->type == APP_FILES) handle_files_click(w, x, y);
                if (w->type == APP_BROWSER) handle_browser_click(w, x, y);
                if (w->type == APP_TASKMGR) handle_taskmgr_click(w, x, y);
                if (w->type == APP_CALC) handle_calc_logic(w, x, y);
                if (w->type == APP_MINESWEEPER) handle_minesweeper(w, x, y, false);
                if (w->type == APP_TICTACTOE) handle_tictactoe(w, x, y);
                if (w->type == APP_IMAGEVIEW) handle_imageview(w, x, y);
                if (w->type == APP_ASSISTANT) handle_assistant_click(w, x, y);
                if (w->type == APP_WELCOME) {
                    int wcx = w->x + 2;
                    int wcy = w->y + WIN_CAPTION_H + 2;
                    if (rect_contains(wcx + 20, wcy + 118, w->w - 44, 28, x, y))
                        launch_app(APP_ASSISTANT);
                }
                if (w->type == APP_RUN) {
                    int rcx = w->x + 2;
                    int rcy = w->y + WIN_CAPTION_H + 2;
                    int rcw = w->w - 4;
                    if (rect_contains(rcx + rcw - 66, rcy + 32, 54, 26, x, y))
                        handle_run_command(w);
                }
                return;
            }
        }
    }
    
    struct { int x, y; const char* n; AppType t; } icons[] = {
        {20, 20, "Terminal", APP_TERMINAL}, {20, 90, "Files", APP_FILES},
        {20, 160, "Paint", APP_PAINT}, {20, 230, "Local Browser", APP_BROWSER},
        {20, 300, "Calc", APP_CALC}, {20, 370, "Task Mgr", APP_TASKMGR},
        {20, 440, "Settings", APP_SETTINGS}, {100, 20, "Tic-Tac-Toe", APP_TICTACTOE},
        {100, 90, "ImageView", APP_IMAGEVIEW}, {100, 160, "About", APP_ABOUT},
        {100, 230, "AI Assistant", APP_ASSISTANT}
    };
    for(int i=0; i<11; i++) {
        if (rect_contains(icons[i].x, icons[i].y, 60, 60, x, y)) {
            launch_app(icons[i].t); return;
        }
    }
}

void gui_demo_run(void) {
    desktop_log("GUI: Starting Glass Desktop...");
    g_gui_running = true;
    graphics_enable_double_buffer();
    screen_w = graphics_get_width();
    screen_h = graphics_get_height();
    for(int i=0; i<MAX_WINDOWS; i++) windows[i] = NULL;
    desktop_get_mouse(&mouse);
    prev_mouse = mouse;
    mem_zero(mouse_trail, sizeof(mouse_trail));
    trail_head = 0;
    start_menu_open = false;
    g_desktop_shown_mode = false;
    g_ctx_menu.active = false;
    g_desktop_notice[0] = '\0';
    g_desktop_notice_until = 0;
    settings_load();
    create_window(APP_WELCOME, "Welcome", 350, 200);

    bool desktop_exit_ready = false;
    while (!desktop_exit_ready) {
      while(g_gui_running) {
        desktop_yield();
        char c = keyboard_poll_char();
        Window* top = get_top_window();
        if (c == 27) {
            if (top && top->type == APP_FILES &&
                top->state.files.renaming) {
                top->state.files.renaming = false;
                str_copy(top->state.files.status,
                         sizeof(top->state.files.status),
                         "Rename cancelled.");
                c = 0;
            } else {
                break;
            }
        }
        if (c && top && top->focused) {
            if (top->type == APP_FILES && top->state.files.renaming) {
                FileManagerState* state = &top->state.files;
                if (c == '\n') {
                    files_commit_rename(top);
                } else if (c == '\b') {
                    if (state->rename_select_all) {
                        state->rename_buffer[0] = 0;
                        state->rename_length = 0;
                        state->rename_select_all = false;
                    } else if (state->rename_length > 0) {
                        state->rename_buffer[--state->rename_length] = 0;
                    }
                } else if (c >= 33 && c <= 126 &&
                           c != '/' && c != '\\' &&
                           (state->rename_select_all ||
                            state->rename_length <
                                (int)sizeof(state->rename_buffer) - 1)) {
                    if (state->rename_select_all) {
                        state->rename_buffer[0] = 0;
                        state->rename_length = 0;
                        state->rename_select_all = false;
                    }
                    if (state->rename_length <
                        (int)sizeof(state->rename_buffer) - 1) {
                        state->rename_buffer[state->rename_length++] = c;
                        state->rename_buffer[state->rename_length] = 0;
                    }
                }
            }
            else if(top->type == APP_TERMINAL) handle_terminal_input(top, c);
            else if(top->type == APP_BROWSER) handle_browser_input(top, c);
            else if(top->type == APP_ASSISTANT) handle_assistant_input(top, c);
            else if(top->type == APP_RUN) {
                RunState* r = &top->state.run;
                if (c == '\n') handle_run_command(top);
                else if (c == '\b') { if(r->len>0) r->cmd[--r->len]=0; }
                else if (c >= 32 && c <= 126 &&
                         r->len < (int)sizeof(r->cmd) - 1) {
                    r->cmd[r->len++]=c; r->cmd[r->len]=0;
                }
            }
            else if (top->type == APP_NOTEPAD) {
                NotepadState* ns = &top->state.notepad;
                bool edited = false;
                if (c == '\b') {
                    if (ns->length > 0) {
                        ns->buffer[--ns->length] = 0;
                        ns->dirty = true;
                        ns->discard_deadline = 0;
                        cancel_exit_discard_confirmation();
                        edited = true;
                    }
                } else if ((c == '\n' || (c >= 32 && c <= 126)) &&
                           ns->length < (int)sizeof(ns->buffer) - 1) {
                    ns->buffer[ns->length++] = c;
                    ns->buffer[ns->length] = 0;
                    ns->dirty = true;
                    ns->discard_deadline = 0;
                    cancel_exit_discard_confirmation();
                    edited = true;
                } else if (c == '\n' || (c >= 32 && c <= 126)) {
                    str_copy(ns->status, sizeof(ns->status),
                             "File full: maximum 1,023 bytes.");
                }
                if (edited)
                    str_copy(ns->status, sizeof(ns->status),
                             "Unsaved changes.");
            }
        }

        top = get_top_window();
        prev_mouse = mouse; 
        desktop_get_mouse(&mouse);
        
        if (mouse.left_button && top && !g_ctx_menu.active) {
            if (top->dragging) {
                top->x = mouse.x - top->drag_off_x;
                top->y = mouse.y - top->drag_off_y;
                if (top->x < 0) top->x = 0;
                if (top->y < 0) top->y = 0;
                if (top->x + top->w > screen_w) top->x = screen_w - top->w;
                if (top->y + top->h > screen_h - TASKBAR_H)
                    top->y = screen_h - TASKBAR_H - top->h;
            } else if (top->resizing) {
                int nw = mouse.x - top->drag_off_x;
                int nh = mouse.y - top->drag_off_y;
                if (nw < top->min_w) nw = top->min_w;
                if (nh < top->min_h) nh = top->min_h;
                int max_w = screen_w - top->x;
                int max_h = screen_h - TASKBAR_H - top->y;
                if (nw > max_w) nw = max_w;
                if (nh > max_h) nh = max_h;
                top->w = nw;
                top->h = nh;
            } else if (top->type == APP_PAINT) {
                int canvas_x, canvas_y, canvas_w, canvas_h;
                paint_canvas_rect(top, &canvas_x, &canvas_y,
                                  &canvas_w, &canvas_h);
                if (rect_contains(canvas_x, canvas_y, canvas_w, canvas_h,
                                  mouse.x, mouse.y))
                    handle_paint_click(top, mouse.x, mouse.y);
            }
        }
        
        // Handle Left Click
        if (mouse.left_button && !prev_mouse.left_button) {
             on_click(mouse.x, mouse.y);
        }

        // Handle Right Click (Context Menu)
        if (mouse.right_button && !prev_mouse.right_button) {
            on_right_click(mouse.x, mouse.y);
        }

        if (!mouse.left_button) for(int i=0; i<MAX_WINDOWS; i++) if(windows[i]) {
            windows[i]->dragging = false;
            windows[i]->resizing = false;
        }

        // Refresh live System Monitor samples while its window is open.
        for(int i=0; i<MAX_WINDOWS; i++) {
            if(windows[i] && windows[i]->type == APP_SYSMON) {
                 update_sysmon(windows[i]);
            }
        }

        render_desktop();
        graphics_swap_buffer();
      }
      if (desktop_save_all()) {
          desktop_exit_ready = true;
      } else {
          g_gui_running = true;
      }
    }
    
    while (windows[0]) {
        if (!close_window(0)) {
            desktop_log("GUI: unexpected clean-window close failure");
            break;
        }
    }
    graphics_disable_double_buffer();
    g_gui_running = false;
}
