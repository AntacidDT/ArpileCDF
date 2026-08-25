#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "arpile_ui_core.h"
#include "arpile_ui_draw.h"
#include "ili9488.h"
#include "usb_host_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Arpile desktop: KDE-style shell + window manager + UI widget layer.
 *
 * Applications create windows and receive keyboard/pointer input. The shell
 * (panel, taskbar, launcher, clock, background) is owned by this module.
 *
 * Rendering model: no full framebuffer. Each widget/event handler calls the
 * ui_draw_* ops to repaint exactly the region it owns. Window compositing is
 * done by redrawing affected windows top-to-bottom; the framework minimises
 * repaints by tracking a per-window dirty region.
 */

#define UI_MAX_WINDOWS  8
#define UI_WIN_TITLE_LEN 24

typedef struct ui_win ui_win_t;

typedef struct {
    ui_rect_t rect;          /* window position/size (in window, before decorations) */
    char title[UI_WIN_TITLE_LEN];
    bool focused;
    bool visible;
    bool minimized;
    /* When true the window fills the whole desktop area above the taskbar
     * (title bar pinned to the top; taskbar is never covered). */
    bool maximized;
    ui_rect_t normal_rect;   /* saved geometry to restore from maximized */
    /* When true, the desktop forwards arrow keys to this window's on_key
     * instead of using them to move the mouse cursor (text editors, etc.). */
    bool capture_nav;
} ui_win_state_t;

/** A window's event-handling interface. Every callback optional. */
typedef struct {
    /** Called to repaint the window content area. */
    void (*on_paint)(ui_win_t *win);
    void (*on_key)(ui_win_t *win, const arpile_input_event_t *ev);
    void (*on_mouse)(ui_win_t *win, const arpile_input_event_t *ev);
    void (*on_focus)(ui_win_t *win, bool focused);
    void (*on_close)(ui_win_t *win);
} ui_win_ops_t;

typedef struct ui_win {
    uint8_t id;
    ui_win_state_t state;
    ui_win_ops_t ops;
    void *user;
    struct ui_win *prev, *next;   /* z-order: head = top */
} ui_win_t;

typedef enum {
    UI_LAUNCH_TAG_TERMINAL = 0,
} ui_launch_t;

/* ---- lifecycle ---- */
esp_err_t arpile_ui_start(ili9488_t *lcd);

/* ---- window management (used by apps) ---- */
ui_win_t *arpile_ui_win_new(const char *title, int x, int y, int w, int h,
                            const ui_win_ops_t *ops, void *user);
void arpile_ui_win_close(ui_win_t *win);
void arpile_ui_win_focus(ui_win_t *win);
void arpile_ui_win_move(ui_win_t *win, int x, int y);
void arpile_ui_win_resize(ui_win_t *win, int w, int h);
void arpile_ui_win_set_title(ui_win_t *win, const char *title);
void arpile_ui_win_redraw(ui_win_t *win);

/* Get the client area rect of a window (excluding title bar and borders). */
ui_rect_t win_client_rect(const ui_win_t *w);

/* ---- shell ---- */
void arpile_ui_launcher_open(void);
void arpile_ui_launcher_close(void);
bool arpile_ui_launcher_is_open(void);
void arpile_ui_update_clock(void);

/* Radial health / status getters (for status bar). */
bool arpile_ui_is_started(void);

/* Get the LCD device (for applications that need to draw directly). */
ili9488_t *arpile_ui_get_lcd(void);

/* Unlink/free a window slot (UI task only). */
void arpile_ui_win_teardown(ui_win_t *win);

/* Request closing the app that owns this window (deferred to the UI task). */
void arpile_app_close_win(ui_win_t *win);

#ifdef __cplusplus
}
#endif