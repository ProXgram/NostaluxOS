#include "gui_defs.h"
#include "graphics.h"
#include "kstring.h"
#include "syslog.h"
#include "io.h" // For Shutdown
#include <stddef.h>

static Window windows[MAX_WINDOWS];
static int screen_width;
static int screen_height;
static bool start_menu_open = false;

// --- Helpers ---
static bool rect_contains(int x, int y, int w, int h, int px, int py) {
    return (px >= x && px < x + w && py >= y && py < y + h);
}

static void bring_to_front(int idx) {
    if (idx < 0 || idx >= MAX_WINDOWS - 1) return;
    Window temp = windows[idx];
    for (int i = idx; i < MAX_WINDOWS - 1; i++) {
        windows[i] = windows[i+1];
    }
    windows[MAX_WINDOWS - 1] = temp;
    
    // Focus logic
    for(int i=0; i<MAX_WINDOWS; i++) windows[i].focused = false;
    windows[MAX_WINDOWS-1].focused = true;
    windows[MAX_WINDOWS-1].minimized = false;
}

// --- Public API ---

void wm_init(int w, int h) {
    screen_width = w;
    screen_height = h;
    start_menu_open = false;
    for(int i=0; i<MAX_WINDOWS; i++) windows[i].visible = false;
}

Window* wm_create_window(AppType type, const char* title, int w, int h) {
    int slot = -1;
    // Find empty
    for(int i=0; i<MAX_WINDOWS; i++) {
        if (!windows[i].visible) { slot = i; break; }
    }
    // If full, recycle bottom-most
    if (slot == -1) slot = 0;

    Window* win = &windows[slot];
    win->type = type;
    win->visible = true;
    win->minimized = false;
    win->dragging = false;
    win->w = w;
    win->h = h;
    win->x = 50 + (slot * 30); // Cascade
    win->y = 50 + (slot * 30);
    
    // Copy Title
    int i=0; while(title[i] && i<31) { win->title[i] = title[i]; i++; }
    win->title[i] = 0;

    // Init State
    if (type == APP_CALC) {
        win->state.calc.val = 0;
        win->state.calc.new_input = true;
    } else if (type == APP_NOTEPAD) {
        win->state.notepad.length = 0;
        win->state.notepad.buffer[0] = 0;
    }

    bring_to_front(slot);
    return &windows[MAX_WINDOWS-1];
}

void wm_handle_click(int x, int y) {
    int ty = screen_height - TASKBAR_H;

    // 1. Start Menu Toggle
    if (rect_contains(0, ty, 80, TASKBAR_H, x, y)) {
        start_menu_open = !start_menu_open;
        return;
    }

    // 2. Start Menu Items
    if (start_menu_open) {
        int my = ty - 200;
        if (rect_contains(0, my, 160, 200, x, y)) {
            int local_y = y - my;
            if (local_y > 160) { // Shutdown
                outw(0x604, 0x2000);
            } else if (local_y < 40) {
                wm_create_window(APP_NOTEPAD, "Notepad", 300, 200);
            } else if (local_y < 80) {
                wm_create_window(APP_CALC, "Calculator", 220, 300);
            }
            start_menu_open = false;
            return;
        }
        start_menu_open = false; // Close if clicked outside
        // Continue to check if we clicked a window behind the menu area
    }

    // 3. Taskbar
    if (y >= ty) {
        int bx = 90;
        for(int i=0; i<MAX_WINDOWS; i++) {
            if (windows[i].visible) {
                if (rect_contains(bx, ty, 100, TASKBAR_H, x, y)) {
                    if (windows[i].focused && !windows[i].minimized) {
                        windows[i].minimized = true;
                    } else {
                        windows[i].minimized = false;
                        bring_to_front(i);
                    }
                    return;
                }
                bx += 105;
            }
        }
        return;
    }

    // 4. Windows (Top-Down Hit Test)
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        Window* w = &windows[i];
        if (!w->visible || w->minimized) continue;

        if (rect_contains(w->x, w->y, w->w, w->h, x, y)) {
            bring_to_front(i);
            w = &windows[MAX_WINDOWS - 1]; // Ptr updated after move

            // Controls
            int cx = w->x + w->w - 24;
            int mx = cx - 22;
            
            if (rect_contains(cx, w->y+4, 18, 18, x, y)) { // Close
                w->visible = false;
                return;
            }
            if (rect_contains(mx, w->y+4, 18, 18, x, y)) { // Min
                w->minimized = true;
                return;
            }
            if (y < w->y + WIN_CAPTION_H) { // Drag
                w->dragging = true;
                w->drag_off_x = x - w->x;
                w->drag_off_y = y - w->y;
                return;
            }
            
            // App Interaction (Calculator Buttons)
            if (w->type == APP_CALC) {
                // Simple hit logic for keypad
                // Implementation detail: Calc logic handled here or separate function
            }
            
            return;
        }
    }
    
    // 5. Desktop Icons
    if (rect_contains(20, 20, 60, 50, x, y)) {
        wm_create_window(APP_WELCOME, "My PC", 300, 200);
    }
}

void wm_handle_drag(int x, int y) {
    Window* top = &windows[MAX_WINDOWS - 1];
    if (top->visible && top->dragging) {
        top->x = x - top->drag_off_x;
        top->y = y - top->drag_off_y;
    }
}

