#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"
#include "usb_host_input.h"

static const char *TAG = "arpile_usbhost";

#define KEYBOARD_ENTER_MAIN_CHAR   '\r'
#define KEYBOARD_ENTER_LF_EXTEND   1

const uint8_t keycode2ascii[57][2] = {
    {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {'a', 'A'}, {'b', 'B'}, {'c', 'C'}, {'d', 'D'}, {'e', 'E'}, {'f', 'F'},
    {'g', 'G'}, {'h', 'H'}, {'i', 'I'}, {'j', 'J'}, {'k', 'K'}, {'l', 'L'},
    {'m', 'M'}, {'n', 'N'}, {'o', 'O'}, {'p', 'P'}, {'q', 'Q'}, {'r', 'R'},
    {'s', 'S'}, {'t', 'T'}, {'u', 'U'}, {'v', 'V'}, {'w', 'W'}, {'x', 'X'},
    {'y', 'Y'}, {'z', 'Z'},
    {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'},
    {'6', '^'}, {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'},
    {KEYBOARD_ENTER_MAIN_CHAR, KEYBOARD_ENTER_MAIN_CHAR},
    {0, 0}, {'\b', 0}, {0, 0},
    {' ', ' '}, {'-', '_'}, {'=', '+'},
    {'[', '{'}, {']', '}'}, {'\\', '|'}, {'\\', '|'},
    {';', ':'}, {'\'', '"'}, {'`', '~'},
    {',', '<'}, {'.', '>'}, {'/', '?'}
};

typedef struct {
    enum key_state { KEY_STATE_PRESSED = 0x00, KEY_STATE_RELEASED = 0x01 } state;
    uint8_t modifier;
    uint8_t key_code;
} key_event_t;

typedef enum { APP_EVENT = 0, APP_EVENT_HID_HOST } app_event_group_t;

typedef struct {
    app_event_group_t event_group;
    struct {
        hid_host_device_handle_t handle;
        hid_host_driver_event_t event;
        void *arg;
    } hid_host_device;
} app_event_queue_t;

static QueueHandle_t app_event_queue = NULL;
static const char *hid_proto_name_str[] = { "NONE", "KEYBOARD", "MOUSE" };

static void (*s_text_cb)(const char *text) = NULL;
static void (*s_event_cb)(const arpile_input_event_t *ev) = NULL;

void arpile_usb_host_set_text_cb(void (*cb)(const char *text))
{
    s_text_cb = cb;
}

void arpile_usb_host_set_event_cb(void (*cb)(const arpile_input_event_t *ev))
{
    s_event_cb = cb;
}

static void notify_event(const arpile_input_event_t *ev)
{
    if (s_event_cb) {
        s_event_cb(ev);
    }
}

static void notify_text(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify_text(const char *fmt, ...)
{
    if (!s_text_cb) {
        return;
    }
    char buf[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_text_cb(buf);
}

static inline void hid_keyboard_print_char(unsigned int key_char)
{
    if (!!key_char) {
        putchar(key_char);
#if KEYBOARD_ENTER_LF_EXTEND
        if (KEYBOARD_ENTER_MAIN_CHAR == key_char) {
            putchar('\n');
        }
#endif
        fflush(stdout);
    }
}

static void hid_print_new_device_report_header(hid_protocol_t proto)
{
    static hid_protocol_t prev_proto_output = -1;
    if (prev_proto_output != proto) {
        prev_proto_output = proto;
        printf("\r\n");
        if (proto == HID_PROTOCOL_MOUSE) {
            printf("Mouse\r\n");
        } else if (proto == HID_PROTOCOL_KEYBOARD) {
            printf("Keyboard\r\n");
        } else {
            printf("Generic\r\n");
        }
        fflush(stdout);
    }
}

static inline bool hid_keyboard_is_modifier_shift(uint8_t modifier)
{
    return (((modifier & HID_LEFT_SHIFT) == HID_LEFT_SHIFT) ||
            ((modifier & HID_RIGHT_SHIFT) == HID_RIGHT_SHIFT));
}

static inline bool hid_keyboard_get_char(uint8_t modifier, uint8_t key_code,
                                        unsigned char *key_char)
{
    /* Control keys that numerically fall inside the printable range must NOT
     * be treated as letters (Backspace=0x2A, Tab=0x2B, Space=0x2C are within
     * [HID_KEY_A=0x04 .. HID_KEY_SLASH=0x38]). Let hid_keycode_to_ascii()'s
     * switch map them correctly instead. */
    switch (key_code) {
    case HID_KEY_ENTER:
    case HID_KEY_ESC:
    case 0x2A:            /* Backspace */
    case HID_KEY_TAB:
    case HID_KEY_SPACE:
    case HID_KEY_DELETE:
        return false;
    default:
        break;
    }
    uint8_t mod = (hid_keyboard_is_modifier_shift(modifier)) ? 1 : 0;
    if ((key_code >= HID_KEY_A) && (key_code <= HID_KEY_SLASH)) {
        *key_char = keycode2ascii[key_code][mod];
    } else {
        return false;
    }
    return true;
}

/* Translate a HID keycode to ASCII for special-but-printable keys, else 0. */
static char hid_keycode_to_ascii(uint8_t key_code, uint8_t modifier)
{
    unsigned char c;
    if (hid_keyboard_get_char(modifier, key_code, &c)) {
        return (char)c;
    }
    switch (key_code) {
    case HID_KEY_ENTER:            return '\r';
    case HID_KEY_TAB:              return '\t';
    case 0x2A:                     return '\b';   /* Backspace */
    case HID_KEY_SPACE:            return ' ';
    case HID_KEY_ESC:              return 0x1b;
    case HID_KEY_DELETE:           return 0x7f;
    default:                       return 0;
    }
}

static void key_event_callback(key_event_t *key_event)
{
    unsigned char key_char;
    hid_print_new_device_report_header(HID_PROTOCOL_KEYBOARD);
    if (KEY_STATE_PRESSED == key_event->state) {
        if (hid_keyboard_get_char(key_event->modifier, key_event->key_code, &key_char)) {
            hid_keyboard_print_char(key_char);
            char line[2] = { (char)key_char, 0 };
            notify_text(line);
        }
    }

    /* Structured event for the UI layer. */
    if (s_event_cb) {
        arpile_input_event_t ev;
        ev.type = (key_event->state == KEY_STATE_PRESSED)
                      ? ARPILE_IN_EVENT_KEY_DOWN
                      : ARPILE_IN_EVENT_KEY_UP;
        ev.key.keycode = key_event->key_code;
        ev.key.modifier = key_event->modifier;
        ev.key.ascii = hid_keycode_to_ascii(key_event->key_code, key_event->modifier);
        notify_event(&ev);
        if (key_event->key_code > 0x65) {
            printf("[KEYDBG] code=0x%02X mod=0x%02X\r\n",
                   key_event->key_code, key_event->modifier);
        }
    }
}

static inline bool key_found(const uint8_t *const src, uint8_t key, unsigned int length)
{
    for (unsigned int i = 0; i < length; i++) {
        if (src[i] == key) {
            return true;
        }
    }
    return false;
}

static void hid_host_keyboard_report_callback(const uint8_t *const data, const int length)
{
    hid_keyboard_input_report_boot_t *kb_report = (hid_keyboard_input_report_boot_t *)data;
    if (length < (int)sizeof(hid_keyboard_input_report_boot_t)) {
        return;
    }
    static uint8_t prev_keys[HID_KEYBOARD_KEY_MAX] = { 0 };
    static uint8_t prev_modifier = 0;
    key_event_t key_event;

    /* Detect Win/Meta (GUI modifier) presses/releases, which some keyboards
     * report only as a modifier bit (0x08 left, 0x80 right) with no keycode. */
    uint8_t mod = kb_report->modifier.val;
    if ((mod & 0x88) != (prev_modifier & 0x88)) {
        bool now_up = ((mod & 0x08) == 0) && ((mod & 0x80) == 0);
        if (!now_up) {
            key_event.key_code = (mod & 0x80) ? 0xE7 : 0xE3;   /* right GUI or left GUI */
            key_event.modifier = mod;
            key_event.state = KEY_STATE_PRESSED;
            key_event_callback(&key_event);
        } else if (prev_modifier != mod) {
            key_event.key_code = 0xE3;
            key_event.modifier = mod;
            key_event.state = KEY_STATE_RELEASED;
            key_event_callback(&key_event);
        }
    }
    prev_modifier = mod;

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        /* Modifier keycodes (0xE3/0xE7 GUI) never belong in the 6KRO array per
         * HID spec; they are handled above via the modifier-bit tracker. Skip
         * them here so non-conformant keyboards don't emit a duplicate event. */
        if (kb_report->key[i] == 0xE3 || kb_report->key[i] == 0xE7) {
            continue;
        }
        if (prev_keys[i] > HID_KEY_ERROR_UNDEFINED &&
                !key_found(kb_report->key, prev_keys[i], HID_KEYBOARD_KEY_MAX)) {
            key_event.key_code = prev_keys[i];
            key_event.modifier = 0;
            key_event.state = KEY_STATE_RELEASED;
            key_event_callback(&key_event);
        }
        if (kb_report->key[i] > HID_KEY_ERROR_UNDEFINED &&
                !key_found(prev_keys, kb_report->key[i], HID_KEYBOARD_KEY_MAX)) {
            key_event.key_code = kb_report->key[i];
            key_event.modifier = kb_report->modifier.val;
            key_event.state = KEY_STATE_PRESSED;
            key_event_callback(&key_event);
        }
    }
    memcpy(prev_keys, &kb_report->key, HID_KEYBOARD_KEY_MAX);
}

static void hid_host_mouse_report_callback(const uint8_t *const data, const int length)
{
    hid_mouse_input_report_boot_t *mouse_report = (hid_mouse_input_report_boot_t *)data;
    if (length < (int)sizeof(hid_mouse_input_report_boot_t)) {
        return;
    }
    static int x_pos = 0;
    static int y_pos = 0;
    static uint8_t prev_buttons = 0;
    /* Edge-tracking recovery: if a button is considered held but no transition
     * occurs for a while, the matching release was almost certainly dropped on
     * the wire/USB stack. Re-sync so the next physical press is seen as a fresh
     * edge instead of being swallowed as a "no-change" report -- otherwise
     * left-click launches die permanently after the first app close (a missed
     * release leaves prev_buttons stuck at LEFT). The timer is the ONLY guard:
     * it must NOT require "no movement", or a user who moves the mouse and then
     * clicks again (without pausing) would stay wedged forever. */
    static TickType_t s_last_btn_change = 0;
    static const TickType_t STUCK_BTN_MS = pdMS_TO_TICKS(1000);

    x_pos += mouse_report->x_displacement;
    y_pos -= mouse_report->y_displacement;  /* Invert Y: mouse up = negative displacement = cursor up */
    /* Clamp to the 480x320 display so the cursor stays on screen. */
    if (x_pos < 0) x_pos = 0;
    if (y_pos < 0) y_pos = 0;
    if (x_pos >= 480) x_pos = 479;
    if (y_pos >= 320) y_pos = 319;

    hid_print_new_device_report_header(HID_PROTOCOL_MOUSE);
    printf("X: %06d\tY: %06d\t|%c|%c|\r",
           x_pos, y_pos,
           (mouse_report->buttons.button1 ? 'o' : ' '),
           (mouse_report->buttons.button2 ? 'o' : ' '));
    notify_text("mouse %+d %+d", mouse_report->x_displacement,
                mouse_report->y_displacement);
    fflush(stdout);

    if (s_event_cb) {
        uint8_t btns = (mouse_report->buttons.button1 ? ARPILE_MOUSE_BTN_LEFT : 0)
                     | (mouse_report->buttons.button2 ? ARPILE_MOUSE_BTN_RIGHT : 0);
        arpile_input_event_t ev;
        ev.type = ARPILE_IN_EVENT_MOUSE_MOVE;
        ev.mouse.x = mouse_report->x_displacement;      /* send delta, not absolute */
        ev.mouse.y = mouse_report->y_displacement;
        ev.mouse.buttons = btns;
        /* Do not move the cursor on a report that changes the button state:
         * a click on a touchpad often bundles a small displacement that would
         * nudge the cursor and cause hit-testing (e.g. launcher tiles) to miss. */
        if (btns != prev_buttons) {
            ev.type = ARPILE_IN_EVENT_MOUSE_BTN;
            notify_event(&ev);
            prev_buttons = btns;
            s_last_btn_change = xTaskGetTickCount();
        } else {
            notify_event(&ev);   /* button unchanged: plain movement */
            /* Recovery against a dropped release (see comment on s_last_btn_change).
             * Triggers purely on time since the last transition, so it works even
             * while the cursor is moving. A genuine drag is briefly interrupted at
             * most once if held past STUCK_BTN_MS; clicks (the critical path) always
             * recover. */
            if (prev_buttons != 0 &&
                (xTaskGetTickCount() - s_last_btn_change) > STUCK_BTN_MS) {
                prev_buttons = 0;
                s_last_btn_change = xTaskGetTickCount();
                arpile_input_event_t rel = ev;
                rel.type = ARPILE_IN_EVENT_MOUSE_BTN;
                rel.mouse.buttons = 0;
                notify_event(&rel);
            }
        }
        /* Boot report is 3 bytes; many mice append a wheel byte at index 3. */
        if (length >= 4) {
            int8_t wheel = (int8_t)data[3];
            if (wheel != 0) {
                ev.type = ARPILE_IN_EVENT_MOUSE_WHEEL;
                ev.wheel = (wheel > 0) ? 1 : -1;
                notify_event(&ev);
            }
        }
    }
}

static void hid_host_generic_report_callback(const uint8_t *const data, const int length)
{
    hid_print_new_device_report_header(HID_PROTOCOL_NONE);
    for (int i = 0; i < length; i++) {
        printf("%02X", data[i]);
    }
    putchar('\r');
}

void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                 const hid_host_interface_event_t event,
                                 void *arg)
{
    uint8_t data[64] = { 0 };
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle,
                                                                  data, 64, &data_length));
        if (HID_PROTOCOL_MOUSE == dev_params.proto) {
            /* Route by protocol, not subclass: many touchpads (e.g. Rii 8) expose a
             * non-boot mouse interface whose reports still begin buttons + X + Y,
             * so the boot parser handles them while the generic path would just dump hex. */
            hid_host_mouse_report_callback(data, data_length);
        } else if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
            hid_host_keyboard_report_callback(data, data_length);
        } else {
            hid_host_generic_report_callback(data, data_length);
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID Device, protocol '%s' DISCONNECTED",
                 hid_proto_name_str[dev_params.proto]);
        notify_text("disconnect %s", hid_proto_name_str[dev_params.proto]);
        ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGI(TAG, "HID Device, protocol '%s' TRANSFER_ERROR",
                 hid_proto_name_str[dev_params.proto]);
        break;
    default:
        ESP_LOGW(TAG, "HID Device, protocol '%s' Unhandled event: %d",
                 hid_proto_name_str[dev_params.proto], event);
        break;
    }
}

