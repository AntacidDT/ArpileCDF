/* Arpile Terminal application */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "arpile_app.h"
#include "arpile_ui.h"
#include "wifi_test.h"

#define TERM_BUF_SIZE 2048
#define TERM_HISTORY 10
#define TERM_LINE_MAX 128

typedef struct {
    char buf[TERM_BUF_SIZE];
    int len;
    char line[TERM_LINE_MAX];
    int line_pos;
    char history[TERM_HISTORY][TERM_LINE_MAX];
    int history_count;
    int history_idx;
    char cwd[64];
    bool wifi_was_scanning;   /* for scan-completion announcement */
} term_state_t;

static void term_output(term_state_t *ts, const char *s)
{
    int slen = (int)strlen(s);
    if (ts->len + slen < TERM_BUF_SIZE - 1) {
        memcpy(&ts->buf[ts->len], s, slen);
        ts->len += slen;
        ts->buf[ts->len] = 0;
    }
}

static void term_outputln(term_state_t *ts, const char *s)
{
    term_output(ts, s);
    term_output(ts, "\n");
}

static void term_prompt(term_state_t *ts)
{
    char prompt[80];
    snprintf(prompt, sizeof(prompt), "%s> ", ts->cwd);
    term_output(ts, prompt);
}

static void term_clear(term_state_t *ts)
{
    ts->len = 0;
    ts->buf[0] = 0;
}

static void term_help(term_state_t *ts)
{
    term_outputln(ts, "Arpile 32CDF Terminal");
    term_outputln(ts, "Commands:");
    term_outputln(ts, "  help       - Show this help");
    term_outputln(ts, "  clear      - Clear screen");
    term_outputln(ts, "  echo <txt> - Print text");
    term_outputln(ts, "  ls         - List directory");
    term_outputln(ts, "  cd <dir>   - Change directory");
    term_outputln(ts, "  pwd        - Print working directory");
    term_outputln(ts, "  cat <file> - Display file");
    term_outputln(ts, "  mkdir <d>  - Create directory");
    term_outputln(ts, "  rm <file>  - Remove file");
    term_outputln(ts, "  cp <a> <b> - Copy file");
    term_outputln(ts, "  mv <a> <b> - Move/rename file");
    term_outputln(ts, "  apps       - List applications");
    term_outputln(ts, "  version    - Show version");
    term_outputln(ts, "  wifi       - Show WiFi status");
    term_outputln(ts, "  wifi scan  - Scan for networks (works disconnected)");
    term_outputln(ts, "  wifi list  - Show last scan results");
    term_outputln(ts, "  wifi forget- Forget saved WiFi network");
    term_outputln(ts, "  reboot     - Reboot device");
}

static void term_ls(term_state_t *ts)
{
    DIR *dir = opendir(ts->cwd);
    if (!dir) {
        term_outputln(ts, "Error: cannot open directory");
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char line[64];
        snprintf(line, sizeof(line), "  %.50s", ent->d_name);
        term_outputln(ts, line);
    }
    closedir(dir);
}

static void term_pwd(term_state_t *ts)
{
    term_outputln(ts, ts->cwd);
}

static void term_cd(term_state_t *ts, const char *arg)
{
    if (!arg || !*arg) {
        strcpy(ts->cwd, "/");
        return;
    }
    char new_path[128];
    if (arg[0] == '/') {
        snprintf(new_path, sizeof(new_path), "%s", arg);
    } else {
        snprintf(new_path, sizeof(new_path), "%s/%s", ts->cwd, arg);
    }
    DIR *dir = opendir(new_path);
    if (dir) {
        closedir(dir);
        strncpy(ts->cwd, new_path, sizeof(ts->cwd) - 1);
        ts->cwd[sizeof(ts->cwd) - 1] = 0;
    } else {
        term_outputln(ts, "Error: directory not found");
    }
}

