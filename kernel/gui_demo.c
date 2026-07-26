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
    uint64_t tick = timer_get_ticks();
    while (timer_get_ticks() == tick) {
        timer_idle_wait();
    }
}

static void desktop_shutdown(void) {
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
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
#define COL_DESKTOP     0xFF004488 
#define COL_TASKBAR     0xFF101010
#define COL_WIN_BODY    0xFFF0F0F0
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
typedef struct { int current_val; int accumulator; char op; bool new_entry; } CalcState;
typedef struct { char buffer[512]; int length; } NotepadState;
typedef struct { int selected_index; int scroll_offset; } FileManagerState;
typedef struct { bool wallpaper_enabled; int theme_id; } SettingsState;
typedef struct { char prompt[16]; char input[64]; int input_len; char history[6][64]; } TerminalState;
typedef struct { char url[64]; int url_len; char status[32]; int scroll; } BrowserState;
typedef struct {
    uint64_t selected_pid;
    size_t page_offset;
} TaskMgrState;
typedef struct { uint32_t* canvas_buffer; int width; int height; uint32_t current_color; int brush_size; } PaintState;
typedef struct { char cmd[32]; int len; } RunState;

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
    uint32_t memory_used_kb;
    uint32_t memory_total_kb;
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
static const AppType start_menu_apps[START_MENU_ITEM_COUNT] = {
    APP_ASSISTANT, APP_BROWSER, APP_TERMINAL, APP_PAINT,
    APP_FILES, APP_TASKMGR, APP_NOTEPAD, APP_CALC,
    APP_MINESWEEPER, APP_TICTACTOE, APP_IMAGEVIEW,
    APP_SYSMON, APP_RUN, APP_NONE
};
static const char* start_menu_labels[START_MENU_ITEM_COUNT] = {
    "AI Assistant", "Browser", "Terminal", "Paint",
    "Files", "Task Manager", "Notepad", "Calculator",
    "Minesweeper", "Tic-Tac-Toe", "Image Viewer",
    "Sys Monitor", "Run...", "Exit Desktop"
};

// --- Forward Declarations ---
static void close_window(int index);
static int focus_window(int index);
static void toggle_maximize(Window* w);
static void handle_paint_click(Window* w, int x, int y);
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
static void render_window(Window* w);
static void render_assistant_app(Window* w);
static void draw_wallpaper(void);
static void on_click(int x, int y);
static void on_right_click(int x, int y);
static void update_sysmon(Window* w);
static void create_window(AppType type, const char* title, int w, int h);
static void launch_app(AppType type);
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
static void mem_zero(void* ptr, size_t size) {
    uint8_t* bytes = (uint8_t*)ptr;
    for (size_t i = 0; i < size; i++) bytes[i] = 0;
}
static void str_append(char* d, int capacity, const char* s) {
    int i = kstrlen_local(d);
    int j = 0;
    while (s[j] && i + 1 < capacity) d[i++] = s[j++];
    d[i] = 0;
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

static void close_window(int index) {
    if (index < 0 || index >= MAX_WINDOWS || windows[index] == NULL) return;
    Window* w = windows[index];
    bool was_focused = w->focused;
    
    if (w->type == APP_PAINT && w->state.paint.canvas_buffer) {
        desktop_free(w->state.paint.canvas_buffer);
    }
    
    desktop_free(w);
    windows[index] = NULL;
    
    for (int i = index; i < MAX_WINDOWS - 1; i++) {
        windows[i] = windows[i+1];
        if (windows[i]) windows[i]->id = i; 
    }
    windows[MAX_WINDOWS - 1] = NULL;
    if (was_focused) focus_top_visible();
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

static void create_window(AppType type, const char* title, int w, int h) {
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) { if (windows[i] == NULL) { slot = i; break; } }
    if (slot == -1) {
        close_window(0);
        slot = MAX_WINDOWS - 1;
    }

    Window* win = (Window*)desktop_malloc(sizeof(Window));
    if (!win) return;
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
        win->min_w = 260; win->min_h = 220;
        int cw = w-12; int ch = h-WIN_CAPTION_H-12;
        win->state.paint.width = cw; win->state.paint.height = ch;
        win->state.paint.canvas_buffer = desktop_malloc((size_t)cw * (size_t)ch * sizeof(uint32_t));
        win->state.paint.current_color = 0xFF000000; win->state.paint.brush_size = 2;
        if(win->state.paint.canvas_buffer) {
            for(int i=0; i<cw*ch; i++) win->state.paint.canvas_buffer[i] = 0xFFFFFFFF;
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
        str_copy(win->state.term.prompt, sizeof(win->state.term.prompt), "$ ");
        win->state.term.input[0]=0; win->state.term.input_len=0;
        for(int k=0; k<6; k++) win->state.term.history[k][0] = 0;
    } else if (type == APP_SETTINGS) {
        win->min_w = 240; win->min_h = 210;
        win->state.settings.wallpaper_enabled = g_wallpaper_enabled;
        win->state.settings.theme_id = current_theme_idx;
    } else if (type == APP_BROWSER) {
        win->min_w = 360; win->min_h = 180;
        str_copy(win->state.browser.url, sizeof(win->state.browser.url), "www.retro-os.net");
        win->state.browser.url_len = kstrlen_local("www.retro-os.net");
    } else if (type == APP_TASKMGR) {
        win->min_w = 380; win->min_h = 240;
        win->state.taskmgr.selected_pid = 0;
        win->state.taskmgr.page_offset = 0;
    } else if (type == APP_FILES) {
        win->min_w = 280; win->min_h = 180;
        win->state.files.selected_index = -1;
        win->state.files.scroll_offset = 0;
    } else if (type == APP_NOTEPAD) {
        win->min_w = 260; win->min_h = 180;
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
}

static void launch_app(AppType type) {
    switch (type) {
        case APP_ASSISTANT: create_window(APP_ASSISTANT, "AI Assistant", 520, 380); break;
        case APP_BROWSER: create_window(APP_BROWSER, "Browser", 500, 400); break;
        case APP_TERMINAL: create_window(APP_TERMINAL, "Terminal", 400, 300); break;
        case APP_PAINT: create_window(APP_PAINT, "Paint", 500, 400); break;
        case APP_FILES: create_window(APP_FILES, "Files", 420, 320); break;
        case APP_TASKMGR: create_window(APP_TASKMGR, "Task Manager", 440, 320); break;
        case APP_NOTEPAD: create_window(APP_NOTEPAD, "Notepad", 400, 300); break;
        case APP_CALC: create_window(APP_CALC, "Calculator", 220, 300); break;
        case APP_MINESWEEPER: create_window(APP_MINESWEEPER, "Minesweeper", 220, 260); break;
        case APP_TICTACTOE: create_window(APP_TICTACTOE, "Tic-Tac-Toe", 220, 280); break;
        case APP_IMAGEVIEW: create_window(APP_IMAGEVIEW, "Image Viewer", 420, 340); break;
        case APP_SYSMON: create_window(APP_SYSMON, "System Monitor", 420, 300); break;
        case APP_SETTINGS: create_window(APP_SETTINGS, "Settings", 360, 240); break;
        case APP_RUN: create_window(APP_RUN, "Run", 360, 140); break;
        case APP_ABOUT: create_window(APP_ABOUT, "About Nostalux", 340, 260); break;
        case APP_WELCOME: create_window(APP_WELCOME, "Welcome", 420, 200); break;
        default: break;
    }
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

static bool filename_is_bmp(const char* name) {
    int length = kstrlen_local(name);
    return length >= 4 &&
           name[length - 4] == '.' &&
           ascii_lower(name[length - 3]) == 'b' &&
           ascii_lower(name[length - 2]) == 'm' &&
           ascii_lower(name[length - 1]) == 'p';
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

        state->file_index = index;
        state->fit_to_window = true;
        str_copy(state->filename, sizeof(state->filename), file->name);
        if (bmp_open(file->data, file->size, &state->image)) {
            state->valid = true;
            str_copy(state->status, sizeof(state->status), "Loaded from the filesystem.");
        } else {
            str_copy(state->status, sizeof(state->status), "Invalid or unsupported 24-bit BMP.");
        }
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
        case APP_BROWSER: return "Browser";
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
            char reply[64] = "Opening ";
            str_append(reply, sizeof(reply), assistant_app_name(requested));
            str_append(reply, sizeof(reply), ".");
            assistant_add_wrapped(state, ASSISTANT_ROLE_AI, reply);
            launch_app(requested);
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
            "Apps: Terminal, Files, Paint, Browser, Notepad, Calculator, Settings, monitors, games, and Image Viewer.");
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
        int_to_str((int)(profile->memory_total_kb / 1024), number);
        str_append(details, sizeof(details), number);
        str_append(details, sizeof(details), " MB");
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI, details);
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "The OS is a freestanding hobby kernel with its own shell, GUI, scheduler, and ATA-backed files.");
    } else if (text_contains_ci(prompt, "command") || text_contains_ci(prompt, "shell") ||
               text_contains_ci(prompt, "console")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "At the boot shell, type help. Useful commands include gui, ls, cat, write, calc, time, sysinfo, snake, and shutdown.");
    } else if (text_contains_ci(prompt, "file") || text_contains_ci(prompt, "save")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "Use Files to browse. In the boot shell, ls, cat, touch, write, append, and rm manage the persistent flat filesystem.");
    } else if (text_contains_ci(prompt, "internet") || text_contains_ci(prompt, "online") ||
               text_contains_ci(prompt, "network")) {
        assistant_add_wrapped(state, ASSISTANT_ROLE_AI,
            "NostaluxOS has no network stack yet. I run fully offline, and Browser is currently a visual demo.");
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
    const char* cmd = w->state.run.cmd;
    if (kstrcmp(cmd, "calc") == 0) launch_app(APP_CALC);
    else if (kstrcmp(cmd, "term") == 0) launch_app(APP_TERMINAL);
    else if (kstrcmp(cmd, "paint") == 0) launch_app(APP_PAINT);
    else if (kstrcmp(cmd, "sys") == 0) launch_app(APP_SYSMON);
    else if (kstrcmp(cmd, "mine") == 0) launch_app(APP_MINESWEEPER);
    else if (kstrcmp(cmd, "browser") == 0) launch_app(APP_BROWSER);
    else if (kstrcmp(cmd, "ttt") == 0) launch_app(APP_TICTACTOE);
    else if (kstrcmp(cmd, "img") == 0) launch_app(APP_IMAGEVIEW);
    else if (kstrcmp(cmd, "about") == 0) launch_app(APP_ABOUT);
    else if (kstrcmp(cmd, "ai") == 0 || kstrcmp(cmd, "assistant") == 0) launch_app(APP_ASSISTANT);
    else if (kstrcmp(cmd, "shutdown") == 0) desktop_shutdown();
    close_window(w->id);
}

