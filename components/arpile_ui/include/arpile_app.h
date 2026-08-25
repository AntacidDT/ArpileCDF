/* Arpile application framework */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "arpile_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARPILE_APP_MAX 24
#define ARPILE_APP_NAME_LEN 16

typedef struct arpile_app arpile_app_t;
typedef struct arpile_app_ctx arpile_app_ctx_t;

/* Application lifecycle callbacks */
typedef struct {
    void (*init)(arpile_app_ctx_t *ctx);
    void (*update)(arpile_app_ctx_t *ctx);
    void (*event)(arpile_app_ctx_t *ctx, const arpile_input_event_t *ev);
    void (*render)(arpile_app_ctx_t *ctx, ui_win_t *win);
    void (*destroy)(arpile_app_ctx_t *ctx);
} arpile_app_ops_t;

/* Application descriptor (static, registered at startup) */
struct arpile_app {
    const char *id;
    const char *name;
    const char *icon;
    const arpile_app_ops_t *ops;
};

/* Runtime application context (per-instance) */
struct arpile_app_ctx {
    const arpile_app_t *app;
    ui_win_t *win;
    void *user;
    bool running;
    bool needs_init;   /* deferred: init runs on the UI task, not the HID task */
};

/* Register an application (call once at startup) */
void arpile_app_register(const arpile_app_t *app);

/* Launch an application by ID */
arpile_app_ctx_t *arpile_app_launch(const char *app_id);

/* Close an application */
void arpile_app_close(arpile_app_ctx_t *ctx);

/* Poll all running applications (call periodically from UI loop) */
void arpile_app_poll(void);

/* Get list of registered apps (for launcher) */
const arpile_app_t **arpile_app_list(int *count);

#ifdef __cplusplus
}
#endif
