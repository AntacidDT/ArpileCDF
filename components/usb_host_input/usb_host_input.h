#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Structured input events for the desktop / UI layer -----------------
 * This extends (not replaces) the existing text callback API. The UI layer
 * registers one pointer handler to receive typed characters and synthetic
 * key/mouse events. Key codes use USB HID usage values; modifiers use the
 * HID modifier bit mask.
 */

/** HID usage recomputed for arrows / nav so UI doesn't need hid headers. */
#define ARPILE_KEY_ESCAPE    0x29
#define ARPILE_KEY_BACKSPACE 0x2A
#define ARPILE_KEY_TAB       0x2B
#define ARPILE_KEY_ENTER     0x28
#define ARPILE_KEY_SPACE     0x2C
#define ARPILE_KEY_UP        0x52
#define ARPILE_KEY_DOWN      0x51
#define ARPILE_KEY_LEFT      0x50
#define ARPILE_KEY_RIGHT     0x4F
#define ARPILE_KEY_HOME      0x4A
#define ARPILE_KEY_END       0x4D
#define ARPILE_KEY_DELETE    0x4C
#define ARPILE_KEY_PGUP      0x4B
#define ARPILE_KEY_PGDN      0x4E
#define ARPILE_KEY_INS       0x49
#define ARPILE_KEY_A         0x04
#define ARPILE_KEY_E         0x08
#define ARPILE_KEY_F1        0x3A
#define ARPILE_KEY_F2        0x3B
#define ARPILE_KEY_F4        0x3D
#define ARPILE_KEY_F5        0x3E
#define ARPILE_KEY_F7        0x40
#define ARPILE_KEY_F10       0x43
#define ARPILE_KEY_W         0x1A
#define ARPILE_KEY_S         0x16
#define ARPILE_KEY_D         0x07
#define ARPILE_KEY_Q         0x14
#define ARPILE_KEY_O         0x12
#define ARPILE_KEY_P         0x13
#define ARPILE_KEY_L         0x0F
/* Top-row digits (HID keyboard/1..8). */
#define ARPILE_KEY_1         0x1E
#define ARPILE_KEY_2         0x1F
#define ARPILE_KEY_3         0x20
#define ARPILE_KEY_4         0x21
#define ARPILE_KEY_5         0x22
#define ARPILE_KEY_6         0x23
#define ARPILE_KEY_7         0x24
#define ARPILE_KEY_8         0x25

/* HID modifier bit masks (LEFT_CTRL=0x01, LEFT_SHIFT=0x02, LEFT_ALT=0x04 ...) */
#define ARPILE_MOD_LCTRL   0x01
#define ARPILE_MOD_LSHIFT  0x02
#define ARPILE_MOD_LALT    0x04
#define ARPILE_MOD_LGUI    0x08
#define ARPILE_MOD_RCTRL   0x10
#define ARPILE_MOD_RSHIFT  0x20
#define ARPILE_MOD_RALT    0x40
#define ARPILE_MOD_RGUI    0x80

typedef enum {
    ARPILE_IN_EVENT_KEY_DOWN,     /* raw key press (keycode in `key`) */
    ARPILE_IN_EVENT_KEY_UP,       /* raw key release */
    ARPILE_IN_EVENT_MOUSE_MOVE,   /* pointer moved (relative deltas in `mouse`) */
    ARPILE_IN_EVENT_MOUSE_BTN,    /* button state changed (press/release) */
    ARPILE_IN_EVENT_MOUSE_WHEEL,  /* scroll wheel (`wheel` signed clicks) */
} arpile_input_event_type_t;

#define ARPILE_MOUSE_BTN_LEFT   0x01
#define ARPILE_MOUSE_BTN_RIGHT  0x02

typedef struct {
    arpile_input_event_type_t type;
    union {
        struct {
            uint16_t keycode;     /* USB HID usage; see ARPILE_KEY_* */
            uint8_t  modifier;    /* HID modifier mask */
            char     ascii;       /* printable char, or 0 for non-printable */
        } key;
        struct {
            int  x, y;            /* relative displacement (deltas from driver) */
            uint8_t buttons;      /* ARPILE_MOUSE_BTN_* bitmask */
        } mouse;
        int wheel;                /* ARPILE_IN_EVENT_MOUSE_WHEEL: +up / -down */
    };
} arpile_input_event_t;

/**
 * Optional structured event callback. Called from the USB HID task with each
 * decoded key/mouse event. The event pointer is only valid for the call.
 * Set to NULL (default) to disable. Strings are still one event per char.
 */
void arpile_usb_host_set_event_cb(void (*cb)(const arpile_input_event_t *ev));

/**
 * Optional callback, invoked from the USB HID task with a line of text to
 * surface to the user (connect/disconnect notices or typed characters on a
 * report). Runs in a lower-priority task context, so keep it cheap. The line
 * is only valid for the duration of the call.
 *
 * Set to NULL (default) to disable.
 */
void arpile_usb_host_set_text_cb(void (*cb)(const char *text));

/**
 * Initialize the USB Host stack and HID class driver.
 * Installs usb_host on the OTG port, installs the HID host driver, and spawns
 * background tasks that detect/enumerate HID devices and print decoded
 * keyboard/mouse events to the console. Returns ESP_OK on success.
 *
 * This is a hardware validation layer only — it does not build the desktop
 * input system. Call once at startup; it runs indefinitely.
 */
esp_err_t arpile_usb_host_init(void);

#ifdef __cplusplus
}
#endif