static void handle_paint_click(Window* w, int x, int y) {
    if (!w->state.paint.canvas_buffer) return;
    
    int cx = w->x + 6;
    int cy = w->y + WIN_CAPTION_H + 46;
    
    if (y < cy) {
        int palette_y = w->y + WIN_CAPTION_H + 11;
        if (y >= palette_y && y < palette_y + 25) {
            int local_x = x - (cx + 5);
            if (local_x >= 0) {
                int idx = local_x / 30;
                if (idx >= 0 && idx < 8) {
                    uint32_t colors[] = {0xFF000000, 0xFFFFFFFF, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF};
                    w->state.paint.current_color = colors[idx];
                }
            }
        }
        return;
    }
    
    int rel_x = x - cx;
    int rel_y = y - cy;
    int cw = w->state.paint.width;
    int ch = w->state.paint.height;
    
    if (rel_x >= 0 && rel_x < cw && rel_y >= 0 && rel_y < ch) {
        int sz = w->state.paint.brush_size;
        uint32_t col = w->state.paint.current_color;
        uint32_t* buf = w->state.paint.canvas_buffer;
        
        for(int dy=-sz; dy<=sz; dy++) {
            for(int dx=-sz; dx<=sz; dx++) {
                int px = rel_x + dx;
                int py = rel_y + dy;
                if (px >= 0 && px < cw && py >= 0 && py < ch) {
                    buf[py * cw + px] = col;
                }
            }
        }
    }
}

