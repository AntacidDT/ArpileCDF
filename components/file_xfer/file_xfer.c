#include "file_xfer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_rom_crc.h"

#define XF_UART       UART_NUM_0
#define XF_BAUD_CMD   115200
#define XF_BAUD_FAST  921600
#define XF_RX_BUF     (10 * 1024)
#define XF_BLK        8192

typedef enum { XF_CH_NONE = -1, XF_CH_UART = 0, XF_CH_USJ = 1 } xf_ch_t;

static uint8_t *s_blk;   /* heap-allocated XF_BLK buffer (saves BSS) */
static bool s_busy;
static bool s_usj;
static xf_ch_t s_ch = XF_CH_NONE;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* reads one '\n'-terminated line (without it); -1 timeout, -2 overflow.
   polls both transports; the channel that delivers the first byte wins. */
static int xf_read_line(char *buf, int cap, uint32_t timeout_ms)
{
    int n = 0;
    uint32_t deadline = 0;
    s_ch = XF_CH_NONE;
    for (;;) {
        uint8_t c = 0;
        bool got = false;
        if (s_ch != XF_CH_USJ &&
            uart_read_bytes(XF_UART, &c, 1, 0) == 1) {
            if (s_ch == XF_CH_NONE) s_ch = XF_CH_UART;
            got = true;
        } else if (s_usj && s_ch != XF_CH_UART &&
                   usb_serial_jtag_read_bytes(&c, 1, 0) == 1) {
            if (s_ch == XF_CH_NONE) s_ch = XF_CH_USJ;
            got = true;
        }
        if (!got) {
            vTaskDelay(pdMS_TO_TICKS(5));
            deadline += 5;
            if (deadline >= timeout_ms) return -1;
            continue;
        }
        deadline = 0;
        if (c == '\n') {
            if (n && buf[n - 1] == '\r') n--;
            buf[n] = '\0';
            return n;
        }
        if (n >= cap - 1) return -2;
        buf[n++] = (char)c;
    }
}

static void xf_send(const char *s)
{
    if (s_ch == XF_CH_USJ) {
        usb_serial_jtag_write_bytes(s, strlen(s), pdMS_TO_TICKS(200));
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));
    } else {
        uart_write_bytes(XF_UART, s, strlen(s));
        uart_wait_tx_done(XF_UART, pdMS_TO_TICKS(100));
    }
}