static void term_cat(term_state_t *ts, const char *arg)
{
    if (!arg || !*arg) {
        term_outputln(ts, "Usage: cat <file>");
        return;
    }
    char path[128];
    if (arg[0] == '/') {
        snprintf(path, sizeof(path), "%s", arg);
    } else {
        snprintf(path, sizeof(path), "%s/%s", ts->cwd, arg);
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        term_outputln(ts, "Error: file not found");
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        term_output(ts, line);
    }
    fclose(f);
}

static void term_mkdir(term_state_t *ts, const char *arg)
{
    if (!arg || !*arg) {
        term_outputln(ts, "Usage: mkdir <dir>");
        return;
    }
    char path[128];
    if (arg[0] == '/') {
        snprintf(path, sizeof(path), "%s", arg);
    } else {
        snprintf(path, sizeof(path), "%s/%s", ts->cwd, arg);
    }
    if (mkdir(path, 0755) != 0) {
        term_outputln(ts, "Error: cannot create directory");
    }
}

static void term_rm(term_state_t *ts, const char *arg)
{
    if (!arg || !*arg) {
        term_outputln(ts, "Usage: rm <file>");
        return;
    }
    char path[128];
    if (arg[0] == '/') {
        snprintf(path, sizeof(path), "%s", arg);
    } else {
        snprintf(path, sizeof(path), "%s/%s", ts->cwd, arg);
    }
    if (unlink(path) != 0) {
        term_outputln(ts, "Error: cannot remove file");
    }
}

static void term_cp(term_state_t *ts, const char *arg1, const char *arg2)
{
    if (!arg1 || !arg2) {
        term_outputln(ts, "Usage: cp <src> <dst>");
        return;
    }
    char src_path[128], dst_path[128];
    if (arg1[0] == '/') {
        snprintf(src_path, sizeof(src_path), "%s", arg1);
    } else {
        snprintf(src_path, sizeof(src_path), "%s/%s", ts->cwd, arg1);
    }
    if (arg2[0] == '/') {
        snprintf(dst_path, sizeof(dst_path), "%s", arg2);
    } else {
        snprintf(dst_path, sizeof(dst_path), "%s/%s", ts->cwd, arg2);
    }
    FILE *src = fopen(src_path, "r");
    if (!src) {
        term_outputln(ts, "Error: source not found");
        return;
    }
    FILE *dst = fopen(dst_path, "w");
    if (!dst) {
        fclose(src);
        term_outputln(ts, "Error: cannot create destination");
        return;
    }
    char buf[128];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }
    fclose(src);
    fclose(dst);
}

static void term_mv(term_state_t *ts, const char *arg1, const char *arg2)
{
    if (!arg1 || !arg2) {
        term_outputln(ts, "Usage: mv <src> <dst>");
        return;
    }
    char src_path[128], dst_path[128];
    if (arg1[0] == '/') {
        snprintf(src_path, sizeof(src_path), "%s", arg1);
    } else {
        snprintf(src_path, sizeof(src_path), "%s/%s", ts->cwd, arg1);
    }
    if (arg2[0] == '/') {
        snprintf(dst_path, sizeof(dst_path), "%s", arg2);
    } else {
        snprintf(dst_path, sizeof(dst_path), "%s/%s", ts->cwd, arg2);
    }
    if (rename(src_path, dst_path) != 0) {
        term_outputln(ts, "Error: cannot move file");
    }
}

static void term_apps(term_state_t *ts)
{
    int count;
    const arpile_app_t **apps = arpile_app_list(&count);
    term_outputln(ts, "Registered applications:");
    for (int i = 0; i < count; i++) {
        char line[64];
        snprintf(line, sizeof(line), "  %.20s - %.30s", apps[i]->id, apps[i]->name);
        term_outputln(ts, line);
    }
}

static void term_version(term_state_t *ts)
{
    term_outputln(ts, "Arpile 32CDF v0.1");
    term_outputln(ts, "ESP32-P4 Nano");
    term_outputln(ts, "ESP-IDF v6.0");
}