static void handle_settings_click(Window* w, int x, int y) {
    int cx = w->x + 2;
    int cy = w->y + WIN_CAPTION_H + 2;
    if (rect_contains(cx + 10, cy + 30, 140, 30, x, y)) {
        g_wallpaper_enabled = !g_wallpaper_enabled;
        w->state.settings.wallpaper_enabled = g_wallpaper_enabled;
    } else if (rect_contains(cx + 10, cy + 100, 140, 30, x, y)) {
        current_theme_idx = (current_theme_idx + 1) % 2;
        w->state.settings.theme_id = current_theme_idx;
    }
}

static void handle_files_click(Window* w, int x, int y) {
    int cx = w->x + 4;
    int cy = w->y + WIN_CAPTION_H + 4;
    size_t count = fs_file_count();
    for (size_t i = 0; i < count; i++) {
        int ry = cy + 24 + i * 18;
        if (ry + 18 < w->y + w->h && rect_contains(cx + 2, ry, w->w - 12, 18, x, y)) {
            w->state.files.selected_index = (int)i;
            return;
        }
    }
}

#define TASKMGR_PAGE_CAPACITY 16

static int taskmgr_visible_rows(const Window* w) {
    int content_height = w->h - WIN_CAPTION_H - 4;
    int rows = (content_height - 80) / 20;
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
        return;
    }
    if (rect_contains(cx + 60, button_y, 44, 20, x, y)) {
        size_t next = w->state.taskmgr.page_offset + (size_t)rows;
        if (next < total) w->state.taskmgr.page_offset = next;
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
    int cx = w->x;
    int cy = w->y + WIN_CAPTION_H;
    if (rect_contains(cx + w->w - 40, cy + 5, 30, 20, x, y)) {
        str_copy(w->state.browser.status, sizeof(w->state.browser.status), "Loading...");
    }
}