void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                           const hid_host_driver_event_t event,
                           void *arg)
{
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
    case HID_HOST_DRIVER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "HID Device, protocol '%s' CONNECTED",
                 hid_proto_name_str[dev_params.proto]);
        notify_text("connect %s", hid_proto_name_str[dev_params.proto]);

        const hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL
        };
        if (dev_params.proto != HID_PROTOCOL_NONE) {
            ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
            if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
                ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle,
                                                              HID_REPORT_PROTOCOL_BOOT));
                if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                    ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
                }
            }
            ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
        }
        break;
    default:
        break;
    }
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }
    ESP_LOGI(TAG, "USB shutdown");
    vTaskDelay(10);
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                             const hid_host_driver_event_t event,
                             void *arg)
{
    const app_event_queue_t evt_queue = {
        .event_group = APP_EVENT_HID_HOST,
        .hid_host_device.handle = hid_device_handle,
        .hid_host_device.event = event,
        .hid_host_device.arg = arg
    };
    if (app_event_queue) {
        xQueueSend(app_event_queue, &evt_queue, 0);
    }
}

static void hid_app_task(void *arg)
{
    app_event_queue_t evt_queue;
    while (1) {
        if (xQueueReceive(app_event_queue, &evt_queue, portMAX_DELAY)) {
            if (APP_EVENT_HID_HOST == evt_queue.event_group) {
                hid_host_device_event(evt_queue.hid_host_device.handle,
                                     evt_queue.hid_host_device.event,
                                     evt_queue.hid_host_device.arg);
            }
        }
    }
}

esp_err_t arpile_usb_host_init(void)
{
    app_event_queue = xQueueCreate(10, sizeof(app_event_queue_t));
    if (!app_event_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                                xTaskGetCurrentTaskHandle(), 2, NULL, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    ulTaskNotifyTake(false, pdMS_TO_TICKS(2000));

    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_device_callback,
        .callback_arg = NULL
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

    if (xTaskCreatePinnedToCore(hid_app_task, "hid_app", 4096, NULL, 5, NULL, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "USB HID host ready; plug in a HID device");
    return ESP_OK;
}
