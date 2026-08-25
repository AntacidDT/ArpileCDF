/* Arpile Settings application: Wi-Fi / Display / Input / System / About.
 * Reuses existing subsystem APIs only (wifi_test, ili9488, esp_* helpers). */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_chip_info.h"
#include "arpile_app.h"
#include "arpile_ui.h"
#include "arpile_ui_widgets.h"
#include "wifi_test.h"

#define SEC_WIFI   0
#define SEC_DISP   1
#define SEC_INPUT  2
#define SEC_SYS    3
#define SEC_ABOUT  4
#define SEC_COUNT  5

typedef struct {
    int section;
    int sel;        /* action row within section */
    char msg[48];
} settings_state_t;

static const char *const k_tabs[SEC_COUNT] = {
    "Wi-Fi", "Display", "Input", "System", "About",
};

/* Per-section selectable actions (index -> meaning). */
static const int k_wifi_actions = 2;    /* [Open WiFi app] [Forget saved] */
static const int k_disp_actions = 1;    /* [Toggle backlight] */
static const int k_sys_actions  = 2;    /* [Reboot] [Clear saved net] */

static int section_actions(int sec)
{
    switch (sec) {
    case SEC_WIFI:  return k_wifi_actions;
    case SEC_DISP:  return k_disp_actions;
    case SEC_SYS:   return k_sys_actions;
    default:        return 0;
    }
}

static void set_msg(settings_state_t *st, const char *m)
{
    snprintf(st->msg, sizeof(st->msg), "%s", m);
}

static void run_action(arpile_app_ctx_t *ctx, settings_state_t *st, int action)
{
    switch (st->section) {
    case SEC_WIFI:
        if (action == 0) {
            arpile_app_launch("wifi");
            set_msg(st, "Opened WiFi");
        } else if (action == 1) {
            if (arpile_wifi_has_saved()) {
                arpile_wifi_forget();
                set_msg(st, "Saved network forgotten");
            } else {
                set_msg(st, "No saved network");
            }
        }
        break;
    case SEC_DISP:
        if (action == 0) {
            ili9488_t *lcd = arpile_ui_get_lcd();
            static bool s_bl_on = true;
            s_bl_on = !s_bl_on;
            ili9488_backlight(lcd, s_bl_on);
            set_msg(st, s_bl_on ? "Backlight on" : "Backlight off");
        }
        break;
    case SEC_SYS:
        if (action == 0) {
            set_msg(st, "Rebooting...");
            arpile_ui_win_redraw(ctx->win);
            vTaskDelay(pdMS_TO_TICKS(400));
            esp_restart();
        } else if (action == 1) {
            if (arpile_wifi_has_saved()) {
                arpile_wifi_forget();
                set_msg(st, "Saved network forgotten");
            } else {
                set_msg(st, "No saved network");
            }
        }
        break;
    }
}

/* ---------------- render ---------------- */

static void paint_actions(ili9488_t *lcd, settings_state_t *st,
                          const ui_rect_t *c, int y, const char *const *labels,
                          int count)
{
    /* Size buttons to the widest label in the group so the text always
     * fits inside its frame. */
    int bw = 120;
    for (int i = 0; i < count; i++) {
        int w = (int)ui_text_width(labels[i]) + 24;
        if (w > bw) {
            bw = w;
        }
    }
    if (bw > (int)c->w - 24) {
        bw = (int)c->w - 24;
    }
    for (int i = 0; i < count; i++) {
        ui_rect_t br = { (uint16_t)(c->x + 12), (uint16_t)y,
                         (uint16_t)bw, (uint16_t)(CHAR_H + 8) };
        ui_button_t b = { br, labels[i], st->sel == i, st->sel == i };
        ui_paint_button(lcd, &b);
        y += CHAR_H + 14;
    }
    if (count > 0 && st->sel >= count) {
        st->sel = count - 1;
    }
}