static void calc_button_rect(Window* w, int button, int* x, int* y) {
    int cx = w->x + 2;
    int cy = w->y + WIN_CAPTION_H + 2;
    *x = cx + 10 + (button % 4) * 40;
    *y = cy + 45 + (button / 4) * 30;
}

static int clamp_calc_result(long long value) {
    if (value > INT_MAX) return INT_MAX;
    if (value < INT_MIN) return INT_MIN;
    return (int)value;
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
                int d = c - '0';
                if (s->new_entry) { s->current_val = d; s->new_entry = false; }
                else if (s->current_val >= 0 && s->current_val <= (INT_MAX - d) / 10)
                    s->current_val = s->current_val * 10 + d;
            } 
            else if (c == 'C') { s->current_val = 0; s->accumulator = 0; s->op = 0; s->new_entry = true; } 
            else if (c == '+' || c == '-' || c == '*' || c == '/') { s->accumulator = s->current_val; s->op = c; s->new_entry = true; } 
            else if (c == '=') {
                long long left = s->accumulator;
                long long right = s->current_val;
                if (s->op == '+') s->current_val = clamp_calc_result(left + right);
                else if (s->op == '-') s->current_val = clamp_calc_result(left - right);
                else if (s->op == '*') s->current_val = clamp_calc_result(left * right);
                else if (s->op == '/' && right != 0)
                    s->current_val = clamp_calc_result(left / right);
                s->op = 0; s->new_entry = true;
            }
            return;
        }
    }
}

