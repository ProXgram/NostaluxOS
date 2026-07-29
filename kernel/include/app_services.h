#ifndef APP_SERVICES_H
#define APP_SERVICES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct paging_address_space;

/*
 * Dispatches the capability-gated Apps v1 services that own kernel resources.
 * The caller has already established that process_id and address_space belong
 * to the currently executing ring-3 task.
 */
uint64_t app_services_dispatch(
    uint64_t syscall_id,
    struct paging_address_space* address_space,
    uint64_t process_id,
    uint64_t granted_capabilities,
    uint64_t argument1,
    uint64_t argument2,
    uint64_t argument3,
    uint64_t argument4,
    uint64_t argument5);

/* Releases every file, window, and mapping record owned by one process. */
void app_services_release_process(uint64_t process_id);

/*
 * Grants one process permission to focus the next window it successfully
 * creates. The runtime calls this only for a foreground, user-requested app
 * launch. The grant is one-shot and is discarded across focus boundaries.
 */
bool app_services_authorize_launch_focus(uint64_t process_id);

/* Returns the number of retained app windows, including close-pending ones. */
size_t app_services_window_count(void);

/*
 * Queues one close request for every retained app window while leaving the
 * desktop active so applications can save, report errors, and repaint.
 * Returns the number of live windows that were asked to close.
 */
size_t app_services_request_all_windows_close(void);

/*
 * Hides or restores retained app windows for the desktop's Show Desktop
 * command without destroying their processes or queued close requests.
 */
void app_services_set_windows_hidden(bool hidden);

/*
 * Enables retained app windows only while the graphical desktop can composite
 * and route them. Deactivation asks every open app window to close.
 */
void app_services_set_desktop_active(bool active);

/*
 * Paints retained user-window surfaces after the kernel desktop but before
 * the back buffer is swapped. This function never dereferences user memory.
 */
void app_services_render_overlay(void);

/* Routes the same atomic mouse snapshot the desktop will handle. */
void app_services_route_pointer(
    int x, int y, bool left_button, bool right_button);

/* True when a desktop pointer event belongs to a retained app window. */
bool app_services_pointer_captured(int x, int y);

/* True from an app-owned button press through its matching release. */
bool app_services_pointer_gesture_active(void);

/* Prevents the kernel desktop from consuming the focused app's keypresses. */
bool app_services_has_keyboard_focus(void);

/* Transfers keyboard focus back to a kernel-owned desktop window. */
void app_services_blur_keyboard_focus(void);

#endif /* APP_SERVICES_H */