static bool xf_name_ok(const char *name)
{
    size_t l = strlen(name);
    if (!l || l > 40 || name[0] == '.') return false;
    for (size_t i = 0; i < l; i++) {
        char c = name[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return strstr(name, "..") == NULL;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void xf_cmd_ls(void)
{
    DIR *d = opendir("/sdcard");
    if (!d) {
        xf_send("ERR\n");
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char full[96];
        struct stat st;
        snprintf(full, sizeof(full), "/sdcard/%.40s", e->d_name);
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        char line[96];
        snprintf(line, sizeof(line), "%ld %.60s\n", (long)st.st_size,
                 e->d_name);
        xf_send(line);
    }
    closedir(d);
    xf_send("END\n");
}

static void xf_cmd_del(const char *name)
{
    if (!xf_name_ok(name)) {
        xf_send("ERR\n");
        return;
    }
    char full[80];
    snprintf(full, sizeof(full), "/sdcard/%.40s", name);
    unlink(full);
    xf_send("OK\n");
}

static void xf_cmd_put(const char *name, long size, uint32_t want_crc)
{
    if (!xf_name_ok(name) || size <= 0 ||
        size > 512L * 1024 * 1024) {
        xf_send("ERR bad args\n");
        return;
    }

    xf_send("RDY\n");

    /* optional baud upgrade before the binary phase (UART path only) */
    uint32_t baud = XF_BAUD_CMD;
    char line[64];
    int n = xf_read_line(line, sizeof(line), 300);
    unsigned req = 0;
    if (n > 0 && s_ch == XF_CH_UART &&
        sscanf(line, "ARPFILE baud %u", &req) == 1 &&
        req >= XF_BAUD_CMD && req <= 4000000) {
        xf_send("BAUD\n");
        vTaskDelay(pdMS_TO_TICKS(20));
        uart_flush_input(XF_UART);
        uart_set_baudrate(XF_UART, req);
        baud = req;
    } else if (n > 0) {
        /* TEMP diag */
        char dbg[96];
        snprintf(dbg, sizeof(dbg), "SKIP n=%d ch=%d l=%.44s\n",
                 n, (int)s_ch, line);
        xf_send(dbg);
    } else {
        xf_send("TO\n");   /* TEMP diag */
    }

    s_busy = true;
    esp_log_level_set("*", ESP_LOG_NONE);

    char part[64];
    snprintf(part, sizeof(part), "/sdcard/%.40s.part", name);
    FILE *f = fopen(part, "wb");
    long got_total = 0;
    uint32_t crc = 0;
    size_t fill = 0;
    bool fail = f == NULL;

    while (!fail && got_total < size) {
        size_t want = XF_BLK - fill;
        long left = size - got_total;
        if ((long)want > left) want = (size_t)left;
        int r = (s_ch == XF_CH_USJ)
            ? usb_serial_jtag_read_bytes(s_blk + fill, want,
                                         pdMS_TO_TICKS(10000))
            : uart_read_bytes(XF_UART, s_blk + fill, want,
                              pdMS_TO_TICKS(10000));
        if (r <= 0) {
            fail = true;
            break;
        }
        crc = esp_rom_crc32_le(crc, s_blk + fill, (size_t)r);
        fill += (size_t)r;
        got_total += r;
        if (fill == XF_BLK) {
            if (fwrite(s_blk, 1, fill, f) != fill) {
                fail = true;
                break;
            }
            fill = 0;
            xf_send("K");
        }
    }

    bool ok = false;
    if (f) {
        if (!fail && fill > 0 && fwrite(s_blk, 1, fill, f) == fill)
            ;
        else if (!fail && fill > 0)
            fail = true;
        fclose(f);
    }

    if (!fail && got_total == size && crc == want_crc) {
        char finalp[64];
        snprintf(finalp, sizeof(finalp), "/sdcard/%.40s", name);
        unlink(finalp);
        ok = rename(part, finalp) == 0;
    } else {
        if (f == NULL || fail) { /* fall through */ }
        unlink(part);
    }

    if (baud != XF_BAUD_CMD) uart_set_baudrate(XF_UART, XF_BAUD_CMD);
    vTaskDelay(pdMS_TO_TICKS(30));

    if (ok) {
        char msg[48];
        snprintf(msg, sizeof(msg), "OK %08lx\n", (unsigned long)crc);
        xf_send(msg);
    } else {
        xf_send("BAD\n");
    }

    esp_log_level_set("*", ESP_LOG_INFO);
    s_busy = false;
    uart_flush_input(XF_UART);
}

/* ------------------------------------------------------------------ */
/* listener                                                            */
/* ------------------------------------------------------------------ */

static void xf_task(void *arg)
{
    (void)arg;
    s_blk = malloc(XF_BLK);
    if (!s_blk) {
        printf("[XFER] no block buffer\n");
        vTaskDelete(NULL);
        return;
    }

    uart_config_t cfg = {
        .baud_rate = XF_BAUD_CMD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(XF_UART, XF_RX_BUF, 1024, 0, NULL, 0);
    uart_param_config(XF_UART, &cfg);
    uart_set_pin(XF_UART, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (usb_serial_jtag_driver_install(
            &(usb_serial_jtag_driver_config_t){
                .tx_buffer_size = 512,
                .rx_buffer_size = 8192 }) == ESP_OK)
        s_usj = true;

    /* heartbeat straight onto each transport (no console dependency):
     * proves the listener is alive and reveals the live serial path */
    for (int i = 0; i < 8; i++) {
        uart_write_bytes(XF_UART, "[XFER]U", 7);
        if (s_usj)
            usb_serial_jtag_write_bytes("[XFER]J", 7,
                                        pdMS_TO_TICKS(50));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("[XFER] ready (uart%s)\n", s_usj ? "+usj" : "");

    char line[72];
    for (;;) {
        int n = xf_read_line(line, sizeof(line), 600000);
        if (n <= 0) continue;
        if (s_busy) continue;

        if (!strncmp(line, "ARPFILE put ", 12)) {
            char name[48] = "";
            long size = 0;
            unsigned crc = 0;
            if (sscanf(line + 12, "%47s %ld %x", name, &size, &crc) == 3)
                xf_cmd_put(name, size, crc);
            else
                xf_send("ERR parse\n");
        } else if (!strcmp(line, "ARPFILE ls")) {
            xf_cmd_ls();
        } else if (!strncmp(line, "ARPFILE del ", 12)) {
            xf_cmd_del(line + 12);
        }
    }
}

void arpile_file_xfer_start(void)
{
    if (xTaskCreate(xf_task, "file_xfer", 8192, NULL, 4, NULL) != pdPASS)
        printf("[XFER] task spawn FAILED\n");
}