static void handle_terminal_input(Window* w, char c) {
    TerminalState* ts = &w->state.term;
    if (c == '\n') {
        for (int i=0; i<5; i++) str_copy(ts->history[i], sizeof(ts->history[i]), ts->history[i+1]);
        char line[80]; 
        str_copy(line, sizeof(line), ts->prompt);
        int p_len = kstrlen_local(line); 
        int i = 0; 
        while(ts->input[i] && p_len < 79) line[p_len++] = ts->input[i++];
        line[p_len] = 0; 
        str_copy(ts->history[5], sizeof(ts->history[5]), line);
        
        if (kstrcmp(ts->input, "exit") == 0) {
            close_window(w->id);
            return;
        }
        else if (kstrcmp(ts->input, "cls") == 0) for(int k=0; k<6; k++) ts->history[k][0] = 0;
        else if (kstrcmp(ts->input, "ai") == 0 || kstrcmp(ts->input, "assistant") == 0)
            launch_app(APP_ASSISTANT);
        
        ts->input[0] = 0; 
        ts->input_len = 0;
    } 
    else if (c == '\b') { if (ts->input_len > 0) ts->input[--ts->input_len] = 0; } 
    else if (c >= 32 && c <= 126 && ts->input_len < 60) { ts->input[ts->input_len++] = c; ts->input[ts->input_len] = 0; }
}

