#include <stdio.h>
#include <string.h>
#include "arpile_app.h"
#include "freertos/portmacro.h"

static const arpile_app_t *s_apps[ARPILE_APP_MAX];
static int s_app_count = 0;

static arpile_app_ctx_t s_ctxs[ARPILE_APP_MAX];
static int s_ctx_count = 0;

/* Close requests may arrive from the USB HID task (Alt+Fx, launcher/X clicks),
 * whose 4 KB stack cannot survive app destroy() (FATFS, allocations). So
 * arpile_app_close() only *requests* a close; the heavy teardown runs on the
 * UI task inside arpile_app_poll(). A synchronous close here previously
 * overflowed the HID stack and killed all input after the first app close. */
static arpile_app_ctx_t *s_close_pending[ARPILE_APP_MAX];
static int s_close_pending_count = 0;
static portMUX_TYPE s_close_mux = portMUX_INITIALIZER_UNLOCKED;

void arpile_app_register(const arpile_app_t *app)
{
    if (s_app_count >= ARPILE_APP_MAX) {
        return;
    }
    s_apps[s_app_count++] = app;
}

const arpile_app_t **arpile_app_list(int *count)
{
    if (count) {
        *count = s_app_count;
    }
    return s_apps;
}

static arpile_app_ctx_t *find_ctx_by_win(ui_win_t *win)
{
    for (int i = 0; i < s_ctx_count; i++) {
        if (s_ctxs[i].win == win) {
            return &s_ctxs[i];
        }
    }
    return NULL;
}

static void app_on_paint(ui_win_t *win)
{
    arpile_app_ctx_t *ctx = find_ctx_by_win(win);
    if (ctx && ctx->app->ops->render) {
        ctx->app->ops->render(ctx, win);
    }
}

static void app_on_key(ui_win_t *win, const arpile_input_event_t *ev)
{
    arpile_app_ctx_t *ctx = find_ctx_by_win(win);
    if (ctx && ctx->app->ops->event) {
        ctx->app->ops->event(ctx, ev);
    }
}

static void app_on_mouse(ui_win_t *win, const arpile_input_event_t *ev)
{
    arpile_app_ctx_t *ctx = find_ctx_by_win(win);
    if (ctx && ctx->app->ops->event) {
        ctx->app->ops->event(ctx, ev);
    }
}

static void app_on_close(ui_win_t *win)
{
    arpile_app_ctx_t *ctx = find_ctx_by_win(win);
    if (ctx) {
        arpile_app_close(ctx);
    }
}

void arpile_app_close_win(ui_win_t *win)
{
    arpile_app_ctx_t *ctx = find_ctx_by_win(win);
    if (ctx) {
        arpile_app_close(ctx);
    }
}

arpile_app_ctx_t *arpile_app_launch(const char *app_id)
{
    const arpile_app_t *app = NULL;
    for (int i = 0; i < s_app_count; i++) {
        if (strcmp(s_apps[i]->id, app_id) == 0) {
            app = s_apps[i];
            break;
        }
    }
    if (!app) {
        printf("[APP] launch '%s' -> not registered\r\n", app_id);
        return NULL;
    }

    if (s_ctx_count >= ARPILE_APP_MAX) {
        printf("[APP] launch '%s' -> ctx table full (%d)\r\n", app_id, ARPILE_APP_MAX);
        return NULL;
    }

    arpile_app_ctx_t *ctx = &s_ctxs[s_ctx_count];
    memset(ctx, 0, sizeof(arpile_app_ctx_t));
    ctx->app = app;
    ctx->running = true;
    /* Launches arrive from the USB HID task (4 KB stack). Defer ops->init
     * to arpile_app_poll() on the UI task: FATFS and heavier init would
     * overflow the HID stack (apps silently died right after launch). */
    ctx->needs_init = true;

    ui_win_ops_t ops = { 0 };
    ops.on_paint = app_on_paint;
    ops.on_key = app_on_key;
    ops.on_mouse = app_on_mouse;
    ops.on_close = app_on_close;

    ctx->win = arpile_ui_win_new(app->name, 14, 20,
                                  UI_W - 28, UI_H - UI_PANEL_H - 44,
                                  &ops, ctx);
    if (!ctx->win) {
        return NULL;
    }

    s_ctx_count++;

    arpile_ui_win_focus(ctx->win);
    return ctx;
}

void arpile_app_poll(void)
{
    /* Drain deferred close requests first (runs on the UI task, not the HID
     * task, so destroy() is stack-safe). */
    arpile_app_ctx_t *batch[ARPILE_APP_MAX];
    int n;
    portENTER_CRITICAL(&s_close_mux);
    n = s_close_pending_count;
    for (int i = 0; i < n; i++) {
        batch[i] = s_close_pending[i];
    }
    s_close_pending_count = 0;
    portEXIT_CRITICAL(&s_close_mux);

    for (int k = 0; k < n; k++) {
        arpile_app_ctx_t *ctx = batch[k];
        if (ctx->app->ops->destroy) {
            ctx->app->ops->destroy(ctx);
        }
        /* Free the window slot (UI task only). */
        ui_win_t *win = ctx->win;
        ctx->win = NULL;
        arpile_ui_win_teardown(win);
        /* Remove from the registry array. */
        for (int i = 0; i < s_ctx_count; i++) {
            if (&s_ctxs[i] == ctx) {
                for (int j = i; j < s_ctx_count - 1; j++) {
                    s_ctxs[j] = s_ctxs[j + 1];
                }
                s_ctx_count--;
                break;
            }
        }
    }

    for (int i = 0; i < s_ctx_count; i++) {
        arpile_app_ctx_t *ctx = &s_ctxs[i];
        if (!ctx->running) {
            continue;
        }
        if (ctx->needs_init) {
            ctx->needs_init = false;
            if (ctx->app->ops->init) {
                ctx->app->ops->init(ctx);
            }
            /* init may have painted via win_redraw; ensure a fresh frame */
            if (ctx->win) {
                arpile_ui_win_redraw(ctx->win);
            }
            continue;
        }
        if (ctx->app->ops->update) {
            ctx->app->ops->update(ctx);
        }
    }
}

void arpile_app_close(arpile_app_ctx_t *ctx)
{
    if (!ctx || !ctx->running) {
        return;
    }
    ctx->running = false;

    /* Only enqueue; the actual destroy() + window teardown happens later in
     * arpile_app_poll() on the UI task. This keeps the HID task (4 KB stack)
     * free of any heavy teardown and restores repeated app launches without a
     * reboot. */
    portENTER_CRITICAL(&s_close_mux);
    if (s_close_pending_count < ARPILE_APP_MAX) {
        s_close_pending[s_close_pending_count++] = ctx;
    }
    portEXIT_CRITICAL(&s_close_mux);
}