void wm_handle_release(void) {
    for(int i=0; i<MAX_WINDOWS; i++) windows[i].dragging = false;
}

void wm_handle_key(char c) {
    Window* w = &windows[MAX_WINDOWS - 1];
    if (!w->visible || w->minimized || !w->focused) return;

    if (w->type == APP_NOTEPAD) {
        NotepadState* ns = &w->state.notepad;
        if (c == '\b') {
            if (ns->length > 0) ns->buffer[--ns->length] = 0;
        } else if (c >= 32 && c <= 126) {
            if (ns->length < 254) {
                ns->buffer[ns->length++] = c;
                ns->buffer[ns->length] = 0;
            }
        }
    }
}

// --- Rendering ---
// Note: This really belongs in compositor.c, but for C compatibility without circular
// deps in this flat structure, we implement the drawing calls here using the graphics API.

static void draw_rect(int x, int y, int w, int h, uint32_t c) {
    graphics_fill_rect(x, y, w, h, c);
}

static void draw_window_frame(Window* w) {
    // Shadow
    draw_rect(w->x+6, w->y+6, w->w, w->h, 0x40000000);
    // Border
    uint32_t bc = w->focused ? COL_WIN_ACTIVE : COL_WIN_INACT;
    draw_rect(w->x, w->y, w->w, w->h, bc);
    draw_rect(w->x+2, w->y+2, w->w-4, w->h-4, bc);
    // Title
    draw_rect(w->x+2, w->y+2, w->w-4, WIN_CAPTION_H, bc);
    graphics_draw_string_scaled(w->x+8, w->y+6, w->title, COL_BLACK, bc, 1);
    // Controls
    int cx = w->x + w->w - 24;
    draw_rect(cx, w->y+4, 18, 18, COL_RED);
    graphics_draw_char(cx+6, w->y+5, 'X', COL_WHITE, COL_RED);
    
    // Client
    draw_rect(w->x+4, w->y+WIN_CAPTION_H+4, w->w-8, w->h-WIN_CAPTION_H-8, COL_WIN_BODY);
    
    // Content
    int client_x = w->x+10;
    int client_y = w->y+WIN_CAPTION_H+10;
    
    if (w->type == APP_NOTEPAD) {
        graphics_draw_string_scaled(client_x, client_y, w->state.notepad.buffer, COL_BLACK, COL_WIN_BODY, 1);
        // Cursor
        int cw = kstrlen(w->state.notepad.buffer) * 8;
        draw_rect(client_x + cw, client_y, 2, 10, COL_BLACK);
    } else if (w->type == APP_CALC) {
        graphics_draw_string_scaled(client_x, client_y, "0", COL_BLACK, COL_WIN_BODY, 2);
        // Draw fake buttons
        for(int i=0; i<4; i++) draw_rect(client_x + i*35, client_y+40, 30, 25, 0xFFDDDDDD);
    } else {
        graphics_draw_string_scaled(client_x, client_y, "Nostalux App", COL_BLACK, COL_WIN_BODY, 1);
    }
}

void wm_render_all(void) {
    // Desktop
    draw_rect(0, 0, screen_width, screen_height, COL_DESKTOP);
    graphics_draw_string_scaled(20, 20, "My PC", COL_WHITE, COL_DESKTOP, 1);
    
    // Windows
    for(int i=0; i<MAX_WINDOWS; i++) {
        if (windows[i].visible && !windows[i].minimized) {
            draw_window_frame(&windows[i]);
        }
    }
    
    // Taskbar
    int ty = screen_height - TASKBAR_H;
    draw_rect(0, ty, screen_width, TASKBAR_H, COL_TASKBAR);
    draw_rect(0, ty, 80, TASKBAR_H, start_menu_open ? 0xFF3465A4 : COL_START_BTN);
    graphics_draw_string_scaled(15, ty+10, "Start", COL_WHITE, COL_START_BTN, 1);
    
    // Tabs
    int bx = 90;
    for(int i=0; i<MAX_WINDOWS; i++) {
        if (windows[i].visible) {
            uint32_t tc = (windows[i].focused && !windows[i].minimized) ? 0xFF3A6EA5 : 0xFF2A4E75;
            draw_rect(bx, ty+2, 100, TASKBAR_H-4, tc);
            char buf[12]; int k=0; while(k<10 && windows[i].title[k]){buf[k]=windows[i].title[k]; k++;} buf[k]=0;
            graphics_draw_string_scaled(bx+5, ty+10, buf, COL_WHITE, tc, 1);
            bx += 105;
        }
    }
    
    // Start Menu
    if (start_menu_open) {
        int my = ty - 200;
        draw_rect(0, my, 160, 200, COL_WIN_BODY);
        graphics_draw_string_scaled(10, my+10, "Notepad", COL_BLACK, COL_WIN_BODY, 1);
        graphics_draw_string_scaled(10, my+40, "Calculator", COL_BLACK, COL_WIN_BODY, 1);
        draw_rect(0, my+160, 160, 40, 0xFF333333);
        graphics_draw_string_scaled(10, my+170, "Shutdown", COL_WHITE, 0xFF333333, 1);
    }
}