static void handle_browser_input(Window* w, char c) {
    BrowserState* bs = &w->state.browser;
    if (c == '\b') {
        if (bs->url_len > 0) bs->url[--bs->url_len] = 0;
    } else if (c == '\n') {
        str_copy(bs->status, sizeof(bs->status), "Loaded.");
    } else if (c >= 32 && c <= 126 && bs->url_len < 60) {
        bs->url[bs->url_len++] = c;
        bs->url[bs->url_len] = 0;
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
    uint32_t total_kb = profile->memory_total_kb;
    uint32_t used_kb = profile->memory_used_kb;
    int memory_percent = 0;
    if (total_kb > 0) {
        uint64_t scaled = (uint64_t)used_kb * 100u;
        memory_percent = (int)(scaled / total_kb);
        if (memory_percent > 100) memory_percent = 100;
    }

    s->cpu_percent = cpu_percent;
    s->memory_used_kb = used_kb;
    s->memory_total_kb = total_kb;
    s->cpu_hist[s->head] = cpu_percent;
    s->mem_hist[s->head] = memory_percent;
    s->previous_cpu = current;
    s->last_sample_tick = now_tick;
    s->has_previous_cpu = true;
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
                    // Simple refresh effect handled by redraw
                    break;
                case CTX_WALLPAPER:
                    g_wallpaper_enabled = true;
                    g_wallpaper_seed = fast_rand() % 1000;
                    break;
                case CTX_NEW_FILE:
                    create_window(APP_NOTEPAD, "Untitled.txt", 300, 200);
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
    int ch = w->h - WIN_CAPTION_H - 12;
    
    int tool_h = 40;
    graphics_fill_rect(cx, cy, cw, tool_h, 0xFFE0E0E0);
    
    // Fixed: Added last color to match loop count of 8
    uint32_t colors[] = {0xFF000000, 0xFFFFFFFF, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF};
    for(int i=0; i<8; i++) {
        int px = cx + 5 + (i*30);
        int py = cy + 5;
        graphics_fill_rect(px, py, 25, 25, colors[i]);
        if (w->state.paint.current_color == colors[i]) {
            graphics_fill_rect(px, py+26, 25, 3, 0xFF000000);
        }
    }
    
    int cv_y = cy + tool_h;
    int cv_h = ch - tool_h;

    if (cv_h <= 0 || cw <= 0) return;
    graphics_fill_rect(cx, cv_y, cw, cv_h, COL_WHITE);
    if (w->state.paint.canvas_buffer) {
        uint32_t* buf = w->state.paint.canvas_buffer;
        int buf_w = w->state.paint.width;
        int buf_h = w->state.paint.height;
        int start_x = cx;
        int start_y = cv_y;

        int rows = cv_h < buf_h ? cv_h : buf_h;
        int cols = cw < buf_w ? cw : buf_w;
        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < cols; x++) {
                uint32_t col = buf[y * buf_w + x];
                graphics_put_pixel(start_x + x, start_y + y, col);
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
        draw_bevel_box(cx+2, cy+2, cw-4, ch-4, true);
        graphics_fill_rect(cx+4, cy+4, cw-8, ch-8, COL_WHITE);
        int max_cols = (cw - 12) / 8;
        int max_rows = (ch - 12) / 10;
        int col = 0;
        int row = 0;
        for (int i = 0; i < w->state.notepad.length && row < max_rows; i++) {
            char c = w->state.notepad.buffer[i];
            if (c == '\n' || col >= max_cols) {
                row++;
                col = 0;
                if (c == '\n' || row >= max_rows) continue;
            }
            graphics_draw_char(cx + 6 + col * 8, cy + 6 + row * 10,
                               c, COL_BLACK, COL_WHITE);
            col++;
        }
        if ((timer_get_ticks() / 15) % 2) {
            if (col >= max_cols) {
                row++;
                col = 0;
            }
            if (row < max_rows)
                graphics_fill_rect(cx + 6 + col * 8, cy + 6 + row * 10, 2, 10, COL_BLACK);
        }
    } 
    else if (w->type == APP_PAINT) {
        render_paint_app(w);
    }
    else if (w->type == APP_ABOUT) {
        graphics_draw_string_scaled(cx+20, cy+20, "NostaluxOS", COL_ACCENT, COL_WIN_BODY, 3);
        graphics_draw_string_scaled(cx+20, cy+50, "Version 1.1", COL_BLACK, COL_WIN_BODY, 1);
        graphics_draw_string_scaled(cx+20, cy+70, "(C) 2025 Retro Systems", COL_BLACK, COL_WIN_BODY, 1);
        
        draw_bevel_box(cx+10, cy+100, cw-20, 100, true);
        graphics_fill_rect(cx+12, cy+102, cw-24, 96, COL_WHITE);
        
        const char* credits[] = {
            "Kernel: x86_64",
            "GUI: Glass Window Manager",
            "Features: Apps, Themes,", 
            "Context Menu, Versions",
            "Status: Active Development"
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
        draw_bevel_box(cx+2, cy+2, cw-40, 24, true);
        graphics_fill_rect(cx+4, cy+4, cw-44, 20, COL_WHITE);
        draw_string_bounded(cx+6, cy+8, cw-52, w->state.browser.url,
                            COL_BLACK, COL_WHITE, 1);
        draw_bevel_box(cx+cw-35, cy+2, 30, 24, false);
        graphics_draw_string_scaled(cx+cw-28, cy+8, "GO", COL_BLACK, COL_BTN_FACE, 1);
        
        int content_y = cy + 30;
        int content_h = ch - 32;
        graphics_fill_rect(cx+2, content_y, cw-4, content_h, COL_WHITE);
        graphics_draw_string_scaled(cx+10, content_y+10, "Nostalux Web Browser v1.0", 0xFF0000AA, COL_WHITE, 2);
        graphics_draw_string_scaled(cx+10, content_y+40, "Status:", 0xFF555555, COL_WHITE, 1);
        draw_string_bounded(cx+70, content_y+40, cw-82, w->state.browser.status,
                            0xFF00AA00, COL_WHITE, 1);
        draw_string_bounded(cx+10, content_y+70, cw-20,
                            "Welcome to the future of browsing!",
                            COL_BLACK, COL_WHITE, 1);
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

        draw_string_bounded(cx + 10, cy + ch - 38, cw - 20,
                            "Desktop apps share the kernel/desktop task.",
                            0xFF555555, COL_WIN_BODY, 1);

        int button_y = cy + ch - 24;
        draw_bevel_box(cx + 10, button_y, 44, 20, false);
        graphics_draw_string_scaled(cx + 16, button_y + 6, "Prev",
                                    COL_BLACK, COL_BTN_FACE, 1);
        draw_bevel_box(cx + 60, button_y, 44, 20, false);
        graphics_draw_string_scaled(cx + 66, button_y + 6, "Next",
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
        draw_string_bounded(cx + 114, button_y + 6, cw - 124, task_footer,
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
        int graph_h = 70;
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

        char memory_label[64] = "Tracked RAM: ";
        uint64_to_str((uint64_t)state->memory_used_kb, number);
        str_append(memory_label, sizeof(memory_label), number);
        str_append(memory_label, sizeof(memory_label), " / ");
        uint64_to_str((uint64_t)state->memory_total_kb, number);
        str_append(memory_label, sizeof(memory_label), number);
        str_append(memory_label, sizeof(memory_label), " KB  ");
        int_to_str(state->mem_hist[state->head], number);
        str_append(memory_label, sizeof(memory_label), number);
        str_append(memory_label, sizeof(memory_label), "%");
        draw_string_bounded(cx + 10, cy + 104, cw - 20, memory_label,
                            COL_BLACK, COL_WIN_BODY, 1);

        int memory_graph_y = cy + 120;
        graphics_fill_rect(graph_x, memory_graph_y, graph_w, graph_h, COL_BLACK);
        for (int i = 0; i < SYSMON_HIST; i++) {
            int idx = (state->head - i + SYSMON_HIST) % SYSMON_HIST;
            int val = state->mem_hist[idx];
            if (val > 100) val = 100;
            int bar_h = (val * graph_h) / 100;
            if (val > 0 && bar_h == 0) bar_h = 1;
            int bx = graph_x + graph_w - graph_step * (i + 1);
            if (bx >= graph_x) {
                graphics_fill_rect(bx, memory_graph_y + graph_h - bar_h,
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
        const char* lbl = on ? "Enabled (Random)" : "Disabled (Blue)";
        graphics_draw_string_scaled(cx+20, cy+40, lbl, COL_BLACK, COL_BTN_FACE, 1);
        graphics_draw_string_scaled(cx+10, cy+80, "System Theme:", COL_BLACK, COL_WIN_BODY, 1);
        draw_bevel_box(cx+10, cy+100, 140, 30, false);
        graphics_draw_string_scaled(cx+20, cy+110,
                                    current_theme_idx == 0 ? "Ocean Glass" : "Retro Grey",
                                    COL_BLACK, COL_BTN_FACE, 1);
    }
    else if (w->type == APP_RUN) {
        graphics_draw_string_scaled(cx+12, cy+12, "Type an app command:", COL_BLACK, COL_WIN_BODY, 1);
        draw_bevel_box(cx+12, cy+32, cw-90, 26, true);
        graphics_fill_rect(cx+14, cy+34, cw-94, 22, COL_WHITE);
        draw_string_bounded(cx+18, cy+40, cw-104, w->state.run.cmd,
                            COL_BLACK, COL_WHITE, 1);
        draw_bevel_box(cx+cw-66, cy+32, 54, 26, false);
        graphics_draw_string_scaled(cx+cw-53, cy+41, "RUN", COL_BLACK, COL_BTN_FACE, 1);
        draw_string_bounded(cx+12, cy+70, cw-24,
                            "Try: ai, calc, term, paint, browser, mine, ttt, img",
                            0xFF555555, COL_WIN_BODY, 1);
    }
    else if (w->type == APP_TERMINAL) {
        draw_bevel_box(cx+2, cy+2, cw-4, ch-4, true);
        graphics_fill_rect(cx+4, cy+4, cw-8, ch-8, COL_BLACK);
        for(int i=0; i<6; i++)
            draw_string_bounded(cx+6, cy+6+(i*10), cw-12,
                                w->state.term.history[i], 0xFF00FF00, COL_BLACK, 1);
        int input_y = cy+66;
        graphics_draw_string_scaled(cx+6, input_y, w->state.term.prompt, 0xFF00FF00, COL_BLACK, 1);
        int pw = kstrlen_local(w->state.term.prompt)*8;
        int input_chars = (cw - 12 - pw) / 8;
        if (input_chars < 1) input_chars = 1;
        int input_offset = w->state.term.input_len > input_chars
                         ? w->state.term.input_len - input_chars : 0;
        draw_string_bounded(cx+6+pw, input_y, cw-12-pw,
                            w->state.term.input + input_offset,
                            COL_WHITE, COL_BLACK, 1);
        if ((timer_get_ticks()/15)%2) {
            int shown = w->state.term.input_len - input_offset;
            graphics_fill_rect(cx+6+pw+(shown*8), input_y, 2, 8, 0xFF00FF00);
        }
    }
    else if (w->type == APP_CALC) {
        char buf[16]; int_to_str(w->state.calc.current_val, buf);
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
        draw_bevel_box(cx+2, cy+2, cw-4, ch-4, true);
        graphics_fill_rect(cx+4, cy+4, cw-8, ch-8, COL_WHITE);
        graphics_fill_rect(cx+4, cy+4, cw-8, 18, 0xFFCCCCCC);
        graphics_draw_string_scaled(cx+8, cy+8, "Name", COL_BLACK, 0xFFCCCCCC, 1);
        size_t count = fs_file_count();
        for (size_t i=0; i<count; i++) {
            const struct fs_file* f = fs_file_at(i); if(!f) continue;
            int ry = cy+24 + i*18;
            if (ry + 18 > w->y + w->h - 4) break;
            bool sel = ((int)i == w->state.files.selected_index);
            if (sel) graphics_fill_rect(cx+4, ry, cw-8, 18, 0xFF000080);
            draw_string_bounded(cx+20, ry+4, cw-90, f->name,
                                sel?COL_WHITE:COL_BLACK,
                                sel?0xFF000080:COL_WHITE, 1);
            char sz[16]; int_to_str((int)f->size, sz);
            graphics_draw_string_scaled(cx+cw-60, ry+4, sz, sel?COL_WHITE:COL_BLACK, sel?0xFF000080:COL_WHITE, 1);
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
        {20, 160, "Paint", APP_PAINT}, {20, 230, "Browser", APP_BROWSER},
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
    create_window(APP_WELCOME, "Welcome", 350, 200);

    while(g_gui_running) {
        desktop_yield();
        char c = keyboard_poll_char();
        if (c == 27) break; 
        Window* top = get_top_window();
        if (c && top && top->focused) {
            if(top->type == APP_TERMINAL) handle_terminal_input(top, c);
            else if(top->type == APP_BROWSER) handle_browser_input(top, c);
            else if(top->type == APP_ASSISTANT) handle_assistant_input(top, c);
            else if(top->type == APP_RUN) {
                RunState* r = &top->state.run;
                if (c == '\n') handle_run_command(top);
                else if (c == '\b') { if(r->len>0) r->cmd[--r->len]=0; }
                else if (r->len < 30) { r->cmd[r->len++]=c; r->cmd[r->len]=0; }
            }
            else if (top->type == APP_NOTEPAD) {
                NotepadState* ns = &top->state.notepad;
                if (c == '\b') { if (ns->length > 0) ns->buffer[--ns->length] = 0; }
                else if (c >= 32 && c <= 126 && ns->length < 510) { ns->buffer[ns->length++] = c; ns->buffer[ns->length] = 0; }
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
            } else if (top->type == APP_PAINT &&
                       rect_contains(top->x + 6, top->y + WIN_CAPTION_H + 46,
                                     top->w - 12, top->h - WIN_CAPTION_H - 52,
                                     mouse.x, mouse.y)) {
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

        // Fixed: Added SysMon update logic
        for(int i=0; i<MAX_WINDOWS; i++) {
            if(windows[i] && windows[i]->type == APP_SYSMON) {
                 update_sysmon(windows[i]);
            }
        }

        render_desktop();
        graphics_swap_buffer();
    }
    
    while (windows[0]) close_window(0);
    graphics_disable_double_buffer();
    g_gui_running = false;
}