static void render_section(arpile_app_ctx_t *ctx, settings_state_t *st,
                           const ui_rect_t *c)
{
    ili9488_t *lcd = arpile_ui_get_lcd();
    int x = c->x + 12;
    int y = c->y + 10;
    char line[80];

    switch (st->section) {
    case SEC_WIFI: {
        y = ui_kit_header(lcd, x, y, "Wi-Fi");
        arpile_wifi_state_t wst = arpile_wifi_get_state();
        const char *state =
            wst == ARPILE_WIFI_CONNECTED ? "Connected" :
            wst == ARPILE_WIFI_CONNECTING ? "Connecting..." : "Disconnected";
        y = ui_kit_kv(lcd, x, y, "State", state);
        if (wst == ARPILE_WIFI_CONNECTED) {
            y = ui_kit_kv(lcd, x, y, "Network",
                          arpile_wifi_get_connected_ssid());
            const char *ip = arpile_wifi_get_ip();
            y = ui_kit_kv(lcd, x, y, "IP", ip[0] ? ip : "-");
        }
        y = ui_kit_kv(lcd, x, y, "Saved",
                      arpile_wifi_has_saved() ?
                      arpile_wifi_get_saved_ssid() : "(none)");
        y += 4;
        paint_actions(lcd, st, c, y,
                      (const char *[]){ "Open WiFi app...", "Forget saved" },
                      k_wifi_actions);
        break;
    }
    case SEC_DISP: {
        y = ui_kit_header(lcd, x, y, "Display");
        snprintf(line, sizeof(line), "%dx%d @ SPI 60 MHz", UI_W, UI_H);
        y = ui_kit_kv(lcd, x, y, "Panel", line);
        y = ui_kit_kv(lcd, x, y, "Type", "ILI9488 480x320 RGB565");
        y = ui_kit_kv(lcd, x, y, "Compositing", "dirty-rect incremental");
        y += 4;
        paint_actions(lcd, st, c, y,
                      (const char *[]){ "Toggle backlight" }, k_disp_actions);
        break;
    }
    case SEC_INPUT: {
        y = ui_kit_header(lcd, x, y, "Input");
        y = ui_kit_kv(lcd, x, y, "Keyboard", "USB Host HID (Rii 8 mini)");
        y = ui_kit_kv(lcd, x, y, "Pointer", "air mouse via HID receiver");
        y = ui_kit_kv_wrap(lcd, x, y, "Shortcuts",
                           "Alt+T terminal  Alt+W wifi  "
                           "Alt+F1..F5 close window",
                           c->w - 24);
        break;
    }
    case SEC_SYS: {
        y = ui_kit_header(lcd, x, y, "System");
        uint32_t ms = (uint32_t)(xTaskGetTickCount() * 1000 / configTICK_RATE_HZ);
        snprintf(line, sizeof(line), "%luh %lum %lus",
                 (unsigned long)(ms / 3600000),
                 (unsigned long)((ms / 60000) % 60),
                 (unsigned long)((ms / 1000) % 60));
        y = ui_kit_kv(lcd, x, y, "Uptime", line);
        y = ui_kit_kv(lcd, x, y, "ESP-IDF", esp_get_idf_version());
        const char *chip = "unknown";
        uint32_t chip_rev = 0;
        esp_chip_info_t ci;
        esp_chip_info(&ci);
        if (ci.model == CHIP_ESP32P4) chip = "ESP32-P4 Nano";
        (void)chip_rev;
        y = ui_kit_kv(lcd, x, y, "Hardware", chip);
        y = ui_kit_kv(lcd, x, y, "Radio", "ESP32-C6 via ESP-Hosted SDIO");
        y += 4;
        paint_actions(lcd, st, c, y,
                      (const char *[]){ "Reboot", "Forget saved network" },
                      k_sys_actions);
        break;
    }
    case SEC_ABOUT: {
        y = ui_kit_header(lcd, x, y, "About");
        y = ui_kit_kv(lcd, x, y, "System", "Arpile 32CDF v0.1");
        y = ui_kit_kv(lcd, x, y, "Kernel", "FreeRTOS on ESP-IDF v6");
        y = ui_kit_kv_wrap(lcd, x, y, "Apps",
                           "Terminal WiFi Settings Files Code Math Chem "
                           "SSH Physics Biology Wiki Media Sysmon Paint DOOM",
                           c->w - 24);
        y = ui_kit_kv(lcd, x, y, "Docs", "ArpileDocumentation/, lofs/");
        break;
    }
    }
}