static const char *term_wifi_auth_str(int8_t a)
{
    switch (a) {
    case 0:  return "open";
    case 1:  return "WEP";
    case 2:  return "WPA";
    case 3:  return "WPA2";
    case 4:  return "WPA/WPA2";
    case 5:  return "ent";
    case 6:  return "WPA3";
    case 7:  return "WPA2/3";
    default: return "?";
    }
}

/* wifi list - dump the shared AP model (same data the WiFi app renders). */
static void term_wifi_list(term_state_t *ts)
{
    char line[80];
    if (arpile_wifi_scan_in_progress()) {
        term_outputln(ts, "Scanning... (wifi list again in a moment)");
        return;
    }
    int count = arpile_wifi_get_ap_count();
    if (count <= 0) {
        term_outputln(ts, "No networks stored. Run: wifi scan");
        return;
    }
    snprintf(line, sizeof(line), "%d network(s):", count);
    term_outputln(ts, line);
    for (int i = 0; i < count; i++) {
        arpile_wifi_ap_t ap;
        if (!arpile_wifi_get_ap(i, &ap)) continue;
        snprintf(line, sizeof(line), "  %.24s %ddBm ch%d %s",
                 ap.ssid[0] ? ap.ssid : "(hidden)",
                 (int)ap.rssi, ap.channel, term_wifi_auth_str(ap.authmode));
        term_outputln(ts, line);
    }
}

static void term_wifi(term_state_t *ts, int argc, char **argv)
{
    char line[80];
    /* wifi connect <ssid> [password] */
    if (argc >= 3 && strcmp(argv[1], "connect") == 0) {
        const char *pass = (argc >= 4) ? argv[3] : "";
        esp_err_t r = arpile_wifi_connect(argv[2], pass);
        if (r == ESP_OK) {
            snprintf(line, sizeof(line), "Connecting to %s...", argv[2]);
        } else {
            snprintf(line, sizeof(line), "Connect failed: %s", esp_err_to_name(r));
        }
        term_outputln(ts, line);
        return;
    }
    /* wifi scan / wifi rawscan - start an async scan while disconnected. */
    if (argc >= 2 && (strcmp(argv[1], "scan") == 0 || strcmp(argv[1], "rawscan") == 0)) {
        if (arpile_wifi_scan_in_progress()) {
            term_outputln(ts, "Scan already running; use: wifi list");
            return;
        }
        esp_err_t r = arpile_wifi_start_scan();
        if (r == ESP_OK) {
            term_outputln(ts, "Scanning... wait a few seconds, then: wifi list");
        } else {
            snprintf(line, sizeof(line), "Scan start failed: %s", esp_err_to_name(r));
            term_outputln(ts, line);
        }
        return;
    }
    /* wifi list - show scan results */
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        term_wifi_list(ts);
        return;
    }
    /* wifi forget - erase the remembered network */
    if (argc >= 2 && strcmp(argv[1], "forget") == 0) {
        esp_err_t r = arpile_wifi_forget();
        term_outputln(ts, r == ESP_OK ? "Saved network forgotten."
                                      : "Forget failed.");
        return;
    }
    const char *ip = arpile_wifi_get_ip();
    if (arpile_wifi_is_connected()) {
        snprintf(line, sizeof(line), "Connected to %s (IP %s)",
                 arpile_wifi_get_connected_ssid(), ip[0] ? ip : "?");
        term_outputln(ts, line);
        if (arpile_wifi_has_saved() &&
            strcmp(arpile_wifi_get_saved_ssid(), arpile_wifi_get_connected_ssid()) == 0) {
            term_outputln(ts, "Network saved; will auto-connect at boot.");
        }
    } else if (arpile_wifi_get_state() == ARPILE_WIFI_CONNECTING) {
        term_outputln(ts, "Connecting...");
    } else {
        term_outputln(ts, "Not connected. Try: wifi connect <ssid> <password>");
    }
    if (arpile_wifi_has_saved()) {
        snprintf(line, sizeof(line), "Saved network: %s ('wifi forget' to clear)",
                 arpile_wifi_get_saved_ssid());
        term_outputln(ts, line);
    }
}

