#ifndef GUI_DEFS_H
#define GUI_DEFS_H

#include <stdbool.h>
#include <stdint.h>

// --- Constants ---
#define MAX_WINDOWS 16
#define WIN_CAPTION_H 26
#define TASKBAR_H 32

// --- Colors (Aero Basic) ---
#define COL_DESKTOP     0xFF2D73A8
#define COL_TASKBAR     0xFF18334E
#define COL_START_BTN   0xFF1F4E79
#define COL_WIN_ACTIVE  0xFF6B95BD
#define COL_WIN_INACT   0xFF888888
#define COL_WIN_BODY    0xFFF0F0F0
#define COL_WHITE       0xFFFFFFFF
#define COL_BLACK       0xFF000000
#define COL_RED         0xFFE81123

// --- App Types ---
typedef enum {
    APP_NONE = 0,
    APP_WELCOME,
    APP_NOTEPAD,
    APP_CALC
} AppType;

// --- App States ---
typedef struct {
    char buffer[256];
    int length;
    int cursor_pos;
} NotepadState;

typedef struct {
    int val;
    int acc;
    char op;
    bool new_input;
} CalcState;

// --- Window Structure ---
typedef struct WindowStruct {
    int id;
    AppType type;
    char title[32];
    int x, y, w, h;
    bool visible;
    bool minimized;
    bool focused;
    bool dragging;
    int drag_off_x;
    int drag_off_y;
    
    union {
        NotepadState notepad;
        CalcState calc;
    } state;
} Window;

// --- Window Manager API ---
void wm_init(int screen_w, int screen_h);
Window* wm_create_window(AppType type, const char* title, int w, int h);
void wm_handle_click(int x, int y);
void wm_handle_drag(int x, int y);
void wm_handle_release(void);
void wm_handle_key(char c);
void wm_render_all(void);

#endif