static void settings_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    settings_state_t *st = ctx->user;
    if (!st) return;
    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);
    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);

    /* tab bar */
    ui_rect_t tabs = { (uint16_t)(c.x + 8), (uint16_t)(c.y + 6),
                       (uint16_t)(c.w - 16), (uint16_t)(CHAR_H + 10) };
    ui_kit_tabbar(lcd, &tabs, k_tabs, SEC_COUNT, st->section);

    /* content area below tabs */
    ui_rect_t body = c;
    body.y = (uint16_t)(tabs.y + tabs.h + 8);
    body.h = (uint16_t)(c.y + c.h - body.y - CHAR_H - 14);

    /* section-specific selection reset when switching */
    render_section(ctx, st, &body);

    /* status/message line */
    char right[24];
    snprintf(right, sizeof(right), "%d/%d", st->section + 1, SEC_COUNT);
    ui_kit_statusbar(lcd, &c, st->msg[0] ? st->msg :
                     "Tab: section  PgUp/PgDn: item  Enter: apply",
                     right);
}

/* ---------------- events ---------------- */

/* NOTE: arrow keys never reach apps - the desktop consumes them as mouse
 * cursor movement. Navigation uses the device convention instead:
 * Tab = switch section (Shift+Tab = previous), PgUp/PgDn = move item. */
static void settings_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    settings_state_t *st = ctx->user;
    if (!st || ev->type != ARPILE_IN_EVENT_KEY_DOWN) return;

    uint8_t m = ev->key.modifier;
    bool shift = (m & (ARPILE_MOD_LSHIFT | ARPILE_MOD_RSHIFT)) != 0;
    bool alt = (m & (ARPILE_MOD_LALT | ARPILE_MOD_RALT)) != 0;

    switch (ev->key.keycode) {
    case ARPILE_KEY_TAB:
        if (!alt) {
            int dir = shift ? SEC_COUNT - 1 : 1;
            st->section = (st->section + dir) % SEC_COUNT;
            st->sel = 0;
            st->msg[0] = 0;
            arpile_ui_win_redraw(ctx->win);
        }
        return;
    case ARPILE_KEY_PGUP:
        if (st->sel > 0) st->sel--;
        arpile_ui_win_redraw(ctx->win);
        return;
    case ARPILE_KEY_PGDN:
        if (st->sel < section_actions(st->section) - 1) st->sel++;
        arpile_ui_win_redraw(ctx->win);
        return;
    case ARPILE_KEY_ENTER:
        run_action(ctx, st, st->sel);
        arpile_ui_win_redraw(ctx->win);
        return;
    case ARPILE_KEY_ESCAPE:
        if (!alt && !shift) {
            arpile_app_close(ctx);
        }
        return;
    }
}

/* ---------------- lifecycle ---------------- */

static void settings_init(arpile_app_ctx_t *ctx)
{
    settings_state_t *st = calloc(1, sizeof(settings_state_t));
    ctx->user = st;
    st->section = SEC_WIFI;
}

static void settings_destroy(arpile_app_ctx_t *ctx)
{
    if (ctx->user) {
        free(ctx->user);
        ctx->user = NULL;
    }
}

static const arpile_app_ops_t settings_ops = {
    .init = settings_init,
    .event = settings_event,
    .render = settings_render,
    .destroy = settings_destroy,
};

const arpile_app_t arpile_app_settings = {
    .id = "settings",
    .name = "Settings",
    .icon = "gearbox",
    .ops = &settings_ops,
};