static void term_reboot(term_state_t *ts)
{
    term_outputln(ts, "Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void term_execute(term_state_t *ts, const char *cmd)
{
    char cmd_copy[TERM_LINE_MAX];
    strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = 0;

    char *argv[8];
    int argc = 0;
    char *p = cmd_copy;
    while (*p && argc < 8) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }

    if (argc == 0) {
        return;
    }

    if (strcmp(argv[0], "help") == 0) {
        term_help(ts);
    } else if (strcmp(argv[0], "clear") == 0) {
        term_clear(ts);
    } else if (strcmp(argv[0], "echo") == 0) {
        if (argc > 1) {
            term_outputln(ts, argv[1]);
        }
    } else if (strcmp(argv[0], "ls") == 0) {
        term_ls(ts);
    } else if (strcmp(argv[0], "cd") == 0) {
        term_cd(ts, argc > 1 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "pwd") == 0) {
        term_pwd(ts);
    } else if (strcmp(argv[0], "cat") == 0) {
        term_cat(ts, argc > 1 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "mkdir") == 0) {
        term_mkdir(ts, argc > 1 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "rm") == 0) {
        term_rm(ts, argc > 1 ? argv[1] : NULL);
    } else if (strcmp(argv[0], "cp") == 0) {
        term_cp(ts, argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL);
    } else if (strcmp(argv[0], "mv") == 0) {
        term_mv(ts, argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL);
    } else if (strcmp(argv[0], "apps") == 0) {
        term_apps(ts);
    } else if (strcmp(argv[0], "version") == 0) {
        term_version(ts);
    } else if (strcmp(argv[0], "wifi") == 0) {
        term_wifi(ts, argc, argv);
    } else if (strcmp(argv[0], "reboot") == 0) {
        term_reboot(ts);
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Unknown command: %s", argv[0]);
        term_outputln(ts, msg);
    }
}

static void term_init(arpile_app_ctx_t *ctx)
{
    term_state_t *ts = calloc(1, sizeof(term_state_t));
    ctx->user = ts;
    strcpy(ts->cwd, "/");
    term_help(ts);
    term_prompt(ts);
}

/* Announce scan completion in the terminal itself (no serial monitor needed).
 * arpile_wifi_poll() is driven by the central UI loop; we only watch for the
 * in-progress -> finished edge and print the outcome. */
static void term_update(arpile_app_ctx_t *ctx)
{
    term_state_t *ts = ctx->user;
    if (!ts) return;

    bool scanning = arpile_wifi_scan_in_progress();
    if (ts->wifi_was_scanning && !scanning) {
        int count = arpile_wifi_get_ap_count();
        char line[80];
        if (count > 0) {
            snprintf(line, sizeof(line), "Scan done: %d network(s). Use: wifi list",
                     count);
        } else {
            snprintf(line, sizeof(line), "Scan done: no networks (%s)",
                     arpile_wifi_get_scan_diag());
        }
        term_outputln(ts, line);
        term_prompt(ts);
        arpile_ui_win_redraw(ctx->win);
    }
    ts->wifi_was_scanning = scanning;
}

static void term_render(arpile_app_ctx_t *ctx, ui_win_t *win)
{
    term_state_t *ts = ctx->user;
    if (!ts) return;

    ili9488_t *lcd = arpile_ui_get_lcd();
    ui_rect_t c = win_client_rect(win);
    ui_draw_fill_rect(lcd, &c, UI_C_WIN_BG);

    int cols = c.w / CHAR_W;
    int rows = c.h / (CHAR_H + 2);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    /* Wrap buffer into lines */
    struct { int start, end; } lines[128];
    int nlines = 0;
    int buf_i = 0;
    while (buf_i < ts->len && nlines < 128) {
        int lstart = buf_i;
        while (buf_i < ts->len && ts->buf[buf_i] != '\n' &&
               (buf_i - lstart) < cols) {
            buf_i++;
        }
        if (buf_i < ts->len && ts->buf[buf_i] == '\n') {
            buf_i++;
        }
        lines[nlines].start = lstart;
        lines[nlines].end = buf_i;
        nlines++;
    }

    int display_start = nlines > rows ? nlines - rows : 0;
    int py = c.y + 4;
    for (int k = 0; k < rows; k++) {
        int li = display_start + k;
        if (li < nlines) {
            int len = lines[li].end - lines[li].start;
            char line[128];
            if (len > (int)sizeof(line) - 1) len = (int)sizeof(line) - 1;
            memcpy(line, &ts->buf[lines[li].start], len);
            line[len] = 0;
            ui_draw_text(lcd, (uint16_t)(c.x + 4), (uint16_t)py, line,
                         UI_C_TEXT, UI_C_WIN_BG);
        }
        py += CHAR_H + 2;
    }

    /* Cursor */
    int last_line = nlines - 1;
    if (last_line >= 0) {
        int line_start = lines[last_line].start;
        int line_end = lines[last_line].end;
        int line_len = line_end - line_start;
        int cursor_x = c.x + 4 + line_len * CHAR_W;
        int cursor_y = c.y + 4 + (last_line - display_start) * (CHAR_H + 2);
        ui_draw_fill_rect(lcd, &(ui_rect_t){ (uint16_t)cursor_x,
                          (uint16_t)cursor_y, (uint16_t)CHAR_W, (uint16_t)CHAR_H },
                          UI_C_ACCENT);
    }
}

static void term_event(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev)
{
    term_state_t *ts = ctx->user;
    if (!ts) return;

    if (ev->type == ARPILE_IN_EVENT_KEY_DOWN) {
        if (ev->key.ascii) {
            char ch = ev->key.ascii;
            if (ch == '\r') ch = '\n';
            if (ch == '\t') ch = ' ';

            if (ch == '\n') {
                /* Execute command */
                ts->line[ts->line_pos] = 0;
                term_outputln(ts, ts->line);

                /* Add to history */
                if (ts->history_count < TERM_HISTORY) {
                    strcpy(ts->history[ts->history_count], ts->line);
                    ts->history_count++;
                }
                ts->history_idx = ts->history_count;

                term_execute(ts, ts->line);
                ts->line_pos = 0;
                ts->line[0] = 0;
                term_prompt(ts);
            } else if (ch == 127 || ch == 8) {
                /* Backspace: remove the last character from both the input
                 * line and the rendered scroll buffer (the terminal renderer
                 * doesn't interpret '\b', so we delete the char directly). */
                if (ts->line_pos > 0) {
                    ts->line_pos--;
                    ts->line[ts->line_pos] = 0;
                    if (ts->len > 0) {
                        ts->len--;
                        ts->buf[ts->len] = 0;
                    }
                }
            } else if (ch >= 32 && ch < 127) {
                if (ts->line_pos < TERM_LINE_MAX - 1) {
                    ts->line[ts->line_pos++] = ch;
                    ts->line[ts->line_pos] = 0;
                    char s[2] = { ch, 0 };
                    term_output(ts, s);
                }
            }
            arpile_ui_win_redraw(ctx->win);
        }
    }
}

static void term_destroy(arpile_app_ctx_t *ctx)
{
    if (ctx->user) {
        free(ctx->user);
        ctx->user = NULL;
    }
}

static const arpile_app_ops_t term_ops = {
    .init = term_init,
    .update = term_update,
    .event = term_event,
    .render = term_render,
    .destroy = term_destroy,
};

const arpile_app_t arpile_app_terminal = {
    .id = "terminal",
    .name = "Terminal",
    .icon = "cmd",
    .ops = &term_ops,
};
